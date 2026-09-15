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
#define MAX_COLS 16
#define MAX_VALUE 32
#define MAX_DEF_TEXT 128
#define ROWSET_INITIAL 32
#define MAX_ROW_SLOTS 999999
#define MAX_TABLES 32
#define MAX_INDEXES 4
#define MAX_FKS 4
#define MAX_DEFS (MAX_COLS + MAX_FKS + 1)
#define MAX_STATEMENT 2048
#define KV_KEY 64
#define KV_DATA 960
#define KV_RECLEN (KV_KEY + KV_DATA)
#define KV_DD "MINIKV"
#define TYPE_TEXT 0
#define TYPE_INT 1
#define TYPE_CHAR 2
#define TYPE_VARCHAR 3
#define MAX_CONDS 8
#define OP_EQ 1
#define OP_LT 2
#define OP_GT 3
#define OP_LIKE 4
#define OP_BETWEEN 5
#define LOGIC_AND 1
#define LOGIC_OR 2
#define TX_NONE 0
#define TX_USER 1
#define TX_IMPLICIT 2

struct TableDef {
    char name[MAX_NAME + 1];
    int col_count;
    int pk_col;
    int index_count;
    int fk_count;
    char cols[MAX_COLS][MAX_NAME + 1];
    int col_types[MAX_COLS];
    int col_lens[MAX_COLS];
    char index_names[MAX_INDEXES][MAX_NAME + 1];
    int index_cols[MAX_INDEXES];
    int fk_cols[MAX_FKS];
    char fk_tables[MAX_FKS][MAX_NAME + 1];
    char fk_ref_cols[MAX_FKS][MAX_NAME + 1];
};

struct Row {
    char values[MAX_COLS][MAX_VALUE + 1];
};

struct RowSet {
    struct Row *rows;
    int count;
    int cap;
};

struct KeySet {
    char (*keys)[KV_KEY];
    int count;
    int cap;
};

struct WhereCond {
    int col;
    int op;
    char value1[MAX_VALUE + 1];
    char value2[MAX_VALUE + 1];
};

struct WhereExpr {
    int cond_count;
    int logic[MAX_CONDS - 1];
    struct WhereCond conds[MAX_CONDS];
};

struct GroupRow {
    char value[MAX_VALUE + 1];
    int count;
};

struct KvRec {
    char key[KV_KEY];
    char data[KV_DATA];
};

static void rtrim(char *s);
static void clean_token(char *s);
static int eqi(const char *a, const char *b);
static int parse_where(struct TableDef *t, char *where_text,
                       struct WhereExpr *expr);
static int validate_value(struct TableDef *t, int col, const char *value);
static void cmd_select_join(struct TableDef tables[], int count, char *sql);
static int kv_put_raw(const char *key, const char *data);
static int kv_delete_raw(const char *key);
static int kv_put(const char *key, const char *data);
static int kv_delete(const char *key);
static int key_cb(const char *key, const char *data, void *arg);
static struct TableDef *g_sort_table = NULL;
static int g_sort_col = -1;
static int g_sort_desc = 0;
static int g_group_sort_by_count = 0;
static int g_tx_active = 0;
static int g_tx_mode = TX_NONE;
static int g_tx_seq = 0;

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
static struct KvRec g_host_kv[32768];
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

static void kv_make_index_key(char *out, const char *table,
                              const char *idx, const char *value, int seq)
{
    char tmp[KV_KEY + 1];
    char val[MAX_VALUE + 1];

    memset(out, ' ', KV_KEY);
    strncpy(val, value, MAX_VALUE);
    val[MAX_VALUE] = '\0';
    clean_token(val);
    sprintf(tmp, "X|%-16.16s|%-16.16s|%-18.18s|%06d",
            table, idx, val, seq);
    memcpy(out, tmp, strlen(tmp));
}

static void kv_make_index_prefix(char *out, const char *table,
                                 const char *idx, const char *value,
                                 int *len)
{
    char tmp[KV_KEY + 1];
    char val[MAX_VALUE + 1];

    memset(out, ' ', KV_KEY);
    strncpy(val, value, MAX_VALUE);
    val[MAX_VALUE] = '\0';
    clean_token(val);
    sprintf(tmp, "X|%-16.16s|%-16.16s|%-18.18s|", table, idx, val);
    memcpy(out, tmp, strlen(tmp));
    *len = (int)strlen(tmp);
}

static int kv_index_slot(const char *key)
{
    char buf[KV_KEY + 1];
    char *p;

    memcpy(buf, key, KV_KEY);
    buf[KV_KEY] = '\0';
    rtrim(buf);
    p = strrchr(buf, '|');
    if (p == NULL) {
        return -1;
    }
    return atoi(p + 1);
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

static int kv_put_raw(const char *key, const char *data)
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

static int kv_delete_raw(const char *key)
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

static void tx_key(char *out, const char *kind, int seq)
{
    kv_make_key(out, kind, "TX", seq);
}

static char hex_digit(int n)
{
    return (char)(n < 10 ? '0' + n : 'A' + n - 10);
}

static void key_to_hex(const char *key, char *out)
{
    int i;

    for (i = 0; i < KV_KEY; i++) {
        unsigned char c = (unsigned char)key[i];
        out[i * 2] = hex_digit((c >> 4) & 0x0f);
        out[i * 2 + 1] = hex_digit(c & 0x0f);
    }
    out[KV_KEY * 2] = '\0';
}

static int from_hex(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static int hex_to_key(const char *hex, char *key)
{
    int i;

    if ((int)strlen(hex) < KV_KEY * 2) {
        return 0;
    }
    for (i = 0; i < KV_KEY; i++) {
        int hi = from_hex(hex[i * 2]);
        int lo = from_hex(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return 0;
        }
        key[i] = (char)((hi << 4) | lo);
    }
    return 1;
}

static int tx_clear_journal(int upto)
{
    int i;
    char key[KV_KEY];

    for (i = 1; i <= upto; i++) {
        tx_key(key, "JK", i);
        if (!kv_delete_raw(key)) {
            return 0;
        }
        tx_key(key, "JT", i);
        if (!kv_delete_raw(key)) {
            return 0;
        }
        tx_key(key, "JD", i);
        if (!kv_delete_raw(key)) {
            return 0;
        }
    }
    tx_key(key, "JN", -1);
    if (!kv_delete_raw(key)) {
        return 0;
    }
    tx_key(key, "JS", -1);
    if (!kv_delete_raw(key)) {
        return 0;
    }
    return 1;
}

static int tx_rollback_to(int upto)
{
    int i;
    char jkey[KV_KEY];

    for (i = upto; i >= 1; i--) {
        char hex[KV_KEY * 2 + 1];
        char type[MAX_VALUE + 1];
        char old[KV_DATA + 1];
        char key[KV_KEY];

        tx_key(jkey, "JK", i);
        if (!kv_get(jkey, hex, sizeof(hex))) {
            continue;
        }
        tx_key(jkey, "JT", i);
        if (!kv_get(jkey, type, sizeof(type))) {
            continue;
        }
        tx_key(jkey, "JD", i);
        if (!kv_get(jkey, old, sizeof(old))) {
            old[0] = '\0';
        }
        if (!hex_to_key(hex, key)) {
            return 0;
        }
        if (eqi(type, "P")) {
            if (!kv_put_raw(key, old)) {
                return 0;
            }
        } else if (eqi(type, "D")) {
            if (!kv_delete_raw(key)) {
                return 0;
            }
        } else {
            return 0;
        }
    }
    return tx_clear_journal(upto);
}

static int tx_recover(void)
{
    char key[KV_KEY];
    char status[MAX_VALUE + 1];
    char seqbuf[MAX_VALUE + 1];
    int seq;

    tx_key(key, "JS", -1);
    if (!kv_get(key, status, sizeof(status)) || !eqi(status, "ACTIVE")) {
        return 1;
    }
    tx_key(key, "JN", -1);
    seq = kv_get(key, seqbuf, sizeof(seqbuf)) ? atoi(seqbuf) : 0;
    return tx_rollback_to(seq);
}

static int tx_begin(int mode)
{
    char key[KV_KEY];

    if (g_tx_active) {
        return 0;
    }
    g_tx_active = 1;
    g_tx_mode = mode;
    g_tx_seq = 0;
    tx_key(key, "JS", -1);
    if (!kv_put_raw(key, "ACTIVE")) {
        g_tx_active = 0;
        g_tx_mode = TX_NONE;
        return 0;
    }
    tx_key(key, "JN", -1);
    if (!kv_put_raw(key, "0")) {
        g_tx_active = 0;
        g_tx_mode = TX_NONE;
        return 0;
    }
    return 1;
}

static int tx_commit(void)
{
    int seq = g_tx_seq;

    if (!g_tx_active) {
        return 0;
    }
    if (!tx_clear_journal(seq)) {
        return 0;
    }
    g_tx_active = 0;
    g_tx_mode = TX_NONE;
    g_tx_seq = 0;
    return 1;
}

static int tx_rollback(void)
{
    int seq = g_tx_seq;

    if (!g_tx_active) {
        return 0;
    }
    if (!tx_rollback_to(seq)) {
        return 0;
    }
    g_tx_active = 0;
    g_tx_mode = TX_NONE;
    g_tx_seq = 0;
    return 1;
}

static int tx_log_before(const char *key)
{
    char old[KV_DATA + 1];
    char hex[KV_KEY * 2 + 1];
    char jkey[KV_KEY];
    char seqbuf[MAX_VALUE + 1];
    int existed;

    if (!g_tx_active) {
        return 1;
    }
    existed = kv_get(key, old, sizeof(old));
    key_to_hex(key, hex);
    g_tx_seq++;
    tx_key(jkey, "JK", g_tx_seq);
    if (!kv_put_raw(jkey, hex)) {
        return 0;
    }
    tx_key(jkey, "JT", g_tx_seq);
    if (!kv_put_raw(jkey, existed ? "P" : "D")) {
        return 0;
    }
    tx_key(jkey, "JD", g_tx_seq);
    if (!kv_put_raw(jkey, existed ? old : "")) {
        return 0;
    }
    tx_key(jkey, "JN", -1);
    sprintf(seqbuf, "%d", g_tx_seq);
    return kv_put_raw(jkey, seqbuf);
}

static int kv_put(const char *key, const char *data)
{
    if (!tx_log_before(key)) {
        return 0;
    }
    return kv_put_raw(key, data);
}

static int kv_delete(const char *key)
{
    if (!tx_log_before(key)) {
        return 0;
    }
    return kv_delete_raw(key);
}

static int kv_scan(const char *prefix, int prefix_len,
                   int (*cb)(const char *key, const char *data, void *arg),
                   void *arg)
{
#ifdef __MVS__
    struct KvRec rec;
    char last_key[KV_KEY];
    int rc;
    int have_last = 0;
    if (!kv_open()) {
        return 0;
    }
    memset(&rec, ' ', sizeof(rec));
    if (__vsstge(g_kv, &rec, sizeof(rec), (void *)prefix, prefix_len) != 0) {
        __vsclr(g_kv);
        return 1;
    }
    if (key_has_prefix(rec.key, prefix, prefix_len)) {
        char data[KV_DATA + 1];
        data_get(data, rec.data, sizeof(data));
        if (!cb(rec.key, data, arg)) {
            return 1;
        }
        memcpy(last_key, rec.key, KV_KEY);
        have_last = 1;
    }
    while ((rc = __vsread(g_kv, &rec, sizeof(rec), NULL, 0)) >= 0) {
        char data[KV_DATA + 1];
        if (!key_has_prefix(rec.key, prefix, prefix_len)) {
            break;
        }
        if (have_last && memcmp(last_key, rec.key, KV_KEY) == 0) {
            continue;
        }
        data_get(data, rec.data, sizeof(data));
        if (!cb(rec.key, data, arg)) {
            break;
        }
        memcpy(last_key, rec.key, KV_KEY);
        have_last = 1;
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

static int line_is_empty_input(const char *s)
{
    while (*s != '\0') {
        unsigned char c = (unsigned char)*s;
        if (isalnum(c) || strchr(".,/*<>=_'\"()-", c) != NULL) {
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
           starts_i(s, "DESC") ||
           starts_i(s, "DESCRIBE") ||
           starts_i(s, "CREATE TABLE") ||
           starts_i(s, "CREATE INDEX") ||
           starts_i(s, "INSERT INTO") ||
           starts_i(s, "SELECT") ||
           starts_i(s, "UPDATE") ||
           starts_i(s, "DELETE FROM") ||
           starts_i(s, "DROP TABLE") ||
           starts_i(s, "BEGIN") ||
           starts_i(s, "COMMIT") ||
           starts_i(s, "ROLLBACK");
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

static void clean_token_max(char *s, int max)
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

static void clean_token(char *s)
{
    clean_token_max(s, MAX_VALUE + 1);
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

static int parse_column_def(char *text, char *name, int *type, int *len)
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

static void type_to_text(int type, int len, char *out, int max)
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

static int validate_value(struct TableDef *t, int col, const char *value)
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

static int cmp_value(struct TableDef *t, int col,
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

static int where_match(struct TableDef *t, struct Row *row,
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

static int where_simple_eq(struct WhereExpr *expr, int *col, char *value)
{
    if (expr->cond_count != 1 || expr->conds[0].op != OP_EQ) {
        return 0;
    }
    *col = expr->conds[0].col;
    strncpy(value, expr->conds[0].value1, MAX_VALUE);
    value[MAX_VALUE] = '\0';
    return 1;
}

static int parse_csv_limit(char *text, char values[][MAX_VALUE + 1],
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

static int parse_csv(char *text, char values[MAX_COLS][MAX_VALUE + 1],
                     int *count)
{
    return parse_csv_limit(text, values, count, MAX_COLS);
}

static int parse_def_csv(char *text, char values[MAX_DEFS][MAX_DEF_TEXT + 1],
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

static int load_catalog(struct TableDef tables[], int *count)
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

static int save_catalog(struct TableDef tables[], int count)
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

static int find_pk_duplicate(struct TableDef *t, struct Row rows[],
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

static int find_index_name(struct TableDef *t, const char *name)
{
    int i;
    for (i = 0; i < t->index_count; i++) {
        if (eqi(t->index_names[i], name)) {
            return i;
        }
    }
    return -1;
}

static int find_index_col(struct TableDef *t, int col)
{
    int i;
    for (i = 0; i < t->index_count; i++) {
        if (t->index_cols[i] == col) {
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
    struct RowSet *set;
    int ok;
};

static void rowset_init(struct RowSet *set)
{
    set->rows = NULL;
    set->count = 0;
    set->cap = 0;
}

static void rowset_free(struct RowSet *set)
{
    if (set->rows != NULL) {
        free(set->rows);
    }
    set->rows = NULL;
    set->count = 0;
    set->cap = 0;
}

static int rowset_reserve(struct RowSet *set, int need)
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

static int rowset_add(struct RowSet *set, struct Row *row)
{
    if (!rowset_reserve(set, set->count + 1)) {
        return 0;
    }
    set->rows[set->count++] = *row;
    return 1;
}

static void keyset_init(struct KeySet *set)
{
    set->keys = NULL;
    set->count = 0;
    set->cap = 0;
}

static void keyset_free(struct KeySet *set)
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

static int load_rows(struct TableDef *t, struct RowSet *set)
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

static int load_row_slot(struct TableDef *t, int slot, struct Row *row,
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

static int delete_index_rows(struct TableDef *t)
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

static int key_cb(const char *key, const char *data, void *arg)
{
    struct KeySet *keys = (struct KeySet *)arg;

    (void)data;
    if (!keyset_add(keys, key)) {
        return 0;
    }
    return 1;
}

static int rebuild_indexes(struct TableDef *t, struct Row rows[],
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

static int save_rows(struct TableDef *t, struct Row rows[], int row_count)
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

static int check_foreign_keys(struct TableDef tables[], int count,
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

static int row_is_referenced(struct TableDef tables[], int count,
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

static void print_select_line(struct TableDef *t, struct Row *row, int header)
{
    char line[MAX_LINE];
    int c;

    line[0] = '\0';
    for (c = 0; c < t->col_count; c++) {
        if (c > 0) {
            strcat(line, " | ");
        }
        if (header) {
            strcat(line, t->cols[c]);
        } else {
            strcat(line, row->values[c]);
        }
    }
    printf("%s\n", line);
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
                                 int cols[], int col_count, int header)
{
    char line[MAX_LINE];
    int i;

    line[0] = '\0';
    for (i = 0; i < col_count; i++) {
        if (i > 0) {
            strcat(line, " | ");
        }
        if (header) {
            strcat(line, t->cols[cols[i]]);
        } else {
            strcat(line, row->values[cols[i]]);
        }
    }
    printf("%s\n", line);
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

static char *next_select_clause(char *where, char *group, char *order)
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

static void print_join_header(struct TableDef *left, struct TableDef *right)
{
    int c;
    int first = 1;

    for (c = 0; c < left->col_count; c++) {
        if (!first) {
            printf(" | ");
        }
        printf("%s.%s", left->name, left->cols[c]);
        first = 0;
    }
    for (c = 0; c < right->col_count; c++) {
        if (!first) {
            printf(" | ");
        }
        printf("%s.%s", right->name, right->cols[c]);
        first = 0;
    }
    printf("\n");
}

static void print_join_row(struct TableDef *left, struct Row *lrow,
                           struct TableDef *right, struct Row *rrow)
{
    int c;
    int first = 1;

    for (c = 0; c < left->col_count; c++) {
        if (!first) {
            printf(" | ");
        }
        printf("%s", lrow->values[c]);
        first = 0;
    }
    for (c = 0; c < right->col_count; c++) {
        if (!first) {
            printf(" | ");
        }
        printf("%s", rrow->values[c]);
        first = 0;
    }
    printf("\n");
}

static void cmd_select_join(struct TableDef tables[], int count, char *sql)
{
    char left_name[MAX_NAME + 1];
    char right_name[MAX_NAME + 1];
    char qleft_table[MAX_NAME + 1];
    char qleft_col[MAX_NAME + 1];
    char qright_table[MAX_NAME + 1];
    char qright_col[MAX_NAME + 1];
    char onbuf[MAX_STATEMENT];
    char *p;
    char *joinp;
    char *onp;
    char *eq;
    int i;
    int left_idx;
    int right_idx;
    int left_col;
    int right_col;
    int right_index;
    int matched = 0;
    struct RowSet left_rows;
    struct RowSet right_rows;

    p = ltrim(sql + strlen("SELECT"));
    if (*p != '*') {
        printf("ERR JOIN SUPPORTS SELECT *\n");
        return;
    }
    p++;
    p = ltrim(p);
    if (!starts_i(p, "FROM")) {
        printf("ERR BAD JOIN\n");
        return;
    }
    p = ltrim(p + strlen("FROM"));
    i = 0;
    while (*p && (isalnum((unsigned char)*p) || *p == '_')) {
        if (i < MAX_NAME) {
            left_name[i++] = (char)toupper((unsigned char)*p);
        }
        p++;
    }
    left_name[i] = '\0';
    joinp = find_keyword(p, "JOIN");
    if (!valid_name(left_name) || joinp == NULL) {
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
    if (!valid_name(right_name) || onp == NULL) {
        printf("ERR BAD JOIN\n");
        return;
    }
    strncpy(onbuf, onp + strlen("ON"), MAX_STATEMENT - 1);
    onbuf[MAX_STATEMENT - 1] = '\0';
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
    if (!eqi(qleft_table, left_name) || !eqi(qright_table, right_name)) {
        printf("ERR JOIN ORDER\n");
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
    print_join_header(&tables[left_idx], &tables[right_idx]);
    if (right_index >= 0) {
        int l;

        for (l = 0; l < left_rows.count; l++) {
            struct KeySet keys;
            char prefix[KV_KEY];
            int prefix_len;
            int k;

            keyset_init(&keys);
            kv_make_index_prefix(prefix, tables[right_idx].name,
                                 tables[right_idx].index_names[right_index],
                                 left_rows.rows[l].values[left_col],
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
                    print_join_row(&tables[left_idx], &left_rows.rows[l],
                                   &tables[right_idx], &rrow);
                    matched++;
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
                    print_join_row(&tables[left_idx], &left_rows.rows[l],
                                   &tables[right_idx], &right_rows.rows[r]);
                    matched++;
                }
            }
        }
        rowset_free(&right_rows);
    }
    rowset_free(&left_rows);
    printf("OK %d ROWS\n", matched);
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

static void cmd_desc(struct TableDef tables[], int count, char *sql)
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

static void cmd_create(struct TableDef tables[], int *count, char *sql)
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

static void cmd_create_index(struct TableDef tables[], int count, char *sql)
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

static void cmd_insert(struct TableDef tables[], int count, char *sql)
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

static void cmd_select(struct TableDef tables[], int count, char *sql)
{
    char name[MAX_NAME + 1];
    char *p;
    char *where;
    char *group;
    char *order;
    char *next;
    int idx;
    int r;
    int c;
    int group_count = 0;
    int group_col = -1;
    int order_col = -1;
    int order_count = 0;
    int order_desc = 0;
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
    next = next_select_clause(where, group, order);
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
        (group != NULL && order != NULL && group > order)) {
        printf("ERR BAD SELECT\n");
        return;
    }
    where_buf[0] = '\0';
    group_buf[0] = '\0';
    order_buf[0] = '\0';
    if (where != NULL) {
        int len;
        next = next_select_clause(NULL, group != NULL && group > where ?
                                  group : NULL,
                                  order != NULL && order > where ?
                                  order : NULL);
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
        next = order != NULL && order > group ? order : NULL;
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
        strncpy(order_buf, order, MAX_STATEMENT - 1);
        order_buf[MAX_STATEMENT - 1] = '\0';
        rtrim(order_buf);
        order = order_buf;
        if (!parse_order_clause(&tables[idx], order, group_col, &order_col,
                                &order_count, &order_desc)) {
            printf("ERR BAD ORDER BY\n");
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
        kv_make_index_prefix(prefix, tables[idx].name,
                             tables[idx].index_names[index_no],
                             where_val, &prefix_len);
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
        if (select_all || (select_col_count > 0 && select_count)) {
            printf("%s | COUNT\n", tables[idx].cols[group_col]);
        } else if (select_count) {
            printf("COUNT\n");
        } else {
            printf("%s\n", tables[idx].cols[group_col]);
        }
        for (r = 0; r < group_count; r++) {
            if (select_all || (select_col_count > 0 && select_count)) {
                printf("%s | %d\n", groups[r].value, groups[r].count);
            } else if (select_count) {
                printf("%d\n", groups[r].count);
            } else {
                printf("%s\n", groups[r].value);
            }
        }
        printf("OK %d GROUPS\n", group_count);
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
        printf("COUNT\n");
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
        print_select_line(&tables[idx], NULL, 1);
    } else {
        print_projected_line(&tables[idx], NULL, select_cols,
                             select_col_count, 1);
    }
    for (r = 0; r < out.count; r++) {
        if (select_all) {
            print_select_line(&tables[idx], &out.rows[r], 0);
        } else {
            print_projected_line(&tables[idx], &out.rows[r], select_cols,
                                 select_col_count, 0);
        }
    }
    printf("OK %d ROWS\n", out.count);
    rowset_free(&out);
}

static int where_tokenize(char *text,
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
        } else if (*p == '=' || *p == '<' || *p == '>') {
            toks[n][i++] = *p++;
        } else {
            while (*p != '\0' && !isspace((unsigned char)*p) &&
                   *p != '=' && *p != '<' && *p != '>') {
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

static int parse_where(struct TableDef *t, char *where_text,
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

static void cmd_delete(struct TableDef tables[], int count, char *sql)
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

static void cmd_update(struct TableDef tables[], int count, char *sql)
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

static void cmd_drop(struct TableDef tables[], int *count, char *sql)
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

static void cmd_help(void)
{
    printf("COMMANDS:\n");
    printf("  CREATE TABLE name (col1, col2, ...);\n");
    printf("  TYPES: INT, INTEGER, CHAR(n), VARCHAR(n), TEXT\n");
    printf("  CREATE TABLE name (id INT PRIMARY KEY, name VARCHAR(16));\n");
    printf("  CREATE TABLE name (id PRIMARY KEY, col2, ...);\n");
    printf("  CREATE TABLE name (id, col2, PRIMARY KEY (id));\n");
    printf("  FOREIGN KEY (col) REFERENCES parent(pkcol)\n");
    printf("  CREATE INDEX idx ON name (col);\n");
    printf("  INSERT INTO name VALUES (v1, v2, ...);\n");
    printf("  SELECT *|cols|COUNT(*) FROM name [WHERE expression]\n");
    printf("    [GROUP BY col] [ORDER BY col|COUNT [ASC|DESC]];\n");
    printf("  SELECT * FROM a JOIN b ON a.col=b.col;\n");
    printf("  WHERE: =, <, >, LIKE, BETWEEN, AND, OR\n");
    printf("  GROUP BY supports one column and returns column | COUNT\n");
    printf("  UPDATE name SET col=value WHERE expression;\n");
    printf("  DELETE FROM name WHERE expression;\n");
    printf("  DROP TABLE name;\n");
    printf("  BEGIN; COMMIT; ROLLBACK;\n");
    printf("  .TABLES\n");
    printf("  .SCHEMA name\n");
    printf("  DESC name or DESCRIBE name\n");
    printf("  .HELP or //HELP\n");
    printf("  .QUIT\n");
}

static void execute(char *sql)
{
    struct TableDef tables[MAX_TABLES];
    int table_count = 0;
    char *s = trim(sql);
    int len = (int)strlen(s);
    int implicit_tx = 0;

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
    if (eqi(s, "BEGIN") || eqi(s, "BEGIN TRANSACTION")) {
        if (!tx_begin(TX_USER)) {
            printf("ERR TRANSACTION ACTIVE\n");
            return;
        }
        printf("OK TRANSACTION BEGIN\n");
        return;
    }
    if (eqi(s, "COMMIT")) {
        if (!tx_commit()) {
            printf("ERR NO TRANSACTION\n");
            return;
        }
        printf("OK TRANSACTION COMMIT\n");
        return;
    }
    if (eqi(s, "ROLLBACK")) {
        if (!tx_rollback()) {
            printf("ERR NO TRANSACTION\n");
            return;
        }
        printf("OK TRANSACTION ROLLBACK\n");
        return;
    }
    if (!load_catalog(tables, &table_count)) {
        printf("ERR CANNOT READ CATALOG\n");
        return;
    }

    if (!g_tx_active &&
        (starts_i(s, "CREATE TABLE") || starts_i(s, "CREATE INDEX") ||
         starts_i(s, "INSERT INTO") || starts_i(s, "DELETE FROM") ||
         starts_i(s, "UPDATE") || starts_i(s, "DROP TABLE"))) {
        if (!tx_begin(TX_IMPLICIT)) {
            printf("ERR CANNOT BEGIN TRANSACTION\n");
            return;
        }
        implicit_tx = 1;
    }

    if (eqi(s, ".TABLES")) {
        cmd_tables(tables, table_count);
    } else if (starts_i(s, ".SCHEMA")) {
        cmd_schema(tables, table_count, s);
    } else if (starts_i(s, "DESC") || starts_i(s, "DESCRIBE")) {
        cmd_desc(tables, table_count, s);
    } else if (starts_i(s, "CREATE TABLE")) {
        cmd_create(tables, &table_count, s);
    } else if (starts_i(s, "CREATE INDEX")) {
        cmd_create_index(tables, table_count, s);
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
    if (implicit_tx) {
        if (!tx_commit()) {
            printf("ERR CANNOT COMMIT TRANSACTION\n");
        }
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
    if (!tx_recover()) {
        printf("ERR CANNOT RECOVER TRANSACTION\n");
        return 8;
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
        if (line_is_empty_input(p)) {
            if (interactive) {
                printf("SQL> ");
                fflush(stdout);
            }
            continue;
        }
        if (interactive && stmt[0] == '\0' &&
            !line_starts_command(p) && strchr(p, ';') == NULL) {
            printf("SQL> ");
            fflush(stdout);
            continue;
        }
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
