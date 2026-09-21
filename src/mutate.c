#include "minisql.h"



void cmd_insert(struct TableDef tables[], int count, char *sql)
{
    char name[MAX_NAME + 1];
    char vals[MAX_COLS][MAX_VALUE + 1];
    char *p;
    char *q;
    int idx;
    int val_count = 0;
    int i;
    struct RowSet rows;
    p = sql + strlen("INSERT INTO");
    p = ltrim(p);
    i = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (i < MAX_NAME) {
            name[i++] = (char)toupper((unsigned char)*p);
        }
        p++;
    }
    name[i] = '\0';
    idx = find_table(tables, count, name);
    if (idx < 0) {
        printf("ERR TABLE NOT FOUND\n");
        return;
    }
    p = find_i(p, "VALUES");
    if (p == NULL) {
        printf("ERR USAGE: INSERT INTO name VALUES (...)\n");
        return;
    }
    p = strchr(p, '(');
    q = strrchr(sql, ')');
    if (p == NULL || q == NULL || q <= p) {
        printf("ERR BAD VALUES\n");
        return;
    }
    *q = '\0';
    if (!parse_csv(p + 1, vals, &val_count) ||
        val_count != tables[idx].col_count) {
        printf("ERR VALUE COUNT\n");
        return;
    }
    for (i = 0; i < val_count; i++) {
        if (!validate_value(&tables[idx], i, vals[i])) {
            return;
        }
    }
    if (!load_rows(&tables[idx], &rows)) {
        printf("ERR CANNOT READ TABLE\n");
        return;
    }
    if (tables[idx].pk_col >= 0) {
        if (vals[tables[idx].pk_col][0] == '\0') {
            rowset_free(&rows);
            printf("ERR PRIMARY KEY REQUIRED\n");
            return;
        }
        if (find_pk_duplicate(&tables[idx], rows.rows, rows.count,
                              vals[tables[idx].pk_col], -1) >= 0) {
            rowset_free(&rows);
            printf("ERR DUPLICATE PRIMARY KEY\n");
            return;
        }
    }
    if (!check_foreign_keys(tables, count, &tables[idx], vals)) {
        rowset_free(&rows);
        return;
    }
    if (!rowset_reserve(&rows, rows.count + 1)) {
        rowset_free(&rows);
        printf("ERR OUT OF MEMORY\n");
        return;
    }
    for (i = 0; i < val_count; i++) {
        strncpy(rows.rows[rows.count].values[i], vals[i], MAX_VALUE);
        rows.rows[rows.count].values[i][MAX_VALUE] = '\0';
    }
    rows.count++;
    if (!save_rows(&tables[idx], rows.rows, rows.count)) {
        rowset_free(&rows);
        printf("ERR CANNOT WRITE TABLE\n");
        return;
    }
    rowset_free(&rows);
    printf("OK 1 ROW INSERTED\n");
}

void cmd_delete(struct TableDef tables[], int count, char *sql)
{
    char name[MAX_NAME + 1];
    char *p;
    char *where;
    int idx;
    int i = 0;
    int r;
    int deleted = 0;
    struct WhereExpr expr;
    struct RowSet rows;
    struct RowSet out;
    p = sql + strlen("DELETE FROM");
    p = ltrim(p);
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (i < MAX_NAME) {
            name[i++] = (char)toupper((unsigned char)*p);
        }
        p++;
    }
    name[i] = '\0';
    idx = find_table(tables, count, name);
    if (idx < 0) {
        printf("ERR TABLE NOT FOUND\n");
        return;
    }
    where = find_i(p, "WHERE");
    if (!parse_where(&tables[idx], where, &expr)) {
        printf("ERR BAD WHERE\n");
        return;
    }
    if (!load_rows(&tables[idx], &rows)) {
        printf("ERR CANNOT READ TABLE\n");
        return;
    }
    rowset_init(&out);
    for (r = 0; r < rows.count; r++) {
        if (where_match(&tables[idx], &rows.rows[r], &expr)) {
            if (tables[idx].pk_col >= 0 &&
                row_is_referenced(tables, count, &tables[idx],
                                  rows.rows[r].values[tables[idx].pk_col])) {
                rowset_free(&rows);
                rowset_free(&out);
                printf("ERR ROW REFERENCED\n");
                return;
            }
            deleted++;
        } else {
            if (!rowset_add(&out, &rows.rows[r])) {
                rowset_free(&rows);
                rowset_free(&out);
                printf("ERR OUT OF MEMORY\n");
                return;
            }
        }
    }
    if (!save_rows(&tables[idx], out.rows, out.count)) {
        rowset_free(&rows);
        rowset_free(&out);
        printf("ERR CANNOT WRITE TABLE\n");
        return;
    }
    rowset_free(&rows);
    rowset_free(&out);
    printf("OK %d ROWS DELETED\n", deleted);
}

void cmd_update(struct TableDef tables[], int count, char *sql)
{
    char name[MAX_NAME + 1];
    char *p;
    char *setp;
    char *where;
    char *eq;
    char set_col_name[MAX_VALUE + 1];
    char set_val[MAX_VALUE + 1];
    struct WhereExpr expr;
    int set_col;
    int idx;
    int i = 0;
    int r;
    int changed = 0;
    struct RowSet rows;
    p = sql + strlen("UPDATE");
    p = ltrim(p);
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (i < MAX_NAME) {
            name[i++] = (char)toupper((unsigned char)*p);
        }
        p++;
    }
    name[i] = '\0';
    idx = find_table(tables, count, name);
    if (idx < 0) {
        printf("ERR TABLE NOT FOUND\n");
        return;
    }
    setp = find_i(p, "SET");
    if (setp == NULL) {
        printf("ERR USAGE: UPDATE name SET col=value WHERE col=value\n");
        return;
    }
    setp += strlen("SET");
    where = find_i(setp, "WHERE");
    if (where != NULL) {
        char *where_keyword = where;
        *where_keyword = '\0';
        where = where_keyword + strlen("WHERE");
    }
    eq = strchr(setp, '=');
    if (eq == NULL) {
        printf("ERR BAD SET\n");
        return;
    }
    *eq = '\0';
    strncpy(set_col_name, setp, MAX_VALUE);
    set_col_name[MAX_VALUE] = '\0';
    clean_token(set_col_name);
    strncpy(set_val, eq + 1, MAX_VALUE);
    set_val[MAX_VALUE] = '\0';
    clean_token(set_val);
    set_col = find_col(&tables[idx], set_col_name);
    if (set_col < 0) {
        printf("ERR BAD SET COLUMN\n");
        return;
    }
    if (tables[idx].pk_col == set_col) {
        printf("ERR CANNOT UPDATE PRIMARY KEY\n");
        return;
    }
    if (!validate_value(&tables[idx], set_col, set_val)) {
        return;
    }
    if (!parse_where(&tables[idx], where, &expr)) {
        printf("ERR BAD WHERE\n");
        return;
    }
    if (!load_rows(&tables[idx], &rows)) {
        printf("ERR CANNOT READ TABLE\n");
        return;
    }
    for (r = 0; r < rows.count; r++) {
        if (where_match(&tables[idx], &rows.rows[r], &expr)) {
            strncpy(rows.rows[r].values[set_col], set_val, MAX_VALUE);
            rows.rows[r].values[set_col][MAX_VALUE] = '\0';
            if (!check_foreign_keys(tables, count, &tables[idx],
                                    rows.rows[r].values)) {
                rowset_free(&rows);
                return;
            }
            changed++;
        }
    }
    if (!save_rows(&tables[idx], rows.rows, rows.count)) {
        rowset_free(&rows);
        printf("ERR CANNOT WRITE TABLE\n");
        return;
    }
    rowset_free(&rows);
    printf("OK %d ROWS UPDATED\n", changed);
}

