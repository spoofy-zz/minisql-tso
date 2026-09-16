#include <assert.h>
#include <string.h>
#include "terminal3270.h"

/* Interpret the emitted orders using the actual terminal buffer size.
 * In particular, SF at 1919 wraps only on a model 2 terminal. */
static void check_geometry(int cells)
{
    unsigned char stream[2100], display[4000];
    const char *sql = "SELECT * FROM PEOPLE;";
    int n = term_recall_screen(stream, sql);
    int i = 1, pos = 0, end, cursor = -1, input = -1;
    memset(display, 0xff, sizeof(display));
    while (i < n) {
        unsigned char c = stream[i++];
        if (c == 0x11) {
            pos = term_address(stream[i], stream[i + 1]); i += 2;
        } else if (c == 0x3c) {
            end = term_address(stream[i], stream[i + 1]); i += 2;
            c = stream[i++];
            do {
                display[pos] = c; pos = (pos + 1) % cells;
            } while (pos != end);
        } else if (c == 0x1d) {
            c = stream[i++]; display[pos] = 0;
            pos = (pos + 1) % cells;
            if (c == 0xc1) input = pos;
        } else if (c == 0x13) {
            cursor = pos;
        } else {
            display[pos] = c; pos = (pos + 1) % cells;
        }
    }
    assert(!memcmp(display, "SQL> ", 5));
    assert(input == 6);
    assert(!memcmp(display + input, sql, strlen(sql)));
    assert(cursor == input + (int)strlen(sql));
}

int main(void)
{
    unsigned char screen[2100];
    unsigned char raw[2100] = {0x7d,0x40,0x40,0x11,0x40,0xc6};
    char out[2100], large[1915];
    const char *sql = "SELECT 'SELECT' FROM PEOPLE WHERE ID=10;";
    int n, len = (int)strlen(sql), i;
    check_geometry(24 * 80);
    check_geometry(32 * 80);
    check_geometry(43 * 80);
    check_geometry(27 * 132);
    n = term_recall_screen(screen, sql);
    assert(n > len);
    /* Emulate terminal Read Modified on Enter, including unchanged text. */
    assert(screen[21] == 0x1d && screen[22] == 0xc1);
    assert(!memcmp(screen + 23, sql, len));
    memcpy(raw + 6, sql, len);
    assert(term_input(out, sizeof(out), raw, len + 6, 1) == len);
    assert(!strcmp(out, sql));
    /* Edited field, NUL holes, short/deleted suffix and malformed SBA. */
    raw[6] = 0;
    assert(term_input(out, sizeof(out), raw, len + 6, 1) == len);
    assert(out[0] == ' ' && out[1] == 'E');
    memcpy(raw + 6, ".QUIT", 5);
    assert(term_input(out, sizeof(out), raw, 11, 1) == 5);
    assert(!strcmp(out, ".QUIT"));
    assert(term_input(out, sizeof(out), raw, 4, 1) == -1);
    assert(term_input(out, 3, raw, 11, 1) == -1);
    for (i = 0; i < 1920; i++)
        assert(term_address(term_code(i / 64), term_code(i)) == i);
    memset(large, 'A', sizeof(large));
    large[1913] = 0;
    assert(term_recall_screen(screen, large) > 0);
    large[1913] = 'A'; large[1914] = 0;
    assert(term_recall_screen(screen, large) == -1);
    return 0;
}
