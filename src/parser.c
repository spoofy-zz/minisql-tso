#include "minisql.h"

static void upper_copy(char *dst, const char *src, int max);
static int parse_type_name(const char *s, int *type, int *len);
static int like_match(const char *text, const char *pat);
static int eval_cond(struct TableDef *t, struct Row *row,
                     struct WhereCond *cond);
static int parse_where_cond(struct TableDef *t,
                            char toks[][MAX_VALUE + 1],
                            int ntok, int *pos,
                            struct WhereCond *cond);

char *ltrim(char *s)
{
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    return s;
}

void rtrim(char *s)
{
    int n = (int)strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[n - 1] = '\0';
        n--;
    }
}

char *trim(char *s)
{
    s = ltrim(s);
    rtrim(s);
    return s;
}

int line_is_empty_input(const char *s)
{
    while (*s != '\0') {
        unsigned char c = (unsigned char)*s;
        if (!isspace(c)) {
            return 0;
        }
        s++;
    }
    return 1;
}

static void upper_copy(char *dst, const char *src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i] != '\0'; i++) {
        dst[i] = (char)toupper((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

int eqi(const char *a, const char *b)
{
    while (*a && *b) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

int starts_i(const char *s, const char *prefix)
{
    while (*prefix) {
        if (toupper((unsigned char)*s) != toupper((unsigned char)*prefix)) {
            return 0;
        }
        s++;
        prefix++;
    }
    return 1;
}

int line_starts_command(const char *s)
{
    return starts_i(s, ".QUIT") ||
           starts_i(s, "//QUIT") ||
           starts_i(s, "QUIT") ||
           starts_i(s, ".HELP") ||
           starts_i(s, "//HELP") ||
           starts_i(s, "HELP") ||
           starts_i(s, ".CLEAR") ||
           starts_i(s, "//CLEAR") ||
           starts_i(s, "CLEAR") ||
           starts_i(s, ".TABLES") ||
           starts_i(s, ".SCHEMA") ||
           starts_i(s, "DESC") ||
           starts_i(s, "DESCRIBE") ||
           starts_i(s, "CREATE TABLE") ||
           starts_i(s, "CREATE INDEX") ||
           starts_i(s, "DROP INDEX") ||
           starts_i(s, "EXPLAIN") ||
           starts_i(s, "INSERT INTO") ||
           starts_i(s, "SELECT") ||
           starts_i(s, "UPDATE") ||
           starts_i(s, "DELETE FROM") ||
           starts_i(s, "DROP TABLE") ||
           starts_i(s, "BEGIN") ||
           starts_i(s, "COMMIT") ||
           starts_i(s, "ROLLBACK") ||
           starts_i(s, "REPEAT") ||
           starts_i(s, ".REPEAT") ||
           starts_i(s, "!!") ||
           starts_i(s, "PF12");
}

int ncmp_i(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (b[i] == '\0') {
            return 0;
        }
        if (a[i] == '\0') {
            return -1;
        }
        if (toupper((unsigned char)a[i]) != toupper((unsigned char)b[i])) {
            return toupper((unsigned char)a[i]) - toupper((unsigned char)b[i]);
        }
    }
    return 0;
}

char *find_i(char *s, const char *needle)
{
    int n = (int)strlen(needle);
    char *p;
    for (p = s; *p; p++) {
        if (ncmp_i(p, needle, n) == 0) {
            return p;
        }
    }
    return NULL;
}

void clean_token_max(char *s, int max)
{
    char *t = trim(s);
    int len;
    if (t != s) {
        memmove(s, t, strlen(t) + 1);
    }
    len = (int)strlen(s);
    if (len >= 2 && s[0] == '\'' && s[len - 1] == '\'') {
        memmove(s, s + 1, len - 2);
        s[len - 2] = '\0';
    }
    upper_copy(s, s, max);
}

void clean_token(char *s)
{
    clean_token_max(s, MAX_VALUE + 1);
}

int valid_name(const char *s)
{
    int i;
    if (!isalpha((unsigned char)s[0])) {
        return 0;
    }
    for (i = 1; s[i]; i++) {
        if (!isalnum((unsigned char)s[i]) && s[i] != '_') {
            return 0;
        }
    }
    return (int)strlen(s) <= MAX_NAME;
}

static int parse_type_name(const char *s, int *type, int *len)
{
    char buf[MAX_VALUE + 1];
    char *open;
    char *close;
    int n;

    *type = TYPE_TEXT;
    *len = MAX_VALUE;
    strncpy(buf, s, MAX_VALUE);
    buf[MAX_VALUE] = '\0';
    clean_token(buf);
    if (buf[0] == '\0' || eqi(buf, "TEXT")) {
        return 1;
    }
    if (eqi(buf, "INT") || eqi(buf, "INTEGER")) {
        *type = TYPE_INT;
        *len = 0;
        return 1;
    }
    if (starts_i(buf, "CHAR(") || starts_i(buf, "VARCHAR(")) {
        open = strchr(buf, '(');
        close = strrchr(buf, ')');
        if (open == NULL || close == NULL || close <= open + 1) {
            return 0;
        }
        *close = '\0';
        n = atoi(open + 1);
        if (n < 1 || n > MAX_VALUE) {
            return 0;
        }
        *type = starts_i(buf, "CHAR(") ? TYPE_CHAR : TYPE_VARCHAR;
        *len = n;
        return 1;
    }
    return 0;
}

int parse_column_def(char *text, char *name, int *type, int *len)
{
    char buf[MAX_VALUE + 1];
    char *p;
    int i = 0;

    strncpy(buf, text, MAX_VALUE);
    buf[MAX_VALUE] = '\0';
    clean_token(buf);
    p = buf;
    while (*p && !isspace((unsigned char)*p)) {
        if (i < MAX_NAME) {
            name[i++] = *p;
        }
        p++;
    }
    name[i] = '\0';
    p = ltrim(p);
    if (!valid_name(name)) {
        return 0;
    }
    return parse_type_name(p, type, len);
}

void type_to_text(int type, int len, char *out, int max)
{
    if (type == TYPE_INT) {
        strncpy(out, "INT", max);
    } else if (type == TYPE_CHAR) {
        sprintf(out, "CHAR(%d)", len);
    } else if (type == TYPE_VARCHAR) {
        sprintf(out, "VARCHAR(%d)", len);
    } else {
        strncpy(out, "TEXT", max);
    }
    out[max - 1] = '\0';
}

int validate_value(struct TableDef *t, int col, const char *value)
{
    int i;
    int start = 0;
    int len = (int)strlen(value);

    if (len > MAX_VALUE) {
        printf("ERR VALUE TOO LONG\n");
        return 0;
    }
    if (t->col_types[col] == TYPE_INT) {
        if (value[0] == '-' || value[0] == '+') {
            start = 1;
        }
        if (value[start] == '\0') {
            printf("ERR BAD INT VALUE\n");
            return 0;
        }
        for (i = start; value[i] != '\0'; i++) {
            if (!isdigit((unsigned char)value[i])) {
                printf("ERR BAD INT VALUE\n");
                return 0;
            }
        }
    } else if (t->col_types[col] == TYPE_CHAR ||
               t->col_types[col] == TYPE_VARCHAR) {
        if (len > t->col_lens[col]) {
            printf("ERR VALUE TOO LONG\n");
            return 0;
        }
    }
    return 1;
}

int cmp_value(struct TableDef *t, int col,
                     const char *left, const char *right)
{
    long a;
    long b;

    if (t->col_types[col] == TYPE_INT) {
        a = atol(left);
        b = atol(right);
        if (a < b) {
            return -1;
        }
        if (a > b) {
            return 1;
        }
        return 0;
    }
    return strcmp(left, right);
}

static int like_match(const char *text, const char *pat)
{
    if (*pat == '\0') {
        return *text == '\0';
    }
    if (*pat == '%') {
        while (*pat == '%') {
            pat++;
        }
        if (*pat == '\0') {
            return 1;
        }
        while (*text != '\0') {
            if (like_match(text, pat)) {
                return 1;
            }
            text++;
        }
        return like_match(text, pat);
    }
    if (*pat == '_') {
        return *text != '\0' && like_match(text + 1, pat + 1);
    }
    if (toupper((unsigned char)*text) !=
        toupper((unsigned char)*pat)) {
        return 0;
    }
    return like_match(text + 1, pat + 1);
}

static int eval_cond(struct TableDef *t, struct Row *row,
                     struct WhereCond *cond)
{
    const char *val = row->values[cond->col];
    int cmp1;
    int cmp2;

    if (cond->op == OP_EQ) {
        return eqi(val, cond->value1);
    }
    if (cond->op == OP_LT) {
        return cmp_value(t, cond->col, val, cond->value1) < 0;
    }
    if (cond->op == OP_GT) {
        return cmp_value(t, cond->col, val, cond->value1) > 0;
    }
    if (cond->op == OP_LE) {
        return cmp_value(t, cond->col, val, cond->value1) <= 0;
    }
    if (cond->op == OP_GE) {
        return cmp_value(t, cond->col, val, cond->value1) >= 0;
    }
    if (cond->op == OP_NE) {
        return !eqi(val, cond->value1);
    }
    if (cond->op == OP_LIKE) {
        return like_match(val, cond->value1);
    }
    if (cond->op == OP_BETWEEN) {
        cmp1 = cmp_value(t, cond->col, val, cond->value1);
        cmp2 = cmp_value(t, cond->col, val, cond->value2);
        return cmp1 >= 0 && cmp2 <= 0;
    }
    return 0;
}

int where_match(struct TableDef *t, struct Row *row,
                       struct WhereExpr *expr)
{
    int i;
    int any_or;
    int current;

    if (expr->cond_count == 0) {
        return 1;
    }
    any_or = 0;
    current = eval_cond(t, row, &expr->conds[0]);
    for (i = 1; i < expr->cond_count; i++) {
        if (expr->logic[i - 1] == LOGIC_AND) {
            current = current && eval_cond(t, row, &expr->conds[i]);
        } else {
            any_or = any_or || current;
            current = eval_cond(t, row, &expr->conds[i]);
        }
    }
    return any_or || current;
}

int where_simple_eq(struct WhereExpr *expr, int *col, char *value)
{
    if (expr->cond_count != 1 || expr->conds[0].op != OP_EQ) {
        return 0;
    }
    *col = expr->conds[0].col;
    strncpy(value, expr->conds[0].value1, MAX_VALUE);
    value[MAX_VALUE] = '\0';
    return 1;
}

int parse_csv_limit(char *text, char values[][MAX_VALUE + 1],
                           int *count, int limit)
{
    int n = 0;
    char *p = text;
    char *start = text;
    int in_quote = 0;

    while (1) {
        if (*p == '\'') {
            in_quote = !in_quote;
        }
        if ((*p == ',' && !in_quote) || *p == '\0') {
            char save = *p;
            if (n >= limit) {
                return 0;
            }
            *p = '\0';
            strncpy(values[n], start, MAX_VALUE);
            values[n][MAX_VALUE] = '\0';
            clean_token(values[n]);
            n++;
            if (save == '\0') {
                break;
            }
            start = p + 1;
        }
        p++;
    }

    *count = n;
    return 1;
}

int parse_csv(char *text, char values[MAX_COLS][MAX_VALUE + 1],
                     int *count)
{
    return parse_csv_limit(text, values, count, MAX_COLS);
}

int parse_def_csv(char *text, char values[MAX_DEFS][MAX_DEF_TEXT + 1],
                         int *count)
{
    int n = 0;
    char *p = text;
    char *start = text;
    int in_quote = 0;
    int paren = 0;

    while (1) {
        if (*p == '\'') {
            in_quote = !in_quote;
        } else if (*p == '(' && !in_quote) {
            paren++;
        } else if (*p == ')' && !in_quote && paren > 0) {
            paren--;
        }
        if ((*p == ',' && !in_quote && paren == 0) || *p == '\0') {
            char save = *p;
            if (n >= MAX_DEFS) {
                return 0;
            }
            *p = '\0';
            strncpy(values[n], start, MAX_DEF_TEXT);
            values[n][MAX_DEF_TEXT] = '\0';
            clean_token_max(values[n], MAX_DEF_TEXT + 1);
            n++;
            if (save == '\0') {
                break;
            }
            start = p + 1;
        }
        p++;
    }
    *count = n;
    return 1;
}

int parse_name_after(char *sql, const char *prefix, char *name)
{
    char *p = sql + strlen(prefix);
    int i = 0;
    p = ltrim(p);
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (i < MAX_NAME) {
            name[i++] = (char)toupper((unsigned char)*p);
        }
        p++;
    }
    name[i] = '\0';
    return i > 0 && valid_name(name);
}

int where_tokenize(char *text,
                          char toks[][MAX_VALUE + 1], int *count)
{
    char *p = text;
    int n = 0;

    while (*p != '\0') {
        int i = 0;

        p = ltrim(p);
        if (*p == '\0') {
            break;
        }
        if (n >= MAX_CONDS * 5) {
            return 0;
        }
        if (*p == '\'' || *p == '"') {
            char quote = *p++;
            while (*p != '\0' && *p != quote) {
                if (i < MAX_VALUE) {
                    toks[n][i++] = *p;
                }
                p++;
            }
            if (*p == quote) {
                p++;
            }
        } else if (*p == '=' || *p == '<' || *p == '>' || *p == '!') {
            toks[n][i++] = *p++;
            if ((*toks[n] == '<' || *toks[n] == '>' || *toks[n] == '!') &&
                *p == '=') {
                toks[n][i++] = *p++;
            } else if (*toks[n] == '<' && *p == '>') {
                toks[n][i++] = *p++;
            }
        } else {
            while (*p != '\0' && !isspace((unsigned char)*p) &&
                   *p != '=' && *p != '<' && *p != '>' && *p != '!') {
                if (i < MAX_VALUE) {
                    toks[n][i++] = *p;
                }
                p++;
            }
        }
        toks[n][i] = '\0';
        clean_token(toks[n]);
        n++;
    }
    *count = n;
    return 1;
}

static int parse_where_cond(struct TableDef *t,
                            char toks[][MAX_VALUE + 1],
                            int ntok, int *pos,
                            struct WhereCond *cond)
{
    int col;

    if (*pos + 2 >= ntok) {
        return 0;
    }
    col = find_col(t, toks[*pos]);
    if (col < 0) {
        return 0;
    }
    cond->col = col;
    (*pos)++;
    if (eqi(toks[*pos], "LIKE")) {
        cond->op = OP_LIKE;
        (*pos)++;
        if (*pos >= ntok) {
            return 0;
        }
        strncpy(cond->value1, toks[*pos], MAX_VALUE);
        cond->value1[MAX_VALUE] = '\0';
        cond->value2[0] = '\0';
        (*pos)++;
        return 1;
    }
    if (eqi(toks[*pos], "BETWEEN")) {
        cond->op = OP_BETWEEN;
        (*pos)++;
        if (*pos + 2 >= ntok || !eqi(toks[*pos + 1], "AND")) {
            return 0;
        }
        strncpy(cond->value1, toks[*pos], MAX_VALUE);
        cond->value1[MAX_VALUE] = '\0';
        strncpy(cond->value2, toks[*pos + 2], MAX_VALUE);
        cond->value2[MAX_VALUE] = '\0';
        if (!validate_value(t, col, cond->value1) ||
            !validate_value(t, col, cond->value2)) {
            return 0;
        }
        *pos += 3;
        return 1;
    }
    if (eqi(toks[*pos], "=")) {
        cond->op = OP_EQ;
    } else if (eqi(toks[*pos], "<")) {
        cond->op = OP_LT;
    } else if (eqi(toks[*pos], ">")) {
        cond->op = OP_GT;
    } else if (eqi(toks[*pos], "<=")) {
        cond->op = OP_LE;
    } else if (eqi(toks[*pos], ">=")) {
        cond->op = OP_GE;
    } else if (eqi(toks[*pos], "<>") || eqi(toks[*pos], "!=")) {
        cond->op = OP_NE;
    } else {
        return 0;
    }
    (*pos)++;
    if (*pos >= ntok) {
        return 0;
    }
    strncpy(cond->value1, toks[*pos], MAX_VALUE);
    cond->value1[MAX_VALUE] = '\0';
    cond->value2[0] = '\0';
    if (!validate_value(t, col, cond->value1)) {
        return 0;
    }
    (*pos)++;
    return 1;
}

int parse_where(struct TableDef *t, char *where_text,
                       struct WhereExpr *expr)
{
    char toks[MAX_CONDS * 5][MAX_VALUE + 1];
    int ntok = 0;
    int pos = 0;

    expr->cond_count = 0;
    if (where_text == NULL) {
        return 1;
    }
    where_text = ltrim(where_text);
    if (starts_i(where_text, "WHERE")) {
        where_text += strlen("WHERE");
    }
    if (!where_tokenize(where_text, toks, &ntok) || ntok == 0) {
        return 0;
    }
    while (pos < ntok) {
        if (expr->cond_count >= MAX_CONDS) {
            return 0;
        }
        if (!parse_where_cond(t, toks, ntok, &pos,
                              &expr->conds[expr->cond_count])) {
            return 0;
        }
        expr->cond_count++;
        if (pos >= ntok) {
            break;
        }
        if (expr->cond_count >= MAX_CONDS) {
            return 0;
        }
        if (eqi(toks[pos], "AND")) {
            expr->logic[expr->cond_count - 1] = LOGIC_AND;
        } else if (eqi(toks[pos], "OR")) {
            expr->logic[expr->cond_count - 1] = LOGIC_OR;
        } else {
            return 0;
        }
        pos++;
    }
    return 1;
}

