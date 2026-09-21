#include "minisql.h"

struct JoinProj {
    int source;
    int col;
};


struct GroupRow {
    char value[MAX_VALUE + 1];
    int count;
};


static void measure_cell(int *width, const char *value);
static void print_cell(const char *value, int width, int last);
static void print_header_rule(int widths[], int col_count);
static int is_count_expr(const char *s);
static char *find_keyword(char *s, const char *keyword);
static void print_projected_line(struct TableDef *t, struct Row *row,
                                 int cols[], int col_count, int header,
                                 int widths[]);
static int parse_select_list(struct TableDef *t, char *text, int cols[],
                             int *col_count, int *select_all,
                             int *select_count);
static int cmp_row_qsort(const void *a, const void *b);
static int cmp_group_qsort(const void *a, const void *b);
static char *next_select_clause(char *where, char *group, char *order,
                                char *limit);
static int parse_select_col_clause(char *text, const char *keyword,
                                   char *col_name);
static int parse_order_clause(struct TableDef *t, char *text, int group_col,
                              int *order_col, int *order_count, int *desc);
static int parse_limit_clause(char *text, int *limit);
static int add_group_row(struct GroupRow groups[], int group_cap,
                         int *group_count, const char *value);
static int parse_qualified_col(char *text, char *table, char *col);
static int name_matches(const char *token, const char *table,
                        const char *alias);
static int parse_alias_segment(char *start, char *end, char *alias);
static int parse_join_select_list(char *text, struct TableDef *left,
                                  const char *left_alias,
                                  struct TableDef *right,
                                  const char *right_alias,
                                  struct JoinProj projs[],
                                  int *proj_count, int *select_all);
static void print_join_header(struct TableDef *left, const char *left_alias,
                              struct TableDef *right, const char *right_alias,
                              struct JoinProj projs[], int proj_count,
                              int widths[], int measure);
static void print_join_row(struct Row *lrow, struct Row *rrow,
                           struct JoinProj projs[], int proj_count,
                           int widths[], int measure);
static void cmd_select_join(struct TableDef tables[], int count, char *sql);
static void explain_select_join(struct TableDef tables[], int count, char *sql);

static struct TableDef *g_sort_table = NULL;
static int g_sort_col = -1;
static int g_sort_desc = 0;
static int g_group_sort_by_count = 0;

static void measure_cell(int *width, const char *value)
{
    int len = (int)strlen(value);
    if (len > *width) {
        *width = len;
    }
}

static void print_cell(const char *value, int width, int last)
{
    if (last) {
        printf("%s\n", value);
    } else {
        printf("%-*s | ", width, value);
    }
}

static void print_header_rule(int widths[], int col_count)
{
    int c;
    int i;

    for (c = 0; c < col_count; c++) {
        if (c > 0) {
            printf("-|");
        }
        for (i = 0; i < widths[c] + (c > 0 ? 1 : 0); i++) {
            printf("-");
        }
    }
    printf("\n");
}

static int is_count_expr(const char *s)
{
    return eqi(s, "COUNT") || eqi(s, "COUNT()") || eqi(s, "COUNT(*)");
}

static char *find_keyword(char *s, const char *keyword)
{
    int n = (int)strlen(keyword);
    char *p;

    for (p = s; *p != '\0'; p++) {
        int before = p == s || !isalnum((unsigned char)p[-1]);
        int after;

        if (ncmp_i(p, keyword, n) != 0) {
            continue;
        }
        after = p[n] == '\0' || !isalnum((unsigned char)p[n]);
        if (before && after) {
            return p;
        }
    }
    return NULL;
}

static void print_projected_line(struct TableDef *t, struct Row *row,
                                 int cols[], int col_count, int header,
                                 int widths[])
{
    int i;

    for (i = 0; i < col_count; i++) {
        print_cell(header ? t->cols[cols[i]] : row->values[cols[i]],
                   widths[i], i == col_count - 1);
    }
    if (header) {
        print_header_rule(widths, col_count);
    }
}

static int parse_select_list(struct TableDef *t, char *text, int cols[],
                             int *col_count, int *select_all,
                             int *select_count)
{
    char vals[MAX_COLS][MAX_VALUE + 1];
    int val_count = 0;
    int i;

    *col_count = 0;
    *select_all = 0;
    *select_count = 0;
    clean_token_max(text, MAX_STATEMENT);
    if (eqi(text, "*")) {
        *select_all = 1;
        return 1;
    }
    if (is_count_expr(text)) {
        *select_count = 1;
        return 1;
    }
    if (!parse_csv_limit(text, vals, &val_count, MAX_COLS) ||
        val_count < 1) {
        return 0;
    }
    for (i = 0; i < val_count; i++) {
        int c;

        if (is_count_expr(vals[i])) {
            *select_count = 1;
            continue;
        }
        c = find_col(t, vals[i]);
        if (c < 0) {
            return 0;
        }
        cols[*col_count] = c;
        (*col_count)++;
    }
    return *col_count > 0 || *select_count;
}

static int cmp_row_qsort(const void *a, const void *b)
{
    const struct Row *ra = (const struct Row *)a;
    const struct Row *rb = (const struct Row *)b;
    int rc = cmp_value(g_sort_table, g_sort_col,
                       ra->values[g_sort_col], rb->values[g_sort_col]);

    return g_sort_desc ? -rc : rc;
}

static int cmp_group_qsort(const void *a, const void *b)
{
    const struct GroupRow *ga = (const struct GroupRow *)a;
    const struct GroupRow *gb = (const struct GroupRow *)b;
    int rc;

    if (g_group_sort_by_count) {
        if (ga->count < gb->count) {
            rc = -1;
        } else if (ga->count > gb->count) {
            rc = 1;
        } else {
            rc = 0;
        }
    } else {
        rc = cmp_value(g_sort_table, g_sort_col, ga->value, gb->value);
    }
    return g_sort_desc ? -rc : rc;
}

static char *next_select_clause(char *where, char *group, char *order,
                                char *limit)
{
    char *next = NULL;

    if (where != NULL) {
        next = where;
    }
    if (group != NULL && (next == NULL || group < next)) {
        next = group;
    }
    if (order != NULL && (next == NULL || order < next)) {
        next = order;
    }
    if (limit != NULL && (next == NULL || limit < next)) {
        next = limit;
    }
    return next;
}

static int parse_select_col_clause(char *text, const char *keyword,
                                   char *col_name)
{
    char *p = ltrim(text + strlen(keyword));
    int i = 0;

    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (i < MAX_NAME) {
            col_name[i++] = (char)toupper((unsigned char)*p);
        }
        p++;
    }
    col_name[i] = '\0';
    p = ltrim(p);
    return valid_name(col_name) && *p == '\0';
}

static int parse_order_clause(struct TableDef *t, char *text, int group_col,
                              int *order_col, int *order_count, int *desc)
{
    char *p = ltrim(text + strlen("ORDER BY"));
    char name[MAX_NAME + 1];
    int i = 0;

    *order_col = -1;
    *order_count = 0;
    *desc = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_' || *p == '(' ||
           *p == ')' || *p == '*')) {
        if (i < MAX_NAME) {
            name[i++] = (char)toupper((unsigned char)*p);
        }
        p++;
    }
    name[i] = '\0';
    p = ltrim(p);
    if (eqi(p, "DESC")) {
        *desc = 1;
        p += strlen("DESC");
    } else if (eqi(p, "ASC")) {
        p += strlen("ASC");
    }
    p = ltrim(p);
    if (*p != '\0') {
        return 0;
    }
    if (eqi(name, "COUNT") || eqi(name, "COUNT()") ||
        eqi(name, "COUNT(*)")) {
        if (group_col < 0) {
            return 0;
        }
        *order_count = 1;
        return 1;
    }
    *order_col = find_col(t, name);
    if (*order_col < 0) {
        return 0;
    }
    if (group_col >= 0 && *order_col != group_col) {
        return 0;
    }
    return 1;
}

static int parse_limit_clause(char *text, int *limit)
{
    char *p = ltrim(text + strlen("LIMIT"));
    int n;

    if (*p == '\0') {
        return 0;
    }
    n = 0;
    while (*p != '\0' && isdigit((unsigned char)*p)) {
        n = n * 10 + (*p - '0');
        p++;
    }
    p = ltrim(p);
    if (*p != '\0' || n < 0) {
        return 0;
    }
    *limit = n;
    return 1;
}

static int add_group_row(struct GroupRow groups[], int group_cap,
                         int *group_count, const char *value)
{
    int i;

    for (i = 0; i < *group_count; i++) {
        if (eqi(groups[i].value, value)) {
            groups[i].count++;
            return 1;
        }
    }
    if (*group_count >= group_cap) {
        return 0;
    }
    strncpy(groups[*group_count].value, value, MAX_VALUE);
    groups[*group_count].value[MAX_VALUE] = '\0';
    groups[*group_count].count = 1;
    (*group_count)++;
    return 1;
}

static int parse_qualified_col(char *text, char *table, char *col)
{
    char buf[MAX_VALUE + 1];
    char *dot;

    strncpy(buf, text, MAX_VALUE);
    buf[MAX_VALUE] = '\0';
    clean_token(buf);
    dot = strchr(buf, '.');
    if (dot == NULL) {
        return 0;
    }
    *dot = '\0';
    strncpy(table, buf, MAX_NAME);
    table[MAX_NAME] = '\0';
    strncpy(col, dot + 1, MAX_NAME);
    col[MAX_NAME] = '\0';
    return valid_name(table) && valid_name(col);
}

static int name_matches(const char *token, const char *table,
                        const char *alias)
{
    return eqi(token, table) || (alias[0] != '\0' && eqi(token, alias));
}

static int parse_alias_segment(char *start, char *end, char *alias)
{
    char buf[MAX_VALUE + 1];
    char *p;
    int len;
    int i = 0;

    alias[0] = '\0';
    len = (int)(end - start);
    if (len <= 0) {
        return 1;
    }
    if (len > MAX_VALUE) {
        return 0;
    }
    strncpy(buf, start, len);
    buf[len] = '\0';
    p = trim(buf);
    if (*p == '\0') {
        return 1;
    }
    if (starts_i(p, "AS") && isspace((unsigned char)p[2])) {
        p = ltrim(p + 2);
    }
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (i < MAX_NAME) {
            alias[i++] = (char)toupper((unsigned char)*p);
        }
        p++;
    }
    alias[i] = '\0';
    p = ltrim(p);
    return valid_name(alias) && *p == '\0';
}

static int parse_join_select_list(char *text, struct TableDef *left,
                                  const char *left_alias,
                                  struct TableDef *right,
                                  const char *right_alias,
                                  struct JoinProj projs[],
                                  int *proj_count, int *select_all)
{
    char vals[MAX_JOIN_COLS][MAX_VALUE + 1];
    int val_count = 0;
    int i;

    *proj_count = 0;
    *select_all = 0;
    clean_token_max(text, MAX_STATEMENT);
    if (eqi(text, "*")) {
        *select_all = 1;
        return 1;
    }
    if (!parse_csv_limit(text, vals, &val_count, MAX_JOIN_COLS) ||
        val_count < 1) {
        return 0;
    }
    for (i = 0; i < val_count; i++) {
        char qtable[MAX_NAME + 1];
        char qcol[MAX_NAME + 1];
        int col;

        if (!parse_qualified_col(vals[i], qtable, qcol)) {
            return 0;
        }
        if (name_matches(qtable, left->name, left_alias)) {
            col = find_col(left, qcol);
            if (col < 0) {
                return 0;
            }
            projs[*proj_count].source = 0;
            projs[*proj_count].col = col;
            (*proj_count)++;
        } else if (name_matches(qtable, right->name, right_alias)) {
            col = find_col(right, qcol);
            if (col < 0) {
                return 0;
            }
            projs[*proj_count].source = 1;
            projs[*proj_count].col = col;
            (*proj_count)++;
        } else {
            return 0;
        }
    }
    return *proj_count > 0;
}

static void print_join_header(struct TableDef *left, const char *left_alias,
                              struct TableDef *right, const char *right_alias,
                              struct JoinProj projs[], int proj_count,
                              int widths[], int measure)
{
    int c;
    char label[MAX_NAME * 2 + 2];

    for (c = 0; c < proj_count; c++) {
        struct TableDef *t = projs[c].source == 0 ? left : right;
        const char *alias = projs[c].source == 0 ? left_alias : right_alias;
        sprintf(label, "%s.%s", alias[0] != '\0' ? alias : t->name,
                t->cols[projs[c].col]);
        if (measure) {
            widths[c] = (int)strlen(label);
        } else {
            print_cell(label, widths[c], c == proj_count - 1);
        }
    }
    if (!measure) {
        print_header_rule(widths, proj_count);
    }
}

static void print_join_row(struct Row *lrow, struct Row *rrow,
                           struct JoinProj projs[], int proj_count,
                           int widths[], int measure)
{
    int c;

    for (c = 0; c < proj_count; c++) {
        struct Row *row = projs[c].source == 0 ? lrow : rrow;
        const char *value = row->values[projs[c].col];
        if (measure) {
            measure_cell(&widths[c], value);
        } else {
            print_cell(value, widths[c], c == proj_count - 1);
        }
    }
}

static void cmd_select_join(struct TableDef tables[], int count, char *sql)
{
    char left_name[MAX_NAME + 1];
    char right_name[MAX_NAME + 1];
    char left_alias[MAX_NAME + 1];
    char right_alias[MAX_NAME + 1];
    char qleft_table[MAX_NAME + 1];
    char qleft_col[MAX_NAME + 1];
    char qright_table[MAX_NAME + 1];
    char qright_col[MAX_NAME + 1];
    char select_buf[MAX_STATEMENT];
    char onbuf[MAX_STATEMENT];
    char wherebuf[MAX_STATEMENT];
    char *p;
    char *fromp;
    char *joinp;
    char *onp;
    char *eq;
    int i;
    int left_idx;
    int right_idx;
    int left_col;
    int right_col;
    int right_index;
    int widths[MAX_JOIN_COLS];
    int pass;
    int matched = 0;
    int has_where = 0;
    int where_source = -1;
    struct WhereExpr where_expr;
    int select_all = 0;
    int proj_count = 0;
    struct JoinProj projs[MAX_JOIN_COLS];
    struct RowSet left_rows;
    struct RowSet right_rows;

    fromp = find_keyword(sql + strlen("SELECT"), "FROM");
    if (fromp == NULL) {
        printf("ERR BAD JOIN\n");
        return;
    }
    i = (int)(fromp - (sql + strlen("SELECT")));
    if (i <= 0 || i >= MAX_STATEMENT) {
        printf("ERR BAD JOIN\n");
        return;
    }
    strncpy(select_buf, sql + strlen("SELECT"), i);
    select_buf[i] = '\0';
    p = ltrim(fromp + strlen("FROM"));
    i = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (i < MAX_NAME) {
            left_name[i++] = (char)toupper((unsigned char)*p);
        }
        p++;
    }
    left_name[i] = '\0';
    joinp = find_keyword(p, "JOIN");
    if (!valid_name(left_name) || joinp == NULL ||
        !parse_alias_segment(p, joinp, left_alias)) {
        printf("ERR BAD JOIN\n");
        return;
    }
    p = ltrim(joinp + strlen("JOIN"));
    i = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (i < MAX_NAME) {
            right_name[i++] = (char)toupper((unsigned char)*p);
        }
        p++;
    }
    right_name[i] = '\0';
    onp = find_keyword(p, "ON");
    if (!valid_name(right_name) || onp == NULL ||
        !parse_alias_segment(p, onp, right_alias)) {
        printf("ERR BAD JOIN\n");
        return;
    }
    strncpy(onbuf, onp + strlen("ON"), MAX_STATEMENT - 1);
    onbuf[MAX_STATEMENT - 1] = '\0';
    {
        char *wherep = find_keyword(onbuf, "WHERE");
        if (wherep != NULL) {
            strncpy(wherebuf, wherep + strlen("WHERE"), MAX_STATEMENT - 1);
            wherebuf[MAX_STATEMENT - 1] = '\0';
            *wherep = '\0';
            rtrim(onbuf);
            has_where = 1;
        }
    }
    eq = strchr(onbuf, '=');
    if (eq == NULL) {
        printf("ERR BAD JOIN\n");
        return;
    }
    *eq = '\0';
    if (!parse_qualified_col(onbuf, qleft_table, qleft_col) ||
        !parse_qualified_col(eq + 1, qright_table, qright_col)) {
        printf("ERR BAD JOIN\n");
        return;
    }
    left_idx = find_table(tables, count, left_name);
    right_idx = find_table(tables, count, right_name);
    if (left_idx < 0 || right_idx < 0) {
        printf("ERR TABLE NOT FOUND\n");
        return;
    }
    if (has_where) {
        char toks[MAX_CONDS * 5][MAX_VALUE + 1];
        int ntok = 0;
        char qtable[MAX_NAME + 1];
        char qcol[MAX_NAME + 1];
        char local_where[MAX_STATEMENT];
        char *wt = ltrim(wherebuf);
        if (!where_tokenize(wt, toks, &ntok) || ntok < 1 ||
            !parse_qualified_col(toks[0], qtable, qcol)) {
            printf("ERR BAD WHERE\n");
            return;
        }
        if (name_matches(qtable, tables[left_idx].name, left_alias)) {
            where_source = 0;
            strncpy(local_where, wt, MAX_STATEMENT - 1);
        } else if (name_matches(qtable, tables[right_idx].name, right_alias)) {
            where_source = 1;
            strncpy(local_where, wt, MAX_STATEMENT - 1);
        } else {
            printf("ERR BAD WHERE\n");
            return;
        }
        local_where[MAX_STATEMENT - 1] = '\0';
        /* Replace the qualified column with its table-local name. */
        {
            char rebuilt[MAX_STATEMENT];
            const char *rest = local_where + strlen(toks[0]);
            sprintf(rebuilt, "%s%s", qcol, rest);
            if (!parse_where(where_source == 0 ? &tables[left_idx] :
                             &tables[right_idx], rebuilt, &where_expr)) {
                printf("ERR BAD WHERE\n");
                return;
            }
        }
    }
    if (!name_matches(qleft_table, left_name, left_alias) ||
        !name_matches(qright_table, right_name, right_alias)) {
        printf("ERR JOIN ORDER\n");
        return;
    }
    if (!parse_join_select_list(select_buf, &tables[left_idx], left_alias,
                                &tables[right_idx], right_alias, projs,
                                &proj_count, &select_all)) {
        printf("ERR BAD SELECT LIST\n");
        return;
    }
    left_col = find_col(&tables[left_idx], qleft_col);
    right_col = find_col(&tables[right_idx], qright_col);
    if (left_col < 0 || right_col < 0) {
        printf("ERR BAD JOIN COLUMN\n");
        return;
    }
    if (!load_rows(&tables[left_idx], &left_rows)) {
        printf("ERR CANNOT READ TABLE\n");
        return;
    }
    right_index = find_index_col(&tables[right_idx], right_col);
    if (select_all) {
        proj_count = 0;
        for (i = 0; i < tables[left_idx].col_count; i++) {
            projs[proj_count].source = 0;
            projs[proj_count++].col = i;
        }
        for (i = 0; i < tables[right_idx].col_count; i++) {
            projs[proj_count].source = 1;
            projs[proj_count++].col = i;
        }
    }
    /* First pass measures matching rows without retaining the join result. */
    for (pass = 0; pass < 2; pass++) {
        print_join_header(&tables[left_idx], select_all ? "" : left_alias,
                          &tables[right_idx], select_all ? "" : right_alias,
                          projs, proj_count, widths, pass == 0);
        if (right_index >= 0) {
            int l;

            for (l = 0; l < left_rows.count; l++) {
                struct KeySet keys;
                char prefix[KV_KEY];
                int prefix_len;
                int k;

                keyset_init(&keys);
                kv_make_index_base_prefix(prefix, tables[right_idx].name,
                                          tables[right_idx].index_names[right_index],
                                          &prefix_len);
                if (!kv_scan(prefix, prefix_len, key_cb, &keys)) {
                    keyset_free(&keys);
                    rowset_free(&left_rows);
                    printf("ERR CANNOT READ INDEX\n");
                    return;
                }
                for (k = 0; k < keys.count; k++) {
                    int slot = kv_index_slot(keys.keys[k]);
                    int found;
                    struct Row rrow;

                    if (slot < 1) {
                        continue;
                    }
                    if (!load_row_slot(&tables[right_idx], slot, &rrow,
                                       &found)) {
                        keyset_free(&keys);
                        rowset_free(&left_rows);
                        printf("ERR CANNOT READ TABLE\n");
                        return;
                    }
                    if (found && eqi(left_rows.rows[l].values[left_col],
                                     rrow.values[right_col])) {
                        if (has_where &&
                            !where_match(where_source == 0 ? &tables[left_idx] :
                                         &tables[right_idx],
                                         where_source == 0 ? &left_rows.rows[l] :
                                         &rrow, &where_expr)) {
                            continue;
                        }
                        print_join_row(&left_rows.rows[l], &rrow, projs,
                                       proj_count, widths, pass == 0);
                        if (pass != 0) {
                            matched++;
                        }
                    }
                }
                keyset_free(&keys);
            }
        } else {
            int l;
            int r;

            if (!load_rows(&tables[right_idx], &right_rows)) {
                rowset_free(&left_rows);
                printf("ERR CANNOT READ TABLE\n");
                return;
            }
            for (l = 0; l < left_rows.count; l++) {
                for (r = 0; r < right_rows.count; r++) {
                    if (eqi(left_rows.rows[l].values[left_col],
                            right_rows.rows[r].values[right_col])) {
                        if (has_where &&
                            !where_match(where_source == 0 ? &tables[left_idx] :
                                         &tables[right_idx],
                                         where_source == 0 ? &left_rows.rows[l] :
                                         &right_rows.rows[r], &where_expr)) {
                            continue;
                        }
                        print_join_row(&left_rows.rows[l], &right_rows.rows[r],
                                       projs, proj_count, widths, pass == 0);
                        if (pass != 0) {
                            matched++;
                        }
                    }
                }
            }
            rowset_free(&right_rows);
        }
    }
    rowset_free(&left_rows);
    printf("OK %d ROWS\n", matched);
}

void cmd_select(struct TableDef tables[], int count, char *sql)
{
    char name[MAX_NAME + 1];
    char *p;
    char *where;
    char *group;
    char *order;
    char *limit;
    char *next;
    int idx;
    int r;
    int c;
    int group_count = 0;
    int group_col = -1;
    int order_col = -1;
    int order_count = 0;
    int order_desc = 0;
    int limit_count = -1;
    int widths[MAX_COLS];
    int group_width;
    int count_width;
    char count_text[32];
    int select_cols[MAX_COLS];
    int select_col_count = 0;
    int select_all = 0;
    int select_count = 0;
    int where_col;
    int index_no;
    char where_val[MAX_VALUE + 1];
    char col_name[MAX_NAME + 1];
    char select_buf[MAX_STATEMENT];
    char where_buf[MAX_STATEMENT];
    char group_buf[MAX_STATEMENT];
    char order_buf[MAX_STATEMENT];
    char limit_buf[MAX_STATEMENT];
    struct WhereExpr expr;
    struct RowSet rows;
    struct RowSet out;
    struct GroupRow *groups;

    rowset_init(&rows);
    rowset_init(&out);
    groups = NULL;

    if (!starts_i(sql, "SELECT")) {
        printf("ERR BAD SELECT\n");
        return;
    }
    if (find_keyword(sql + strlen("SELECT"), "JOIN") != NULL) {
        cmd_select_join(tables, count, sql);
        return;
    }
    p = find_keyword(sql + strlen("SELECT"), "FROM");
    if (p == NULL) {
        printf("ERR BAD SELECT\n");
        return;
    }
    c = (int)(p - (sql + strlen("SELECT")));
    if (c <= 0 || c >= MAX_STATEMENT) {
        printf("ERR BAD SELECT\n");
        return;
    }
    strncpy(select_buf, sql + strlen("SELECT"), c);
    select_buf[c] = '\0';
    if (*trim(select_buf) == '\0') {
        printf("ERR BAD SELECT\n");
        return;
    }
    p += strlen("FROM");
    p = ltrim(p);
    c = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (c < MAX_NAME) {
            name[c++] = (char)toupper((unsigned char)*p);
        }
        p++;
    }
    name[c] = '\0';
    idx = find_table(tables, count, name);
    if (idx < 0) {
        printf("ERR TABLE NOT FOUND\n");
        return;
    }
    if (!parse_select_list(&tables[idx], select_buf, select_cols,
                           &select_col_count, &select_all, &select_count)) {
        printf("ERR BAD SELECT LIST\n");
        return;
    }
    p = ltrim(p);
    where = find_i(p, "WHERE");
    group = find_i(p, "GROUP BY");
    order = find_i(p, "ORDER BY");
    limit = find_i(p, "LIMIT");
    next = next_select_clause(where, group, order, limit);
    if (next == NULL && *p != '\0') {
        printf("ERR BAD SELECT\n");
        return;
    }
    if (next != NULL && next != p && *trim(p) != '\0') {
        printf("ERR BAD SELECT\n");
        return;
    }
    if ((where != NULL && group != NULL && where > group) ||
        (where != NULL && order != NULL && where > order) ||
        (where != NULL && limit != NULL && where > limit) ||
        (group != NULL && order != NULL && group > order) ||
        (group != NULL && limit != NULL && group > limit) ||
        (order != NULL && limit != NULL && order > limit)) {
        printf("ERR BAD SELECT\n");
        return;
    }
    where_buf[0] = '\0';
    group_buf[0] = '\0';
    order_buf[0] = '\0';
    limit_buf[0] = '\0';
    if (where != NULL) {
        int len;
        next = next_select_clause(NULL, group != NULL && group > where ?
                                  group : NULL,
                                  order != NULL && order > where ?
                                  order : NULL,
                                  limit != NULL && limit > where ?
                                  limit : NULL);
        len = next != NULL ? (int)(next - where) : (int)strlen(where);
        if (len >= MAX_STATEMENT) {
            printf("ERR STATEMENT TOO LONG\n");
            return;
        }
        strncpy(where_buf, where, len);
        where_buf[len] = '\0';
        rtrim(where_buf);
        where = where_buf;
    }
    if (group != NULL) {
        int len;
        next = next_select_clause(NULL, NULL,
                                  order != NULL && order > group ?
                                  order : NULL,
                                  limit != NULL && limit > group ?
                                  limit : NULL);
        len = next != NULL ? (int)(next - group) : (int)strlen(group);
        if (len >= MAX_STATEMENT) {
            printf("ERR STATEMENT TOO LONG\n");
            return;
        }
        strncpy(group_buf, group, len);
        group_buf[len] = '\0';
        rtrim(group_buf);
        group = group_buf;
        if (!parse_select_col_clause(group, "GROUP BY", col_name)) {
            printf("ERR BAD GROUP BY\n");
            return;
        }
        group_col = find_col(&tables[idx], col_name);
        if (group_col < 0) {
            printf("ERR BAD GROUP BY\n");
            return;
        }
    }
    if (order != NULL) {
        int len;
        next = limit != NULL && limit > order ? limit : NULL;
        len = next != NULL ? (int)(next - order) : (int)strlen(order);
        if (len >= MAX_STATEMENT) {
            printf("ERR STATEMENT TOO LONG\n");
            return;
        }
        strncpy(order_buf, order, len);
        order_buf[len] = '\0';
        rtrim(order_buf);
        order = order_buf;
        if (!parse_order_clause(&tables[idx], order, group_col, &order_col,
                                &order_count, &order_desc)) {
            printf("ERR BAD ORDER BY\n");
            return;
        }
    }
    if (limit != NULL) {
        strncpy(limit_buf, limit, MAX_STATEMENT - 1);
        limit_buf[MAX_STATEMENT - 1] = '\0';
        rtrim(limit_buf);
        limit = limit_buf;
        if (!parse_limit_clause(limit, &limit_count)) {
            printf("ERR BAD LIMIT\n");
            return;
        }
    }
    if (!parse_where(&tables[idx], where, &expr)) {
        printf("ERR BAD WHERE\n");
        return;
    }

    where_col = -1;
    where_val[0] = '\0';
    if (group_col < 0 && order == NULL &&
        where_simple_eq(&expr, &where_col, where_val)) {
        index_no = find_index_col(&tables[idx], where_col);
    } else {
        index_no = -1;
    }
    if (index_no >= 0) {
        struct KeySet keys;
        char prefix[KV_KEY];
        int prefix_len;

        keyset_init(&keys);
        kv_make_index_base_prefix(prefix, tables[idx].name,
                                  tables[idx].index_names[index_no],
                                  &prefix_len);
        if (!kv_scan(prefix, prefix_len, key_cb, &keys)) {
            keyset_free(&keys);
            printf("ERR CANNOT READ INDEX\n");
            return;
        }
        for (r = 0; r < keys.count; r++) {
            int slot;
            int found;
            struct Row row;

            slot = kv_index_slot(keys.keys[r]);
            if (slot < 1) {
                continue;
            }
            if (!load_row_slot(&tables[idx], slot, &row, &found)) {
                keyset_free(&keys);
                rowset_free(&out);
                printf("ERR CANNOT READ TABLE\n");
                return;
            }
            if (!found || !where_match(&tables[idx], &row, &expr)) {
                continue;
            }
            if (!rowset_add(&out, &row)) {
                keyset_free(&keys);
                rowset_free(&out);
                printf("ERR OUT OF MEMORY\n");
                return;
            }
        }
        keyset_free(&keys);
    } else {
        if (!load_rows(&tables[idx], &rows)) {
            rowset_free(&out);
            printf("ERR CANNOT READ TABLE\n");
            return;
        }
        for (r = 0; r < rows.count; r++) {
            if (!where_match(&tables[idx], &rows.rows[r], &expr)) {
                continue;
            }
            if (!rowset_add(&out, &rows.rows[r])) {
                rowset_free(&rows);
                rowset_free(&out);
                printf("ERR OUT OF MEMORY\n");
                return;
            }
        }
        rowset_free(&rows);
    }
    if (group_col >= 0) {
        if (!select_all) {
            for (r = 0; r < select_col_count; r++) {
                if (select_cols[r] != group_col) {
                    printf("ERR BAD SELECT LIST\n");
                    return;
                }
            }
            if (select_col_count == 0 && !select_count) {
                printf("ERR BAD SELECT LIST\n");
                return;
            }
        }
        groups = (struct GroupRow *)malloc(sizeof(struct GroupRow) *
                                           (out.count > 0 ? out.count : 1));
        if (groups == NULL) {
            rowset_free(&out);
            printf("ERR OUT OF MEMORY\n");
            return;
        }
        for (r = 0; r < out.count; r++) {
            if (!add_group_row(groups, out.count, &group_count,
                               out.rows[r].values[group_col])) {
                free(groups);
                rowset_free(&out);
                printf("ERR TOO MANY GROUPS\n");
                return;
            }
        }
        if (order != NULL) {
            g_sort_table = &tables[idx];
            g_sort_col = group_col;
            g_sort_desc = order_desc;
            g_group_sort_by_count = order_count;
            qsort(groups, group_count, sizeof(groups[0]), cmp_group_qsort);
        }
        count_width = 5;
        group_width = (int)strlen(tables[idx].cols[group_col]);
        for (r = 0; r < group_count &&
             (limit_count < 0 || r < limit_count); r++) {
            measure_cell(&group_width, groups[r].value);
            sprintf(count_text, "%d", groups[r].count);
            measure_cell(&count_width, count_text);
        }
        if (select_all || (select_col_count > 0 && select_count)) {
            printf("%-*s | COUNT\n", group_width, tables[idx].cols[group_col]);
            widths[0] = group_width;
            widths[1] = count_width;
            print_header_rule(widths, 2);
        } else if (select_count) {
            printf("COUNT\n");
            print_header_rule(&count_width, 1);
        } else {
            printf("%s\n", tables[idx].cols[group_col]);
            print_header_rule(&group_width, 1);
        }
        for (r = 0; r < group_count &&
             (limit_count < 0 || r < limit_count); r++) {
            if (select_all || (select_col_count > 0 && select_count)) {
                printf("%-*s | %d\n", group_width, groups[r].value,
                       groups[r].count);
            } else if (select_count) {
                printf("%d\n", groups[r].count);
            } else {
                printf("%s\n", groups[r].value);
            }
        }
        printf("OK %d GROUPS\n",
               limit_count >= 0 && group_count > limit_count ?
               limit_count : group_count);
        free(groups);
        rowset_free(&out);
        return;
    }
    if (select_count) {
        if (select_col_count > 0 || select_all) {
            rowset_free(&out);
            printf("ERR BAD SELECT LIST\n");
            return;
        }
        count_width = 5;
        sprintf(count_text, "%d", out.count);
        measure_cell(&count_width, count_text);
        printf("COUNT\n");
        print_header_rule(&count_width, 1);
        printf("%d\n", out.count);
        printf("OK 1 ROWS\n");
        rowset_free(&out);
        return;
    }
    if (order != NULL) {
        g_sort_table = &tables[idx];
        g_sort_col = order_col;
        g_sort_desc = order_desc;
        qsort(out.rows, out.count, sizeof(out.rows[0]), cmp_row_qsort);
    }
    if (select_all) {
        select_col_count = tables[idx].col_count;
        for (c = 0; c < select_col_count; c++) {
            select_cols[c] = c;
        }
    }
    for (c = 0; c < select_col_count; c++) {
        widths[c] = (int)strlen(tables[idx].cols[select_cols[c]]);
        for (r = 0; r < out.count &&
             (limit_count < 0 || r < limit_count); r++) {
            measure_cell(&widths[c], out.rows[r].values[select_cols[c]]);
        }
    }
    print_projected_line(&tables[idx], NULL, select_cols,
                         select_col_count, 1, widths);
    for (r = 0; r < out.count && (limit_count < 0 || r < limit_count); r++) {
        print_projected_line(&tables[idx], &out.rows[r], select_cols,
                             select_col_count, 0, widths);
    }
    printf("OK %d ROWS\n",
           limit_count >= 0 && out.count > limit_count ?
           limit_count : out.count);
    rowset_free(&out);
}

static void explain_select_join(struct TableDef tables[], int count, char *sql)
{
    char left_name[MAX_NAME + 1];
    char right_name[MAX_NAME + 1];
    char left_alias[MAX_NAME + 1];
    char right_alias[MAX_NAME + 1];
    char qleft_table[MAX_NAME + 1];
    char qleft_col[MAX_NAME + 1];
    char qright_table[MAX_NAME + 1];
    char qright_col[MAX_NAME + 1];
    char onbuf[MAX_STATEMENT];
    char *p;
    char *fromp;
    char *joinp;
    char *onp;
    char *eq;
    int i;
    int left_idx;
    int right_idx;
    int right_col;
    int right_index;

    fromp = find_keyword(sql + strlen("SELECT"), "FROM");
    if (fromp == NULL) {
        printf("PLAN ERROR BAD JOIN\n");
        return;
    }
    p = ltrim(fromp + strlen("FROM"));
    i = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (i < MAX_NAME) {
            left_name[i++] = (char)toupper((unsigned char)*p);
        }
        p++;
    }
    left_name[i] = '\0';
    joinp = find_keyword(p, "JOIN");
    if (!valid_name(left_name) || joinp == NULL ||
        !parse_alias_segment(p, joinp, left_alias)) {
        printf("PLAN ERROR BAD JOIN\n");
        return;
    }
    p = ltrim(joinp + strlen("JOIN"));
    i = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (i < MAX_NAME) {
            right_name[i++] = (char)toupper((unsigned char)*p);
        }
        p++;
    }
    right_name[i] = '\0';
    onp = find_keyword(p, "ON");
    if (!valid_name(right_name) || onp == NULL ||
        !parse_alias_segment(p, onp, right_alias)) {
        printf("PLAN ERROR BAD JOIN\n");
        return;
    }
    strncpy(onbuf, onp + strlen("ON"), MAX_STATEMENT - 1);
    onbuf[MAX_STATEMENT - 1] = '\0';
    eq = strchr(onbuf, '=');
    if (eq == NULL) {
        printf("PLAN ERROR BAD JOIN\n");
        return;
    }
    *eq = '\0';
    if (!parse_qualified_col(onbuf, qleft_table, qleft_col) ||
        !parse_qualified_col(eq + 1, qright_table, qright_col)) {
        printf("PLAN ERROR BAD JOIN\n");
        return;
    }
    left_idx = find_table(tables, count, left_name);
    right_idx = find_table(tables, count, right_name);
    if (left_idx < 0 || right_idx < 0) {
        printf("PLAN ERROR TABLE NOT FOUND\n");
        return;
    }
    if (!name_matches(qleft_table, left_name, left_alias) ||
        !name_matches(qright_table, right_name, right_alias)) {
        printf("PLAN ERROR JOIN ORDER\n");
        return;
    }
    right_col = find_col(&tables[right_idx], qright_col);
    if (right_col < 0) {
        printf("PLAN ERROR BAD JOIN COLUMN\n");
        return;
    }
    right_index = find_index_col(&tables[right_idx], right_col);
    printf("PLAN JOIN %s -> %s\n", left_name, right_name);
    printf("PLAN LEFT TABLE SCAN %s\n", left_name);
    if (right_index >= 0) {
        printf("PLAN RIGHT INDEX LOOKUP %s ON %s(%s)\n",
               tables[right_idx].index_names[right_index],
               tables[right_idx].name, tables[right_idx].cols[right_col]);
    } else {
        printf("PLAN RIGHT TABLE SCAN %s\n", right_name);
    }
}

void cmd_explain(struct TableDef tables[], int count, char *sql)
{
    char stmt[MAX_STATEMENT];
    char name[MAX_NAME + 1];
    char where_buf[MAX_STATEMENT];
    char *s;
    char *p;
    char *where;
    char *group;
    char *order;
    char *limit;
    char *next;
    int c;
    int idx;
    int where_col;
    int index_no;
    char where_val[MAX_VALUE + 1];
    struct WhereExpr expr;

    s = ltrim(sql + strlen("EXPLAIN"));
    if (!starts_i(s, "SELECT")) {
        printf("PLAN ERROR ONLY SELECT SUPPORTED\n");
        return;
    }
    strncpy(stmt, s, MAX_STATEMENT - 1);
    stmt[MAX_STATEMENT - 1] = '\0';
    if (find_keyword(stmt + strlen("SELECT"), "JOIN") != NULL) {
        explain_select_join(tables, count, stmt);
        return;
    }
    p = find_keyword(stmt + strlen("SELECT"), "FROM");
    if (p == NULL) {
        printf("PLAN ERROR BAD SELECT\n");
        return;
    }
    p += strlen("FROM");
    p = ltrim(p);
    c = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (c < MAX_NAME) {
            name[c++] = (char)toupper((unsigned char)*p);
        }
        p++;
    }
    name[c] = '\0';
    idx = find_table(tables, count, name);
    if (idx < 0) {
        printf("PLAN ERROR TABLE NOT FOUND\n");
        return;
    }
    p = ltrim(p);
    where = find_i(p, "WHERE");
    group = find_i(p, "GROUP BY");
    order = find_i(p, "ORDER BY");
    limit = find_i(p, "LIMIT");
    where_buf[0] = '\0';
    if (where != NULL) {
        int len;

        next = next_select_clause(NULL, group != NULL && group > where ?
                                  group : NULL,
                                  order != NULL && order > where ?
                                  order : NULL,
                                  limit != NULL && limit > where ?
                                  limit : NULL);
        len = next != NULL ? (int)(next - where) : (int)strlen(where);
        if (len >= MAX_STATEMENT) {
            printf("PLAN ERROR STATEMENT TOO LONG\n");
            return;
        }
        strncpy(where_buf, where, len);
        where_buf[len] = '\0';
        rtrim(where_buf);
        where = where_buf;
    }
    if (!parse_where(&tables[idx], where, &expr)) {
        printf("PLAN ERROR BAD WHERE\n");
        return;
    }
    where_col = -1;
    where_val[0] = '\0';
    index_no = -1;
    if (group == NULL && order == NULL &&
        where_simple_eq(&expr, &where_col, where_val)) {
        index_no = find_index_col(&tables[idx], where_col);
    }
    if (index_no >= 0) {
        printf("PLAN INDEX LOOKUP %s ON %s(%s)\n",
               tables[idx].index_names[index_no],
               tables[idx].name, tables[idx].cols[where_col]);
    } else {
        printf("PLAN TABLE SCAN %s\n", tables[idx].name);
    }
    if (where != NULL) {
        printf("PLAN FILTER %d CONDITION(S)\n", expr.cond_count);
    }
    if (group != NULL) {
        printf("PLAN GROUP\n");
    }
    if (order != NULL) {
        printf("PLAN SORT\n");
    }
    if (limit != NULL) {
        printf("PLAN LIMIT\n");
    }
}

