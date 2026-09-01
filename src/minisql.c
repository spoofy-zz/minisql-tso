#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdarg.h>
#ifdef __MVS__
#include <clibvsam.h>
#endif

#if defined(__MVS__) && defined(MINISQL_TSO)
extern int msqtget(char *buf, int max) asm("MSQTGET");
extern int msqtput(char *buf, int len) asm("MSQTPUT");
#endif

#define MAX_LINE 1024
#define MAX_NAME 16
#define MAX_COLS 8
#define MAX_VALUE 32
#define MAX_ROWS 32
#define MAX_TABLES 32
#define MAX_STATEMENT 2048
#define KV_KEY 32
#define KV_DATA 48
#define KV_RECLEN (KV_KEY + KV_DATA)
#define KV_DD "MINIKV"

struct TableDef {
    char name[MAX_NAME + 1];
    int col_count;
    char cols[MAX_COLS][MAX_NAME + 1];
};

struct Row {
    char values[MAX_COLS][MAX_VALUE + 1];
};

struct KvRec {
    char key[KV_KEY];
    char data[KV_DATA];
};

static void rtrim(char *s);

#if defined(__MVS__) && defined(MINISQL_TSO)
static char g_tso_out[MAX_LINE];
static int g_tso_out_len = 0;

static void tso_flush_line(void)
{
    if (g_tso_out_len > 0) {
        g_tso_out[g_tso_out_len] = '\0';
        msqtput(g_tso_out, g_tso_out_len);
        g_tso_out_len = 0;
    }
}

static int msql_fflush(FILE *fp)
{
    (void)fp;
    tso_flush_line();
    return 0;
}

static int msql_printf(const char *fmt, ...)
{
    char tmp[MAX_LINE];
    va_list ap;
    int n;
    int i;

    va_start(ap, fmt);
    n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return n;
    }
    tmp[sizeof(tmp) - 1] = '\0';

    for (i = 0; tmp[i] != '\0'; i++) {
        if (tmp[i] == '\n') {
            tso_flush_line();
        } else {
            if (g_tso_out_len >= (int)sizeof(g_tso_out) - 1) {
                tso_flush_line();
            }
            g_tso_out[g_tso_out_len++] = tmp[i];
        }
    }
    return n;
}

#define printf msql_printf
#define fflush msql_fflush
#endif

#ifdef __MVS__
static VSFILE *g_kv = NULL;
#else
static struct KvRec g_host_kv[2048];
static int g_host_kv_count = 0;
#endif

static void kv_make_key(char *out, const char *kind, const char *name, int seq)
{
    char tmp[KV_KEY + 1];
    memset(out, ' ', KV_KEY);
    if (seq >= 0) {
        sprintf(tmp, "%s|%-16.16s|%06d", kind, name, seq);
    } else {
        sprintf(tmp, "%s|%-16.16s", kind, name);
    }
    memcpy(out, tmp, strlen(tmp));
}

static void kv_make_prefix(char *out, const char *kind,
                           const char *name, int *len)
{
    char tmp[KV_KEY + 1];
    if (name == NULL || name[0] == '\0') {
        sprintf(tmp, "%s|", kind);
        *len = 2;
    } else {
        sprintf(tmp, "%s|%-16.16s|", kind, name);
        *len = 19;
    }
    memset(out, ' ', KV_KEY);
    memcpy(out, tmp, strlen(tmp));
}

static int key_has_prefix(const char *key, const char *prefix, int len)
{
    return memcmp(key, prefix, len) == 0;
}

static void data_put(char *dst, const char *src)
{
    memset(dst, ' ', KV_DATA);
    strncpy(dst, src, KV_DATA);
}

static void data_get(char *dst, const char *src, int max)
{
    int n;
    if (max < 1) {
        return;
    }
    n = max - 1;
    if (n > KV_DATA) {
        n = KV_DATA;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    rtrim(dst);
}

static int kv_open(void)
{
#ifdef __MVS__
    if (g_kv != NULL) {
        return 1;
    }
    return __vsopen(KV_DD, VSTYPE_KSDS, VSACCESS_DYNAM, VSMODE_UPD, &g_kv) == 0;
#else
    return 1;
#endif
}

static int kv_close(void)
{
#ifdef __MVS__
    if (g_kv != NULL) {
        __vsclos(g_kv);
        g_kv = NULL;
    }
#endif
    return 1;
}

static int kv_get(const char *key, char *data, int max)
{
    struct KvRec rec;
#ifdef __MVS__
    int rc;
    if (!kv_open()) {
        return 0;
    }
    memset(&rec, ' ', sizeof(rec));
    rc = __vsread(g_kv, &rec, sizeof(rec), (void *)key, KV_KEY);
    if (rc < 0) {
        __vsclr(g_kv);
        return 0;
    }
    data_get(data, rec.data, max);
    return 1;
#else
    int i;
    for (i = 0; i < g_host_kv_count; i++) {
        if (memcmp(g_host_kv[i].key, key, KV_KEY) == 0) {
            data_get(data, g_host_kv[i].data, max);
            return 1;
        }
    }
    return 0;
#endif
}

static int kv_put(const char *key, const char *data)
{
    struct KvRec rec;
#ifdef __MVS__
    char old[KV_DATA + 1];
#else
    int i;
#endif
    memset(&rec, ' ', sizeof(rec));
    memcpy(rec.key, key, KV_KEY);
    data_put(rec.data, data);
#ifdef __MVS__
    if (!kv_open()) {
        return 0;
    }
    if (kv_get(key, old, sizeof(old))) {
        __vsread(g_kv, &rec, sizeof(rec), (void *)key, KV_KEY);
        return __vsupdt(g_kv, &rec, sizeof(rec)) == 0;
    }
    return __vswrit(g_kv, &rec, sizeof(rec), (void *)key, KV_KEY) == 0;
#else
    for (i = 0; i < g_host_kv_count; i++) {
        if (memcmp(g_host_kv[i].key, key, KV_KEY) == 0) {
            g_host_kv[i] = rec;
            return 1;
        }
    }
    if (g_host_kv_count >= (int)(sizeof(g_host_kv) / sizeof(g_host_kv[0]))) {
        return 0;
    }
    g_host_kv[g_host_kv_count++] = rec;
    return 1;
#endif
}

static int kv_delete(const char *key)
{
#ifdef __MVS__
    struct KvRec rec;
    int rc;
    if (!kv_open()) {
        return 0;
    }
    memset(&rec, ' ', sizeof(rec));
    rc = __vsread(g_kv, &rec, sizeof(rec), (void *)key, KV_KEY);
    if (rc < 0) {
        __vsclr(g_kv);
        return 1;
    }
    return __vsdel(g_kv, &rec, sizeof(rec)) == 0;
#else
    int i;
    int j;
    for (i = 0; i < g_host_kv_count; i++) {
        if (memcmp(g_host_kv[i].key, key, KV_KEY) == 0) {
            for (j = i; j < g_host_kv_count - 1; j++) {
                g_host_kv[j] = g_host_kv[j + 1];
            }
            g_host_kv_count--;
            return 1;
        }
    }
    return 1;
#endif
}

static int kv_scan(const char *prefix, int prefix_len,
                   int (*cb)(const char *key, const char *data, void *arg),
                   void *arg)
{
#ifdef __MVS__
    struct KvRec rec;
    int rc;
    if (!kv_open()) {
        return 0;
    }
    memset(&rec, ' ', sizeof(rec));
    if (__vsstge(g_kv, &rec, sizeof(rec), (void *)prefix, KV_KEY) != 0) {
        __vsclr(g_kv);
        return 1;
    }
    while ((rc = __vsread(g_kv, &rec, sizeof(rec), NULL, 0)) >= 0) {
        char data[KV_DATA + 1];
        if (!key_has_prefix(rec.key, prefix, prefix_len)) {
            break;
        }
        data_get(data, rec.data, sizeof(data));
        if (!cb(rec.key, data, arg)) {
            break;
        }
    }
    return rc == -2 ? 0 : 1;
#else
    int i;
    for (i = 0; i < g_host_kv_count; i++) {
        char data[KV_DATA + 1];
        if (key_has_prefix(g_host_kv[i].key, prefix, prefix_len)) {
            data_get(data, g_host_kv[i].data, sizeof(data));
            if (!cb(g_host_kv[i].key, data, arg)) {
                break;
            }
        }
    }
    return 1;
#endif
}

static char *ltrim(char *s)
{
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    return s;
}

static void rtrim(char *s)
{
    int n = (int)strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[n - 1] = '\0';
        n--;
    }
}

static char *trim(char *s)
{
    s = ltrim(s);
    rtrim(s);
    return s;
}

static void upper_copy(char *dst, const char *src, int max)
{
    int i;
    for (i = 0; i < max - 1 && src[i] != '\0'; i++) {
        dst[i] = (char)toupper((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

static int eqi(const char *a, const char *b)
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

static int starts_i(const char *s, const char *prefix)
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

static int line_starts_command(const char *s)
{
    return starts_i(s, ".QUIT") ||
           starts_i(s, "//QUIT") ||
           starts_i(s, "QUIT") ||
           starts_i(s, ".HELP") ||
           starts_i(s, "//HELP") ||
           starts_i(s, "HELP") ||
           starts_i(s, ".TABLES") ||
           starts_i(s, ".SCHEMA") ||
           starts_i(s, "CREATE TABLE") ||
           starts_i(s, "INSERT INTO") ||
           starts_i(s, "SELECT") ||
           starts_i(s, "UPDATE") ||
           starts_i(s, "DELETE FROM") ||
           starts_i(s, "DROP TABLE");
}

static void normalize_terminal_line(char *s, int max)
{
    int i;
    int end;

    for (i = 0; i < max; i++) {
        if (line_starts_command(s + i)) {
            if (i > 0) {
                memmove(s, s + i, max - i);
                memset(s + max - i, 0, i);
            }
            break;
        }
    }

    s[max - 1] = '\0';
    for (end = max - 2; end >= 0; end--) {
        unsigned char c = (unsigned char)s[end];
        if (c != '\0' && !isspace(c)) {
            s[end + 1] = '\0';
            return;
        }
        s[end] = '\0';
    }
}

static int ncmp_i(const char *a, const char *b, int n)
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

static char *find_i(char *s, const char *needle)
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

static void clean_token(char *s)
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
    upper_copy(s, s, MAX_VALUE + 1);
}

static int valid_name(const char *s)
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

static int parse_csv(char *text, char values[MAX_COLS][MAX_VALUE + 1],
                     int *count)
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
            if (n >= MAX_COLS) {
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

struct CatalogScan {
    struct TableDef *tables;
    int count;
    int ok;
};

static int catalog_cb(const char *key, const char *data, void *arg)
{
    struct CatalogScan *scan = (struct CatalogScan *)arg;
    char buf[KV_DATA + 1];
    char *tok;
    int c = 0;

    (void)key;
    if (scan->count >= MAX_TABLES) {
        scan->ok = 0;
        return 0;
    }
    strncpy(buf, data, KV_DATA);
    buf[KV_DATA] = '\0';
    tok = strtok(buf, "|");
    if (tok == NULL) {
        return 1;
    }
    strncpy(scan->tables[scan->count].name, tok, MAX_NAME);
    scan->tables[scan->count].name[MAX_NAME] = '\0';
    while ((tok = strtok(NULL, "|")) != NULL && c < MAX_COLS) {
        strncpy(scan->tables[scan->count].cols[c], tok, MAX_NAME);
        scan->tables[scan->count].cols[c][MAX_NAME] = '\0';
        rtrim(scan->tables[scan->count].cols[c]);
        c++;
    }
    scan->tables[scan->count].col_count = c;
    scan->count++;
    return 1;
}

static int load_catalog(struct TableDef tables[], int *count)
{
    char prefix[KV_KEY];
    int prefix_len;
    struct CatalogScan scan;

    kv_make_prefix(prefix, "T", "", &prefix_len);
    scan.tables = tables;
    scan.count = 0;
    scan.ok = 1;
    if (!kv_scan(prefix, prefix_len, catalog_cb, &scan)) {
        return 0;
    }
    *count = scan.count;
    return scan.ok;
}

static int save_catalog(struct TableDef tables[], int count)
{
    int i;
    int c;

    for (i = 0; i < count; i++) {
        char key[KV_KEY];
        kv_make_key(key, "T", tables[i].name, -1);
        if (!kv_delete(key)) {
            return 0;
        }
    }
    for (i = 0; i < count; i++) {
        char key[KV_KEY];
        char data[KV_DATA + 1];
        strcpy(data, tables[i].name);
        for (c = 0; c < tables[i].col_count; c++) {
            if ((int)strlen(data) + 1 + (int)strlen(tables[i].cols[c]) >
                KV_DATA) {
                return 0;
            }
            strcat(data, "|");
            strcat(data, tables[i].cols[c]);
        }
        kv_make_key(key, "T", tables[i].name, -1);
        if (!kv_put(key, data)) {
            return 0;
        }
    }
    return 1;
}

static int find_table(struct TableDef tables[], int count, const char *name)
{
    int i;
    for (i = 0; i < count; i++) {
        if (eqi(tables[i].name, name)) {
            return i;
        }
    }
    return -1;
}

static int find_col(struct TableDef *t, const char *name)
{
    int i;
    for (i = 0; i < t->col_count; i++) {
        if (eqi(t->cols[i], name)) {
            return i;
        }
    }
    return -1;
}

static int parse_name_after(char *sql, const char *prefix, char *name)
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

struct RowScan {
    struct TableDef *table;
    struct Row *rows;
    int count;
    int ok;
};

static int row_cb(const char *key, const char *data, void *arg)
{
    struct RowScan *scan = (struct RowScan *)arg;
    char vals[MAX_COLS][MAX_VALUE + 1];
    char buf[KV_DATA + 1];
    int cnt = 0;
    int i;

    (void)key;
    if (scan->count >= MAX_ROWS) {
        scan->ok = 0;
        return 0;
    }
    strncpy(buf, data, KV_DATA);
    buf[KV_DATA] = '\0';
    if (!parse_csv(buf, vals, &cnt) || cnt != scan->table->col_count) {
        scan->ok = 0;
        return 0;
    }
    for (i = 0; i < cnt; i++) {
        strncpy(scan->rows[scan->count].values[i], vals[i], MAX_VALUE);
        scan->rows[scan->count].values[i][MAX_VALUE] = '\0';
    }
    scan->count++;
    return 1;
}

static int load_rows(struct TableDef *t, struct Row rows[], int *row_count)
{
    char prefix[KV_KEY];
    int prefix_len;
    struct RowScan scan;

    kv_make_prefix(prefix, "R", t->name, &prefix_len);
    scan.table = t;
    scan.rows = rows;
    scan.count = 0;
    scan.ok = 1;
    if (!kv_scan(prefix, prefix_len, row_cb, &scan)) {
        return 0;
    }
    *row_count = scan.count;
    return scan.ok;
}

static int save_rows(struct TableDef *t, struct Row rows[], int row_count)
{
    int r;
    int c;

    for (r = 1; r <= MAX_ROWS; r++) {
        char key[KV_KEY];
        kv_make_key(key, "R", t->name, r);
        if (!kv_delete(key)) {
            return 0;
        }
    }
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
    return 1;
}

static int where_match(struct Row *row, int where_col, const char *where_val)
{
    if (where_col < 0) {
        return 1;
    }
    return eqi(row->values[where_col], where_val);
}

static void cmd_tables(struct TableDef tables[], int count)
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

static void cmd_schema(struct TableDef tables[], int count, char *sql)
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
        if (c > 0) {
            printf(", ");
        }
        printf("%s", tables[idx].cols[c]);
    }
    printf(")\n");
}

static void cmd_create(struct TableDef tables[], int *count, char *sql)
{
    char *p;
    char *q;
    char name[MAX_NAME + 1];
    char vals[MAX_COLS][MAX_VALUE + 1];
    int col_count = 0;
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
    if (!parse_csv(p + 1, vals, &col_count) || col_count < 1) {
        printf("ERR BAD COLUMN LIST\n");
        return;
    }
    if (*count >= MAX_TABLES) {
        printf("ERR TOO MANY TABLES\n");
        return;
    }
    strncpy(tables[*count].name, name, MAX_NAME);
    tables[*count].name[MAX_NAME] = '\0';
    tables[*count].col_count = col_count;
    for (i = 0; i < col_count; i++) {
        if (!valid_name(vals[i])) {
            printf("ERR BAD COLUMN NAME\n");
            return;
        }
        strncpy(tables[*count].cols[i], vals[i], MAX_NAME);
        tables[*count].cols[i][MAX_NAME] = '\0';
    }
    (*count)++;
    if (!save_catalog(tables, *count)) {
        printf("ERR CANNOT WRITE CATALOG\n");
        return;
    }
    printf("OK TABLE CREATED\n");
}

static void cmd_insert(struct TableDef tables[], int count, char *sql)
{
    char name[MAX_NAME + 1];
    char vals[MAX_COLS][MAX_VALUE + 1];
    char *p;
    char *q;
    int idx;
    int val_count = 0;
    int i;
    int row_count = 0;
    struct Row rows[MAX_ROWS];

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
    if (!load_rows(&tables[idx], rows, &row_count)) {
        printf("ERR CANNOT READ TABLE\n");
        return;
    }
    if (row_count >= MAX_ROWS) {
        printf("ERR TOO MANY ROWS\n");
        return;
    }
    for (i = 0; i < val_count; i++) {
        strncpy(rows[row_count].values[i], vals[i], MAX_VALUE);
        rows[row_count].values[i][MAX_VALUE] = '\0';
    }
    if (!save_rows(&tables[idx], rows, row_count + 1)) {
        printf("ERR CANNOT WRITE TABLE\n");
        return;
    }
    printf("OK 1 ROW INSERTED\n");
}

static void cmd_select(struct TableDef tables[], int count, char *sql)
{
    char name[MAX_NAME + 1];
    char *p;
    int idx;
    int r;
    int c;
    int row_count = 0;
    struct Row rows[MAX_ROWS];

    if (!starts_i(sql, "SELECT * FROM")) {
        printf("ERR ONLY SELECT * FROM table IS SUPPORTED\n");
        return;
    }
    p = sql + strlen("SELECT * FROM");
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
    if (!load_rows(&tables[idx], rows, &row_count)) {
        printf("ERR CANNOT READ TABLE\n");
        return;
    }
    for (c = 0; c < tables[idx].col_count; c++) {
        if (c > 0) {
            printf(" | ");
        }
        printf("%s", tables[idx].cols[c]);
    }
    printf("\n");
    for (r = 0; r < row_count; r++) {
        for (c = 0; c < tables[idx].col_count; c++) {
            if (c > 0) {
                printf(" | ");
            }
            printf("%s", rows[r].values[c]);
        }
        printf("\n");
    }
    printf("OK %d ROWS\n", row_count);
}

static int parse_where(struct TableDef *t, char *where_text,
                       int *where_col, char *where_val)
{
    char *eq;
    char col[MAX_VALUE + 1];

    *where_col = -1;
    where_val[0] = '\0';
    if (where_text == NULL) {
        return 1;
    }
    where_text = ltrim(where_text);
    if (starts_i(where_text, "WHERE")) {
        where_text += strlen("WHERE");
    }
    eq = strchr(where_text, '=');
    if (eq == NULL) {
        return 0;
    }
    *eq = '\0';
    strncpy(col, where_text, MAX_VALUE);
    col[MAX_VALUE] = '\0';
    clean_token(col);
    strncpy(where_val, eq + 1, MAX_VALUE);
    where_val[MAX_VALUE] = '\0';
    clean_token(where_val);
    *where_col = find_col(t, col);
    return *where_col >= 0;
}

static void cmd_delete(struct TableDef tables[], int count, char *sql)
{
    char name[MAX_NAME + 1];
    char *p;
    char *where;
    int idx;
    int i = 0;
    int r;
    int kept = 0;
    int deleted = 0;
    int row_count = 0;
    int where_col;
    char where_val[MAX_VALUE + 1];
    struct Row rows[MAX_ROWS];
    struct Row out[MAX_ROWS];

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
    if (!parse_where(&tables[idx], where, &where_col, where_val)) {
        printf("ERR BAD WHERE\n");
        return;
    }
    if (!load_rows(&tables[idx], rows, &row_count)) {
        printf("ERR CANNOT READ TABLE\n");
        return;
    }
    for (r = 0; r < row_count; r++) {
        if (where_match(&rows[r], where_col, where_val)) {
            deleted++;
        } else {
            out[kept++] = rows[r];
        }
    }
    if (!save_rows(&tables[idx], out, kept)) {
        printf("ERR CANNOT WRITE TABLE\n");
        return;
    }
    printf("OK %d ROWS DELETED\n", deleted);
}

static void cmd_update(struct TableDef tables[], int count, char *sql)
{
    char name[MAX_NAME + 1];
    char *p;
    char *setp;
    char *where;
    char *eq;
    char set_col_name[MAX_VALUE + 1];
    char set_val[MAX_VALUE + 1];
    char where_val[MAX_VALUE + 1];
    int where_col;
    int set_col;
    int idx;
    int i = 0;
    int r;
    int changed = 0;
    int row_count = 0;
    struct Row rows[MAX_ROWS];

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
    if (!parse_where(&tables[idx], where, &where_col, where_val)) {
        printf("ERR BAD WHERE\n");
        return;
    }
    if (!load_rows(&tables[idx], rows, &row_count)) {
        printf("ERR CANNOT READ TABLE\n");
        return;
    }
    for (r = 0; r < row_count; r++) {
        if (where_match(&rows[r], where_col, where_val)) {
            strncpy(rows[r].values[set_col], set_val, MAX_VALUE);
            rows[r].values[set_col][MAX_VALUE] = '\0';
            changed++;
        }
    }
    if (!save_rows(&tables[idx], rows, row_count)) {
        printf("ERR CANNOT WRITE TABLE\n");
        return;
    }
    printf("OK %d ROWS UPDATED\n", changed);
}

static void cmd_drop(struct TableDef tables[], int *count, char *sql)
{
    char name[MAX_NAME + 1];
    char key[KV_KEY];
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
    kv_make_key(key, "T", tables[idx].name, -1);
    if (!kv_delete(key)) {
        printf("ERR CANNOT DELETE CATALOG\n");
        return;
    }
    for (i = 1; i <= MAX_ROWS; i++) {
        kv_make_key(key, "R", tables[idx].name, i);
        if (!kv_delete(key)) {
            printf("ERR CANNOT DELETE TABLE\n");
            return;
        }
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

static void cmd_help(void)
{
    printf("COMMANDS:\n");
    printf("  CREATE TABLE name (col1, col2, ...);\n");
    printf("  INSERT INTO name VALUES (v1, v2, ...);\n");
    printf("  SELECT * FROM name [WHERE col=value];\n");
    printf("  UPDATE name SET col=value WHERE col=value;\n");
    printf("  DELETE FROM name WHERE col=value;\n");
    printf("  DROP TABLE name;\n");
    printf("  .TABLES\n");
    printf("  .SCHEMA name\n");
    printf("  .HELP or //HELP\n");
    printf("  .QUIT\n");
}

static void execute(char *sql)
{
    struct TableDef tables[MAX_TABLES];
    int table_count = 0;
    char *s = trim(sql);
    int len = (int)strlen(s);

    if (len > 0 && s[len - 1] == ';') {
        s[len - 1] = '\0';
        s = trim(s);
    }
    if (*s == '\0') {
        return;
    }
    if (eqi(s, ".HELP") || eqi(s, "//HELP") || eqi(s, "HELP")) {
        cmd_help();
        return;
    }
    if (!load_catalog(tables, &table_count)) {
        printf("ERR CANNOT READ CATALOG\n");
        return;
    }

    if (eqi(s, ".TABLES")) {
        cmd_tables(tables, table_count);
    } else if (starts_i(s, ".SCHEMA")) {
        cmd_schema(tables, table_count, s);
    } else if (starts_i(s, "CREATE TABLE")) {
        cmd_create(tables, &table_count, s);
    } else if (starts_i(s, "INSERT INTO")) {
        cmd_insert(tables, table_count, s);
    } else if (starts_i(s, "SELECT")) {
        cmd_select(tables, table_count, s);
    } else if (starts_i(s, "DELETE FROM")) {
        cmd_delete(tables, table_count, s);
    } else if (starts_i(s, "UPDATE")) {
        cmd_update(tables, table_count, s);
    } else if (starts_i(s, "DROP TABLE")) {
        cmd_drop(tables, &table_count, s);
    } else {
        printf("ERR UNKNOWN COMMAND\n");
    }
}

static int run_processor(int interactive)
{
    char line[MAX_LINE];
    char stmt[MAX_STATEMENT];
    char *p;
    int got_line;

    stmt[0] = '\0';
    if (interactive) {
        printf("MINISQL TSO READY\n");
    } else {
        printf("MINISQL MVS READY\n");
    }
    printf("END STATEMENTS WITH ;  USE .QUIT TO EXIT\n");

    if (interactive) {
        printf("SQL> ");
        fflush(stdout);
    }
    while (1) {
#if defined(__MVS__) && defined(MINISQL_TSO)
        if (interactive) {
            memset(line, 0, sizeof(line));
            got_line = msqtget(line, sizeof(line));
            normalize_terminal_line(line, sizeof(line));
        } else
#endif
        {
            got_line = fgets(line, sizeof(line), stdin) != NULL ? 1 : -1;
        }
        if (got_line < 0) {
            break;
        }
        p = trim(line);
        if (eqi(p, ".QUIT") || eqi(p, "//QUIT") || eqi(p, "QUIT")) {
            break;
        }
        if ((int)strlen(stmt) + (int)strlen(p) + 2 >= MAX_STATEMENT) {
            printf("ERR STATEMENT TOO LONG\n");
            stmt[0] = '\0';
            if (interactive) {
                printf("SQL> ");
                fflush(stdout);
            }
            continue;
        }
        if (stmt[0] != '\0') {
            strcat(stmt, " ");
        }
        strcat(stmt, p);
        if (strchr(p, ';') != NULL || p[0] == '.') {
            execute(stmt);
            stmt[0] = '\0';
            if (interactive) {
                printf("SQL> ");
                fflush(stdout);
            }
        }
    }

    kv_close();
    return 0;
}

int main(void)
{
#ifdef MINISQL_TSO
    return run_processor(1);
#else
    return run_processor(0);
#endif
}
