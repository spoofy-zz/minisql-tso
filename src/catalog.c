#include "minisql.h"

static void catalog_col_token(struct TableDef *t, int col,
                              char *out, int max);
static int decode_column_token(struct TableDef *table, int col, char *tok);
static int decode_table_def(struct TableDef *table, const char *data);

static void catalog_col_token(struct TableDef *t, int col,
                              char *out, int max)
{
    if (t->col_types[col] == TYPE_INT) {
        sprintf(out, "%s:I", t->cols[col]);
    } else if (t->col_types[col] == TYPE_CHAR) {
        sprintf(out, "%s:C%d", t->cols[col], t->col_lens[col]);
    } else if (t->col_types[col] == TYPE_VARCHAR) {
        sprintf(out, "%s:V%d", t->cols[col], t->col_lens[col]);
    } else {
        sprintf(out, "%s:T", t->cols[col]);
    }
    out[max - 1] = '\0';
}

static int decode_column_token(struct TableDef *table, int col, char *tok)
{
    char *colon;

    table->col_types[col] = TYPE_TEXT;
    table->col_lens[col] = MAX_VALUE;
    colon = strchr(tok, ':');
    if (colon != NULL) {
        *colon = '\0';
        colon++;
        if (eqi(colon, "I")) {
            table->col_types[col] = TYPE_INT;
            table->col_lens[col] = 0;
        } else if (colon[0] == 'C') {
            table->col_types[col] = TYPE_CHAR;
            table->col_lens[col] = atoi(colon + 1);
        } else if (colon[0] == 'V') {
            table->col_types[col] = TYPE_VARCHAR;
            table->col_lens[col] = atoi(colon + 1);
        } else if (!eqi(colon, "T")) {
            return 0;
        }
        if (table->col_types[col] != TYPE_INT &&
            (table->col_lens[col] < 1 ||
             table->col_lens[col] > MAX_VALUE)) {
            return 0;
        }
    }
    strncpy(table->cols[col], tok, MAX_NAME);
    table->cols[col][MAX_NAME] = '\0';
    rtrim(table->cols[col]);
    return valid_name(table->cols[col]);
}

static int decode_table_def(struct TableDef *table, const char *data)
{
    char buf[KV_DATA + 1];
    char pk_name[MAX_NAME + 1];
    char *tok;
    int c = 0;
    int i;

    strncpy(buf, data, KV_DATA);
    buf[KV_DATA] = '\0';
    tok = strtok(buf, "|");
    if (tok == NULL) {
        return 0;
    }
    strncpy(table->name, tok, MAX_NAME);
    table->name[MAX_NAME] = '\0';
    table->pk_col = -1;
    table->index_count = 0;
    table->fk_count = 0;
    pk_name[0] = '\0';
    while ((tok = strtok(NULL, "|")) != NULL) {
        if (starts_i(tok, "PK=")) {
            strncpy(pk_name, tok + 3, MAX_NAME);
            pk_name[MAX_NAME] = '\0';
            rtrim(pk_name);
            continue;
        }
        if (starts_i(tok, "IX=")) {
            char idxbuf[MAX_VALUE + 1];
            char *colon;
            int ixcol;

            strncpy(idxbuf, tok + 3, MAX_VALUE);
            idxbuf[MAX_VALUE] = '\0';
            rtrim(idxbuf);
            colon = strchr(idxbuf, ':');
            if (colon == NULL || table->index_count >= MAX_INDEXES) {
                return 0;
            }
            *colon = '\0';
            clean_token(idxbuf);
            clean_token(colon + 1);
            ixcol = -1;
            for (i = 0; i < c; i++) {
                if (eqi(table->cols[i], colon + 1)) {
                    ixcol = i;
                    break;
                }
            }
            if (ixcol < 0) {
                return 0;
            }
            strncpy(table->index_names[table->index_count], idxbuf,
                    MAX_NAME);
            table->index_names[table->index_count][MAX_NAME] = '\0';
            table->index_cols[table->index_count] = ixcol;
            table->index_count++;
            continue;
        }
        if (starts_i(tok, "FK=")) {
            char fkbuf[MAX_VALUE + 1];
            char *p1;
            char *p2;
            int fkcol;

            if (table->fk_count >= MAX_FKS) {
                return 0;
            }
            strncpy(fkbuf, tok + 3, MAX_VALUE);
            fkbuf[MAX_VALUE] = '\0';
            rtrim(fkbuf);
            p1 = strchr(fkbuf, ':');
            if (p1 == NULL) {
                return 0;
            }
            *p1++ = '\0';
            p2 = strchr(p1, ':');
            if (p2 == NULL) {
                return 0;
            }
            *p2++ = '\0';
            clean_token(fkbuf);
            clean_token(p1);
            clean_token(p2);
            fkcol = -1;
            for (i = 0; i < c; i++) {
                if (eqi(table->cols[i], fkbuf)) {
                    fkcol = i;
                    break;
                }
            }
            if (fkcol < 0 || !valid_name(p1) || !valid_name(p2)) {
                return 0;
            }
            table->fk_cols[table->fk_count] = fkcol;
            strncpy(table->fk_tables[table->fk_count], p1, MAX_NAME);
            table->fk_tables[table->fk_count][MAX_NAME] = '\0';
            strncpy(table->fk_ref_cols[table->fk_count], p2, MAX_NAME);
            table->fk_ref_cols[table->fk_count][MAX_NAME] = '\0';
            table->fk_count++;
            continue;
        }
        if (c >= MAX_COLS) {
            return 0;
        }
        if (!decode_column_token(table, c, tok)) {
            return 0;
        }
        c++;
    }
    table->col_count = c;
    if (pk_name[0] != '\0') {
        for (i = 0; i < c; i++) {
            if (eqi(table->cols[i], pk_name)) {
                table->pk_col = i;
                break;
            }
        }
    }
    return c > 0;
}

int load_catalog(struct TableDef tables[], int *count)
{
    int i;
    int out = 0;

    for (i = 1; i <= MAX_TABLES; i++) {
        char slot_key[KV_KEY];
        char table_key[KV_KEY];
        char name[MAX_NAME + 1];
        char data[KV_DATA + 1];

        kv_make_key(slot_key, "C", "", i);
        if (!kv_get(slot_key, name, sizeof(name))) {
            continue;
        }
        clean_token(name);
        if (!valid_name(name)) {
            return 0;
        }
        kv_make_key(table_key, "T", name, -1);
        if (!kv_get(table_key, data, sizeof(data))) {
            return 0;
        }
        if (!decode_table_def(&tables[out], data)) {
            return 0;
        }
        out++;
    }
    *count = out;
    return 1;
}

int save_catalog(struct TableDef tables[], int count)
{
    int i;
    int c;

    for (i = 1; i <= MAX_TABLES; i++) {
        char key[KV_KEY];
        kv_make_key(key, "C", "", i);
        if (!kv_delete(key)) {
            return 0;
        }
    }
    for (i = 0; i < count; i++) {
        char key[KV_KEY];
        char data[KV_DATA + 1];
        kv_make_key(key, "C", "", i + 1);
        if (!kv_put(key, tables[i].name)) {
            return 0;
        }
        kv_make_key(key, "T", tables[i].name, -1);
        if (!kv_delete(key)) {
            return 0;
        }
        strcpy(data, tables[i].name);
        if (tables[i].pk_col >= 0) {
            if ((int)strlen(data) + 4 +
                (int)strlen(tables[i].cols[tables[i].pk_col]) > KV_DATA) {
                return 0;
            }
            strcat(data, "|PK=");
            strcat(data, tables[i].cols[tables[i].pk_col]);
        }
        for (c = 0; c < tables[i].col_count; c++) {
            char coltok[MAX_VALUE + 1];

            catalog_col_token(&tables[i], c, coltok, sizeof(coltok));
            if ((int)strlen(data) + 1 + (int)strlen(coltok) > KV_DATA) {
                return 0;
            }
            strcat(data, "|");
            strcat(data, coltok);
        }
        for (c = 0; c < tables[i].index_count; c++) {
            if ((int)strlen(data) + 5 +
                (int)strlen(tables[i].index_names[c]) +
                (int)strlen(tables[i].cols[tables[i].index_cols[c]]) >
                KV_DATA) {
                return 0;
            }
            strcat(data, "|IX=");
            strcat(data, tables[i].index_names[c]);
            strcat(data, ":");
            strcat(data, tables[i].cols[tables[i].index_cols[c]]);
        }
        for (c = 0; c < tables[i].fk_count; c++) {
            if ((int)strlen(data) + 5 +
                (int)strlen(tables[i].cols[tables[i].fk_cols[c]]) +
                (int)strlen(tables[i].fk_tables[c]) +
                (int)strlen(tables[i].fk_ref_cols[c]) > KV_DATA) {
                return 0;
            }
            strcat(data, "|FK=");
            strcat(data, tables[i].cols[tables[i].fk_cols[c]]);
            strcat(data, ":");
            strcat(data, tables[i].fk_tables[c]);
            strcat(data, ":");
            strcat(data, tables[i].fk_ref_cols[c]);
        }
        if (!kv_put(key, data)) {
            return 0;
        }
    }
    kv_close();
    return 1;
}

int find_pk_duplicate(struct TableDef *t, struct Row rows[],
                             int row_count, const char *value,
                             int skip_row)
{
    int r;

    if (t->pk_col < 0) {
        return -1;
    }
    for (r = 0; r < row_count; r++) {
        if (r == skip_row) {
            continue;
        }
        if (eqi(rows[r].values[t->pk_col], value)) {
            return r;
        }
    }
    return -1;
}

int find_table(struct TableDef tables[], int count, const char *name)
{
    int i;
    for (i = 0; i < count; i++) {
        if (eqi(tables[i].name, name)) {
            return i;
        }
    }
    return -1;
}

int find_col(struct TableDef *t, const char *name)
{
    int i;
    for (i = 0; i < t->col_count; i++) {
        if (eqi(t->cols[i], name)) {
            return i;
        }
    }
    return -1;
}

int find_index_name(struct TableDef *t, const char *name)
{
    int i;
    for (i = 0; i < t->index_count; i++) {
        if (eqi(t->index_names[i], name)) {
            return i;
        }
    }
    return -1;
}

int find_index_col(struct TableDef *t, int col)
{
    int i;
    for (i = 0; i < t->index_count; i++) {
        if (t->index_cols[i] == col) {
            return i;
        }
    }
    return -1;
}

