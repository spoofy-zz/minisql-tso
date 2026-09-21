#include "minisql.h"



void cmd_tables(struct TableDef tables[], int count)
{
    int i;
    if (count == 0) {
        printf("NO TABLES\n");
        return;
    }
    for (i = 0; i < count; i++) {
        printf("%s\n", tables[i].name);
    }
}

void cmd_schema(struct TableDef tables[], int count, char *sql)
{
    char name[MAX_NAME + 1];
    int idx;
    int c;

    if (!parse_name_after(sql, ".SCHEMA", name)) {
        printf("ERR USAGE: .SCHEMA table\n");
        return;
    }
    idx = find_table(tables, count, name);
    if (idx < 0) {
        printf("ERR TABLE NOT FOUND\n");
        return;
    }
    printf("%s(", tables[idx].name);
    for (c = 0; c < tables[idx].col_count; c++) {
        char typ[MAX_VALUE + 1];

        if (c > 0) {
            printf(", ");
        }
        type_to_text(tables[idx].col_types[c], tables[idx].col_lens[c],
                     typ, sizeof(typ));
        printf("%s %s", tables[idx].cols[c], typ);
        if (tables[idx].pk_col == c) {
            printf(" PRIMARY KEY");
        }
    }
    printf(")\n");
    for (c = 0; c < tables[idx].index_count; c++) {
        printf("INDEX %s ON %s(%s)\n", tables[idx].index_names[c],
               tables[idx].name, tables[idx].cols[tables[idx].index_cols[c]]);
    }
    for (c = 0; c < tables[idx].fk_count; c++) {
        printf("FOREIGN KEY %s REFERENCES %s(%s)\n",
               tables[idx].cols[tables[idx].fk_cols[c]],
               tables[idx].fk_tables[c], tables[idx].fk_ref_cols[c]);
    }
}

void cmd_desc(struct TableDef tables[], int count, char *sql)
{
    char name[MAX_NAME + 1];
    char *prefix;
    int idx;
    int c;

    prefix = starts_i(sql, "DESCRIBE") ? "DESCRIBE" : "DESC";
    if (!parse_name_after(sql, prefix, name)) {
        printf("ERR USAGE: DESC table\n");
        return;
    }
    idx = find_table(tables, count, name);
    if (idx < 0) {
        printf("ERR TABLE NOT FOUND\n");
        return;
    }
    printf("FIELD | TYPE | KEY | REF\n");
    for (c = 0; c < tables[idx].col_count; c++) {
        char typ[MAX_VALUE + 1];
        char key[MAX_VALUE + 1];
        char ref[MAX_NAME * 2 + 4];
        int i;

        type_to_text(tables[idx].col_types[c], tables[idx].col_lens[c],
                     typ, sizeof(typ));
        key[0] = '\0';
        ref[0] = '\0';
        if (tables[idx].pk_col == c) {
            strcat(key, "PRI");
        }
        if (find_index_col(&tables[idx], c) >= 0) {
            if (key[0] != '\0') {
                strcat(key, ",");
            }
            strcat(key, "MUL");
        }
        for (i = 0; i < tables[idx].fk_count; i++) {
            if (tables[idx].fk_cols[i] != c) {
                continue;
            }
            if (key[0] != '\0') {
                strcat(key, ",");
            }
            strcat(key, "FK");
            sprintf(ref, "%s(%s)", tables[idx].fk_tables[i],
                    tables[idx].fk_ref_cols[i]);
            break;
        }
        printf("%s | %s | %s | %s\n", tables[idx].cols[c], typ, key, ref);
    }
    printf("OK %d COLUMNS\n", tables[idx].col_count);
}

void cmd_create(struct TableDef tables[], int *count, char *sql)
{
    char *p;
    char *q;
    char name[MAX_NAME + 1];
    char vals[MAX_DEFS][MAX_DEF_TEXT + 1];
    char pk_name[MAX_NAME + 1];
    int col_count = 0;
    int actual_cols = 0;
    int pk_col = -1;
    int fk_cols[MAX_FKS];
    char fk_col_names[MAX_FKS][MAX_NAME + 1];
    char fk_tables[MAX_FKS][MAX_NAME + 1];
    char fk_ref_cols[MAX_FKS][MAX_NAME + 1];
    int fk_count = 0;
    int i;

    p = sql + strlen("CREATE TABLE");
    p = ltrim(p);
    i = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (i < MAX_NAME) {
            name[i++] = (char)toupper((unsigned char)*p);
        }
        p++;
    }
    name[i] = '\0';
    if (!valid_name(name)) {
        printf("ERR BAD TABLE NAME\n");
        return;
    }
    if (find_table(tables, *count, name) >= 0) {
        printf("ERR TABLE EXISTS\n");
        return;
    }
    p = strchr(p, '(');
    q = strrchr(sql, ')');
    if (p == NULL || q == NULL || q <= p + 1) {
        printf("ERR USAGE: CREATE TABLE name (col,...)\n");
        return;
    }
    *q = '\0';
    if (!parse_def_csv(p + 1, vals, &col_count) || col_count < 1) {
        printf("ERR BAD COLUMN LIST\n");
        return;
    }
    if (*count >= MAX_TABLES) {
        printf("ERR TOO MANY TABLES\n");
        return;
    }

    pk_name[0] = '\0';
    for (i = 0; i < col_count; i++) {
        char *pk;
        char col_name[MAX_NAME + 1];
        int col_type;
        int col_len;

        if (starts_i(vals[i], "PRIMARY KEY")) {
            char *open = strchr(vals[i], '(');
            char *close = strrchr(vals[i], ')');
            if (open == NULL || close == NULL || close <= open + 1 ||
                pk_name[0] != '\0') {
                printf("ERR BAD PRIMARY KEY\n");
                return;
            }
            *close = '\0';
            strncpy(pk_name, open + 1, MAX_NAME);
            pk_name[MAX_NAME] = '\0';
            clean_token(pk_name);
            continue;
        }
        if (starts_i(vals[i], "FOREIGN KEY")) {
            char *open = strchr(vals[i], '(');
            char *close = strchr(vals[i], ')');
            char *ref;
            char *ropen;
            char *rclose;
            char fk_col_name[MAX_NAME + 1];

            if (fk_count >= MAX_FKS || open == NULL || close == NULL ||
                close <= open + 1) {
                printf("ERR BAD FOREIGN KEY\n");
                return;
            }
            *close = '\0';
            strncpy(fk_col_name, open + 1, MAX_NAME);
            fk_col_name[MAX_NAME] = '\0';
            clean_token(fk_col_name);
            ref = find_i(close + 1, "REFERENCES");
            if (ref == NULL) {
                printf("ERR BAD FOREIGN KEY\n");
                return;
            }
            ref += strlen("REFERENCES");
            ref = ltrim(ref);
            q = ref;
            while (*q && (isalnum((unsigned char)*q) || *q == '_')) {
                q++;
            }
            if (q - ref > MAX_NAME) {
                printf("ERR BAD FOREIGN KEY\n");
                return;
            }
            strncpy(fk_tables[fk_count], ref, q - ref);
            fk_tables[fk_count][q - ref] = '\0';
            clean_token(fk_tables[fk_count]);
            ropen = strchr(q, '(');
            rclose = strrchr(q, ')');
            if (ropen == NULL || rclose == NULL || rclose <= ropen + 1) {
                printf("ERR BAD FOREIGN KEY\n");
                return;
            }
            *rclose = '\0';
            strncpy(fk_ref_cols[fk_count], ropen + 1, MAX_NAME);
            fk_ref_cols[fk_count][MAX_NAME] = '\0';
            clean_token(fk_ref_cols[fk_count]);
            fk_cols[fk_count] = -1;
            if (!valid_name(fk_col_name) ||
                !valid_name(fk_tables[fk_count]) ||
                !valid_name(fk_ref_cols[fk_count])) {
                printf("ERR BAD FOREIGN KEY\n");
                return;
            }
            strncpy(fk_col_names[fk_count], fk_col_name, MAX_NAME);
            fk_col_names[fk_count][MAX_NAME] = '\0';
            fk_count++;
            continue;
        }

        pk = find_i(vals[i], "PRIMARY KEY");
        if (pk != NULL) {
            char tmp_name[MAX_NAME + 1];
            int tmp_type;
            int tmp_len;

            if (pk_name[0] != '\0') {
                printf("ERR BAD PRIMARY KEY\n");
                return;
            }
            *pk = '\0';
            if (!parse_column_def(vals[i], tmp_name, &tmp_type, &tmp_len)) {
                printf("ERR BAD PRIMARY KEY\n");
                return;
            }
            strncpy(pk_name, tmp_name, MAX_NAME);
            pk_name[MAX_NAME] = '\0';
        }

        if (!parse_column_def(vals[i], col_name, &col_type, &col_len)) {
            printf("ERR BAD COLUMN NAME\n");
            return;
        }
        if (actual_cols >= MAX_COLS) {
            printf("ERR TOO MANY COLUMNS\n");
            return;
        }
        strncpy(tables[*count].cols[actual_cols], col_name, MAX_NAME);
        tables[*count].cols[actual_cols][MAX_NAME] = '\0';
        tables[*count].col_types[actual_cols] = col_type;
        tables[*count].col_lens[actual_cols] = col_len;
        if (pk_name[0] != '\0' &&
            eqi(tables[*count].cols[actual_cols], pk_name)) {
            pk_col = actual_cols;
        }
        actual_cols++;
    }
    if (pk_name[0] != '\0' && pk_col < 0) {
        for (i = 0; i < actual_cols; i++) {
            if (eqi(tables[*count].cols[i], pk_name)) {
                pk_col = i;
                break;
            }
        }
    }
    if (actual_cols < 1) {
        printf("ERR BAD COLUMN LIST\n");
        return;
    }
    if (pk_name[0] != '\0' && pk_col < 0) {
        printf("ERR BAD PRIMARY KEY\n");
        return;
    }
    for (i = 0; i < fk_count; i++) {
        int parent;
        int ref_col;
        int c2;

        fk_cols[i] = -1;
        for (c2 = 0; c2 < actual_cols; c2++) {
            if (eqi(tables[*count].cols[c2], fk_col_names[i])) {
                fk_cols[i] = c2;
                break;
            }
        }
        if (fk_cols[i] < 0) {
            printf("ERR BAD FOREIGN KEY\n");
            return;
        }
        parent = find_table(tables, *count, fk_tables[i]);
        if (parent < 0) {
            printf("ERR FOREIGN TABLE NOT FOUND\n");
            return;
        }
        ref_col = find_col(&tables[parent], fk_ref_cols[i]);
        if (ref_col < 0 || tables[parent].pk_col != ref_col) {
            printf("ERR FOREIGN KEY NOT PRIMARY\n");
            return;
        }
        if (tables[*count].col_types[fk_cols[i]] !=
            tables[parent].col_types[ref_col]) {
            printf("ERR FOREIGN KEY TYPE\n");
            return;
        }
    }

    strncpy(tables[*count].name, name, MAX_NAME);
    tables[*count].name[MAX_NAME] = '\0';
    tables[*count].col_count = actual_cols;
    tables[*count].pk_col = pk_col;
    tables[*count].index_count = 0;
    tables[*count].fk_count = fk_count;
    for (i = 0; i < fk_count; i++) {
        tables[*count].fk_cols[i] = fk_cols[i];
        strncpy(tables[*count].fk_tables[i], fk_tables[i], MAX_NAME);
        tables[*count].fk_tables[i][MAX_NAME] = '\0';
        strncpy(tables[*count].fk_ref_cols[i], fk_ref_cols[i], MAX_NAME);
        tables[*count].fk_ref_cols[i][MAX_NAME] = '\0';
    }
    (*count)++;
    if (!save_catalog(tables, *count)) {
        printf("ERR CANNOT WRITE CATALOG\n");
        return;
    }
    printf("OK TABLE CREATED\n");
}

void cmd_create_index(struct TableDef tables[], int count, char *sql)
{
    char idx_name[MAX_NAME + 1];
    char table_name[MAX_NAME + 1];
    char col_name[MAX_NAME + 1];
    char *p;
    char *onp;
    char *open;
    char *close;
    int i = 0;
    int tidx;
    int cidx;
    struct RowSet rows;
    p = sql + strlen("CREATE INDEX");
    p = ltrim(p);
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (i < MAX_NAME) {
            idx_name[i++] = (char)toupper((unsigned char)*p);
        }
        p++;
    }
    idx_name[i] = '\0';
    if (!valid_name(idx_name)) {
        printf("ERR BAD INDEX NAME\n");
        return;
    }
    onp = find_i(p, "ON");
    if (onp == NULL) {
        printf("ERR USAGE: CREATE INDEX name ON table (col)\n");
        return;
    }
    p = ltrim(onp + strlen("ON"));
    i = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (i < MAX_NAME) {
            table_name[i++] = (char)toupper((unsigned char)*p);
        }
        p++;
    }
    table_name[i] = '\0';
    tidx = find_table(tables, count, table_name);
    if (tidx < 0) {
        printf("ERR TABLE NOT FOUND\n");
        return;
    }
    if (tables[tidx].index_count >= MAX_INDEXES) {
        printf("ERR TOO MANY INDEXES\n");
        return;
    }
    if (find_index_name(&tables[tidx], idx_name) >= 0) {
        printf("ERR INDEX EXISTS\n");
        return;
    }
    open = strchr(p, '(');
    close = strrchr(sql, ')');
    if (open == NULL || close == NULL || close <= open + 1) {
        printf("ERR USAGE: CREATE INDEX name ON table (col)\n");
        return;
    }
    *close = '\0';
    strncpy(col_name, open + 1, MAX_NAME);
    col_name[MAX_NAME] = '\0';
    clean_token(col_name);
    cidx = find_col(&tables[tidx], col_name);
    if (cidx < 0) {
        printf("ERR BAD INDEX COLUMN\n");
        return;
    }
    strncpy(tables[tidx].index_names[tables[tidx].index_count], idx_name,
            MAX_NAME);
    tables[tidx].index_names[tables[tidx].index_count][MAX_NAME] = '\0';
    tables[tidx].index_cols[tables[tidx].index_count] = cidx;
    tables[tidx].index_count++;
    if (!load_rows(&tables[tidx], &rows)) {
        printf("ERR CANNOT READ TABLE\n");
        return;
    }
    if (!rebuild_indexes(&tables[tidx], rows.rows, rows.count)) {
        rowset_free(&rows);
        printf("ERR CANNOT WRITE INDEX\n");
        return;
    }
    rowset_free(&rows);
    if (!save_catalog(tables, count)) {
        printf("ERR CANNOT WRITE CATALOG\n");
        return;
    }
    printf("OK INDEX CREATED\n");
}

void cmd_drop_index(struct TableDef tables[], int count, char *sql)
{
    char idx_name[MAX_NAME + 1];
    int t;
    int i;
    int found_table = -1;
    int found_index = -1;
    struct RowSet rows;

    if (!parse_name_after(sql, "DROP INDEX", idx_name)) {
        printf("ERR USAGE: DROP INDEX name\n");
        return;
    }
    for (t = 0; t < count; t++) {
        i = find_index_name(&tables[t], idx_name);
        if (i >= 0) {
            found_table = t;
            found_index = i;
            break;
        }
    }
    if (found_table < 0) {
        printf("ERR INDEX NOT FOUND\n");
        return;
    }
    if (!load_rows(&tables[found_table], &rows)) {
        printf("ERR CANNOT READ TABLE\n");
        return;
    }
    for (i = found_index; i < tables[found_table].index_count - 1; i++) {
        strncpy(tables[found_table].index_names[i],
                tables[found_table].index_names[i + 1], MAX_NAME);
        tables[found_table].index_names[i][MAX_NAME] = '\0';
        tables[found_table].index_cols[i] =
            tables[found_table].index_cols[i + 1];
    }
    tables[found_table].index_count--;
    if (!delete_index_rows(&tables[found_table]) ||
        !rebuild_indexes(&tables[found_table], rows.rows, rows.count)) {
        rowset_free(&rows);
        printf("ERR CANNOT DELETE INDEX\n");
        return;
    }
    rowset_free(&rows);
    if (!save_catalog(tables, count)) {
        printf("ERR CANNOT WRITE CATALOG\n");
        return;
    }
    printf("OK INDEX DROPPED\n");
}

void cmd_drop(struct TableDef tables[], int *count, char *sql)
{
    char name[MAX_NAME + 1];
    char key[KV_KEY];
    char prefix[KV_KEY];
    int prefix_len;
    struct KeySet keys;
    int idx;
    int i;

    if (!parse_name_after(sql, "DROP TABLE", name)) {
        printf("ERR USAGE: DROP TABLE name\n");
        return;
    }
    idx = find_table(tables, *count, name);
    if (idx < 0) {
        printf("ERR TABLE NOT FOUND\n");
        return;
    }
    for (i = 0; i < *count; i++) {
        int f;

        if (i == idx) {
            continue;
        }
        for (f = 0; f < tables[i].fk_count; f++) {
            if (eqi(tables[i].fk_tables[f], tables[idx].name)) {
                printf("ERR TABLE REFERENCED\n");
                return;
            }
        }
    }
    kv_make_key(key, "T", tables[idx].name, -1);
    if (!kv_delete(key)) {
        printf("ERR CANNOT DELETE CATALOG\n");
        return;
    }
    keyset_init(&keys);
    kv_make_prefix(prefix, "R", tables[idx].name, &prefix_len);
    if (!kv_scan(prefix, prefix_len, key_cb, &keys)) {
        keyset_free(&keys);
        printf("ERR CANNOT DELETE TABLE\n");
        return;
    }
    for (i = 0; i < keys.count; i++) {
        if (!kv_delete(keys.keys[i])) {
            keyset_free(&keys);
            printf("ERR CANNOT DELETE TABLE\n");
            return;
        }
    }
    keyset_free(&keys);
    if (!delete_index_rows(&tables[idx])) {
        printf("ERR CANNOT DELETE INDEX\n");
        return;
    }
    for (i = idx; i < *count - 1; i++) {
        tables[i] = tables[i + 1];
    }
    (*count)--;
    if (!save_catalog(tables, *count)) {
        printf("ERR CANNOT WRITE CATALOG\n");
        return;
    }
    printf("OK TABLE DROPPED\n");
}

