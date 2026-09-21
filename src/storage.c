#include "minisql.h"

struct KvRec {
    char key[KV_KEY];
    char data[KV_DATA];
};

#ifdef __MVS__
#include <clibvsam.h>
#endif

static int key_has_prefix(const char *key, const char *prefix, int len);
static void data_put(char *dst, const char *src);
static void data_get(char *dst, const char *src, int max);
#ifdef __MVS__
static int kv_open(void);
#endif
static int kv_put_raw(const char *key, const char *data);
static int kv_delete_raw(const char *key);
static void tx_key(char *out, const char *kind, int seq);
static void key_to_hex(const char *key, char *out);
static int from_hex(char c);
static int hex_to_key(const char *hex, char *key);
static int tx_clear_journal(int upto);
static int tx_rollback_to(int upto);
static int tx_log_before(const char *key);

static int g_tx_active = 0;
static int g_tx_mode = TX_NONE;
static int g_tx_seq = 0;

#ifdef __MVS__
static VSFILE *g_kv = NULL;
#else
static struct KvRec g_host_kv[32768];
static int g_host_kv_count = 0;
#endif

void kv_make_key(char *out, const char *kind, const char *name, int seq)
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

void kv_make_index_key(char *out, const char *table,
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

void kv_make_index_base_prefix(char *out, const char *table,
                                      const char *idx, int *len)
{
    char tmp[KV_KEY + 1];

    memset(out, ' ', KV_KEY);
    sprintf(tmp, "X|%-16.16s|%-16.16s|", table, idx);
    memcpy(out, tmp, strlen(tmp));
    *len = (int)strlen(tmp);
}

int kv_index_slot(const char *key)
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

void kv_make_prefix(char *out, const char *kind,
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

#ifdef __MVS__
static int kv_open(void)
{
    if (g_kv != NULL) {
        return 1;
    }
    return __vsopen(KV_DD, VSTYPE_KSDS, VSACCESS_DYNAM, VSMODE_UPD, &g_kv) == 0;
}
#endif

int kv_close(void)
{
#ifdef __MVS__
    if (g_kv != NULL) {
        __vsclos(g_kv);
        g_kv = NULL;
    }
#endif
    return 1;
}

int kv_get(const char *key, char *data, int max)
{
#ifdef __MVS__
    struct KvRec rec;
    int rc;
    if (!kv_open()) {
        return 0;
    }
    memset(&rec, ' ', sizeof(rec));
    __vsclr(g_kv);
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
        if (__vsread(g_kv, &rec, sizeof(rec), (void *)key, KV_KEY) < 0) {
            __vsclr(g_kv);
            return 0;
        }
        /* Position for update before replacing the old record's payload. */
        data_put(rec.data, data);
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
    __vsclr(g_kv);
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

int tx_recover(void)
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

int tx_begin(int mode)
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

int tx_commit(void)
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

int tx_rollback(void)
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

int kv_put(const char *key, const char *data)
{
    if (!tx_log_before(key)) {
        return 0;
    }
    return kv_put_raw(key, data);
}

int kv_delete(const char *key)
{
    if (!tx_log_before(key)) {
        return 0;
    }
    return kv_delete_raw(key);
}

int kv_scan(const char *prefix, int prefix_len,
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
    /* EOF/error flags are sticky in clibvsam, including across POINT calls. */
    __vsclr(g_kv);
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
    __vsclr(g_kv);
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


int tx_active(void)
{
    return g_tx_active;
}
