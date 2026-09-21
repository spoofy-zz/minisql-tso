#include "minisql.h"

static int keyset_add(struct KeySet *set, const char *key);
static int row_cb(const char *key, const char *data, void *arg);
static int fk_value_exists(struct TableDef tables[], int count,
                           struct TableDef *child, int fk_no,
                           const char *value);

struct RowScan {
    struct TableDef *table;
    struct RowSet *set;
    int ok;
};

void rowset_init(struct RowSet *set)
{
    set->rows = NULL;
    set->count = 0;
    set->cap = 0;
}

void rowset_free(struct RowSet *set)
{
    if (set->rows != NULL) {
        free(set->rows);
    }
    set->rows = NULL;
    set->count = 0;
    set->cap = 0;
}

int rowset_reserve(struct RowSet *set, int need)
{
    struct Row *rows;
    int cap;

    if (need <= set->cap) {
        return 1;
    }
    cap = set->cap == 0 ? ROWSET_INITIAL : set->cap;
    while (cap < need) {
        cap *= 2;
    }
    rows = (struct Row *)realloc(set->rows, sizeof(struct Row) * cap);
    if (rows == NULL) {
        return 0;
    }
    set->rows = rows;
    set->cap = cap;
    return 1;
}

int rowset_add(struct RowSet *set, struct Row *row)
{
    if (!rowset_reserve(set, set->count + 1)) {
        return 0;
    }
    set->rows[set->count++] = *row;
    return 1;
}

void keyset_init(struct KeySet *set)
{
    set->keys = NULL;
    set->count = 0;
    set->cap = 0;
}

void keyset_free(struct KeySet *set)
{
    if (set->keys != NULL) {
        free(set->keys);
    }
    set->keys = NULL;
    set->count = 0;
    set->cap = 0;
}

static int keyset_add(struct KeySet *set, const char *key)
{
    char (*keys)[KV_KEY];
    int cap;

    if (set->count >= set->cap) {
        cap = set->cap == 0 ? ROWSET_INITIAL : set->cap * 2;
        keys = (char (*)[KV_KEY])realloc(set->keys, KV_KEY * cap);
        if (keys == NULL) {
            return 0;
        }
        set->keys = keys;
        set->cap = cap;
    }
    memcpy(set->keys[set->count], key, KV_KEY);
    set->count++;
    return 1;
}

static int row_cb(const char *key, const char *data, void *arg)
{
    struct RowScan *scan = (struct RowScan *)arg;
    char vals[MAX_COLS][MAX_VALUE + 1];
    char buf[KV_DATA + 1];
    int cnt = 0;
    int i;
    struct Row row;

    (void)key;
    strncpy(buf, data, KV_DATA);
    buf[KV_DATA] = '\0';
    if (!parse_csv(buf, vals, &cnt) || cnt != scan->table->col_count) {
        scan->ok = 0;
        return 0;
    }
    for (i = 0; i < cnt; i++) {
        strncpy(row.values[i], vals[i], MAX_VALUE);
        row.values[i][MAX_VALUE] = '\0';
    }
    if (!rowset_add(scan->set, &row)) {
        scan->ok = 0;
        return 0;
    }
    return 1;
}

int load_rows(struct TableDef *t, struct RowSet *set)
{
    char prefix[KV_KEY];
    int prefix_len;
    struct RowScan scan;

    rowset_init(set);
    kv_make_prefix(prefix, "R", t->name, &prefix_len);
    scan.table = t;
    scan.set = set;
    scan.ok = 1;
    if (!kv_scan(prefix, prefix_len, row_cb, &scan) || !scan.ok) {
        rowset_free(set);
        return 0;
    }
    return 1;
}

int load_row_slot(struct TableDef *t, int slot, struct Row *row,
                         int *found)
{
    char key[KV_KEY];
    char data[KV_DATA + 1];
    char vals[MAX_COLS][MAX_VALUE + 1];
    int cnt = 0;
    int c;

    *found = 0;
    kv_make_key(key, "R", t->name, slot);
    if (!kv_get(key, data, sizeof(data))) {
        return 1;
    }
    if (!parse_csv(data, vals, &cnt) || cnt != t->col_count) {
        return 0;
    }
    for (c = 0; c < cnt; c++) {
        strncpy(row->values[c], vals[c], MAX_VALUE);
        row->values[c][MAX_VALUE] = '\0';
    }
    *found = 1;
    return 1;
}

int delete_index_rows(struct TableDef *t)
{
    struct KeySet keys;
    char prefix[KV_KEY];
    int prefix_len;
    int i;

    keyset_init(&keys);
    sprintf(prefix, "X|%-16.16s|", t->name);
    prefix_len = 19;
    if (!kv_scan(prefix, prefix_len, key_cb, &keys)) {
        keyset_free(&keys);
        return 0;
    }
    for (i = 0; i < keys.count; i++) {
        if (!kv_delete(keys.keys[i])) {
            keyset_free(&keys);
            return 0;
        }
    }
    keyset_free(&keys);
    return 1;
}

int key_cb(const char *key, const char *data, void *arg)
{
    struct KeySet *keys = (struct KeySet *)arg;

    (void)data;
    if (!keyset_add(keys, key)) {
        return 0;
    }
    return 1;
}

int rebuild_indexes(struct TableDef *t, struct Row rows[],
                           int row_count)
{
    int i;
    int r;

    if (!delete_index_rows(t)) {
        return 0;
    }
    for (i = 0; i < t->index_count; i++) {
        for (r = 0; r < row_count; r++) {
            char key[KV_KEY];
            kv_make_index_key(key, t->name, t->index_names[i],
                              rows[r].values[t->index_cols[i]], r + 1);
            if (!kv_put(key, "")) {
                return 0;
            }
        }
    }
    return 1;
}

int save_rows(struct TableDef *t, struct Row rows[], int row_count)
{
    struct KeySet keys;
    char prefix[KV_KEY];
    int prefix_len;
    int r;
    int c;

    keyset_init(&keys);
    kv_make_prefix(prefix, "R", t->name, &prefix_len);
    if (!kv_scan(prefix, prefix_len, key_cb, &keys)) {
        keyset_free(&keys);
        return 0;
    }
    for (r = 0; r < keys.count; r++) {
        if (!kv_delete(keys.keys[r])) {
            keyset_free(&keys);
            return 0;
        }
    }
    keyset_free(&keys);
    for (r = 0; r < row_count; r++) {
        char key[KV_KEY];
        char data[KV_DATA + 1];
        data[0] = '\0';
        for (c = 0; c < t->col_count; c++) {
            if (c > 0) {
                if ((int)strlen(data) + 1 > KV_DATA) {
                    return 0;
                }
                strcat(data, ",");
            }
            if ((int)strlen(data) + (int)strlen(rows[r].values[c]) >
                KV_DATA) {
                return 0;
            }
            strcat(data, rows[r].values[c]);
        }
        kv_make_key(key, "R", t->name, r + 1);
        if (!kv_put(key, data)) {
            return 0;
        }
    }
    if (!rebuild_indexes(t, rows, row_count)) {
        return 0;
    }
    kv_close();
    return 1;
}

static int fk_value_exists(struct TableDef tables[], int count,
                           struct TableDef *child, int fk_no,
                           const char *value)
{
    int parent;
    int ref_col;
    int r;
    struct RowSet rows;

    parent = find_table(tables, count, child->fk_tables[fk_no]);
    if (parent < 0) {
        return 0;
    }
    ref_col = find_col(&tables[parent], child->fk_ref_cols[fk_no]);
    if (ref_col < 0 || tables[parent].pk_col != ref_col) {
        return 0;
    }
    if (!load_rows(&tables[parent], &rows)) {
        return 0;
    }
    for (r = 0; r < rows.count; r++) {
        if (eqi(rows.rows[r].values[ref_col], value)) {
            rowset_free(&rows);
            return 1;
        }
    }
    rowset_free(&rows);
    return 0;
}

int check_foreign_keys(struct TableDef tables[], int count,
                              struct TableDef *table,
                              char values[][MAX_VALUE + 1])
{
    int i;

    for (i = 0; i < table->fk_count; i++) {
        if (values[table->fk_cols[i]][0] == '\0' ||
            !fk_value_exists(tables, count, table, i,
                             values[table->fk_cols[i]])) {
            printf("ERR FOREIGN KEY NOT FOUND\n");
            return 0;
        }
    }
    return 1;
}

int row_is_referenced(struct TableDef tables[], int count,
                             struct TableDef *parent, const char *value)
{
    int t;
    int f;

    if (parent->pk_col < 0) {
        return 0;
    }
    for (t = 0; t < count; t++) {
        for (f = 0; f < tables[t].fk_count; f++) {
            int r;
            struct RowSet rows;

            if (!eqi(tables[t].fk_tables[f], parent->name) ||
                !eqi(tables[t].fk_ref_cols[f],
                     parent->cols[parent->pk_col])) {
                continue;
            }
            if (!load_rows(&tables[t], &rows)) {
                return 1;
            }
            for (r = 0; r < rows.count; r++) {
                if (eqi(rows.rows[r].values[tables[t].fk_cols[f]], value)) {
                    rowset_free(&rows);
                    return 1;
                }
            }
            rowset_free(&rows);
        }
    }
    return 0;
}

/* Measure before printing so every PIPE stays in the same position. */
