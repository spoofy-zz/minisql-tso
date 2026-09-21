#define MINISQL_OUTPUT_IMPL
#include "minisql.h"
#ifdef __MVS__
extern int msqtput(char *buf, int len) asm("MSQTPUT");
#endif

/* Shared output routes to TPUT only in the interactive MVS processor. */
#ifdef __MVS__
extern int msqtclr(void) asm("MSQTCLR");
static char g_tso_out[MAX_LINE];
static int g_tso_out_len = 0;
static int g_tso_lines = 0;

static void tso_flush_line(void)
{
    if (g_tso_out_len > 0) {
        /* TPUT EDIT cannot safely scroll past the physical 3270 page on
         * every TSO terminal. Keep long SELECT results alive by starting a
         * fresh page before the next line would overflow it. */
        if (g_tso_lines >= 20) {
            msqtclr();
            g_tso_lines = 0;
        }
        g_tso_out[g_tso_out_len] = '\0';
        msqtput(g_tso_out, g_tso_out_len);
        g_tso_out_len = 0;
        g_tso_lines++;
    }
}

int msql_fflush(FILE *fp)
{
    if (!sql_interactive()) return fflush(fp);
    tso_flush_line();
    return 0;
}

int msql_printf(const char *fmt, ...)
{
    char tmp[MAX_LINE];
    va_list ap;
    int n;
    int i;

    va_start(ap, fmt);
    if (!sql_interactive()) {
        n = vfprintf(stdout, fmt, ap);
        va_end(ap);
        return n;
    }
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

#else
int msql_fflush(FILE *fp)
{
    return fflush(fp);
}

int msql_printf(const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}
#endif
