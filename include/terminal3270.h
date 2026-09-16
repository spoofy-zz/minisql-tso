/* 3270 wire bytes are independent of the compiler's character encoding. */
#define TERM_ENTER 0x7d
#define TERM_PF12  0x7c
#define TERM_CLEAR 0x6d
#define TERM_CAPACITY 1913

static unsigned char term_code(int value)
{
    static const unsigned char codes[64] = {
        0x40,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,
        0xc8,0xc9,0x4a,0x4b,0x4c,0x4d,0x4e,0x4f,
        0x50,0xd1,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,
        0xd8,0xd9,0x5a,0x5b,0x5c,0x5d,0x5e,0x5f,
        0x60,0x61,0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,
        0xe8,0xe9,0x6a,0x6b,0x6c,0x6d,0x6e,0x6f,
        0xf0,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,
        0xf8,0xf9,0x7a,0x7b,0x7c,0x7d,0x7e,0x7f
    };
    return codes[value & 63];
}

static int term_address(unsigned char a, unsigned char b)
{
    return (a & 0xc0) ? ((a & 63) * 64 + (b & 63))
                      : ((a & 63) * 256 + b);
}

/* Full-screen recall: protected prompt, unprotected field with MDT set.
 * MDT makes Enter return the text even if the user has changed nothing. */
static int term_recall_screen(unsigned char *out, const char *sql)
{
    int n = 0, len = (int)strlen(sql), cursor;
    if (len > TERM_CAPACITY) return -1;
    out[n++] = 0xc3; /* reset MDT, restore keyboard */
    out[n++] = 0x11; out[n++] = 0x40; out[n++] = 0x40;
    out[n++] = 0x3c; out[n++] = 0x40; out[n++] = 0x40;
    out[n++] = 0x00; /* erase entire screen to NUL */
    out[n++] = 0x11; out[n++] = term_code(1919 / 64);
    out[n++] = term_code(1919);
    out[n++] = 0x1d; out[n++] = 0x60; /* protected wrap field */
    /* Do not rely on wrapping at 1920: model 3/4/5 screens are larger. */
    out[n++] = 0x11; out[n++] = 0x40; out[n++] = 0x40;
    memcpy(out + n, "SQL> ", 5); n += 5;
    out[n++] = 0x1d; out[n++] = 0xc1; /* input begins at position 6 */
    memcpy(out + n, sql, len); n += len;
    cursor = len < TERM_CAPACITY ? 6 + len : 6;
    out[n++] = 0x11; out[n++] = term_code(cursor / 64);
    out[n++] = term_code(cursor); out[n++] = 0x13;
    return n;
}

/* Decode Read Modified (AID, cursor, SBA + field data). Never scan SQL
 * for keywords: quoted strings and subqueries can contain those too. */
static int term_input(char *out, int cap, const unsigned char *raw,
                      int len, int recall)
{
    int i, pos = -1, n = 0, at;
    if (len < 3) return -1;
    memset(out, ' ', cap - 1);
    for (i = 3; i < len; i++) {
        if (raw[i] == 0x11) {
            if (i + 2 >= len) return -1;
            pos = term_address(raw[i + 1], raw[i + 2]);
            i += 2;
            if (!recall && n && out[n - 1] != ' ') {
                if (n >= cap - 1) return -1;
                out[n++] = ' ';
            }
        } else {
            if (pos < 0) return -1;
            at = recall ? pos - 6 : n;
            if (at < 0 || at >= cap - 1 ||
                (recall && at >= TERM_CAPACITY)) return -1;
            out[at] = raw[i] ? (char)raw[i] : ' ';
            if (at >= n) n = at + 1;
            pos++;
        }
    }
    while (n > 0 && out[n - 1] == ' ') n--;
    out[n] = '\0';
    return n;
}
