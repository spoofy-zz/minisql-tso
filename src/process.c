#include "minisql.h"
#ifdef __MVS__
#include "terminal3270.h"
extern int msqtget(char *buf, int max) asm("MSQTGET");
extern int msqtput(char *buf, int len) asm("MSQTPUT");
extern int msqtclr(void) asm("MSQTCLR");
extern int msqtline(int line) asm("MSQTLINE");
extern int msqtscr(char *buf, int len) asm("MSQTSCR");
#endif

static void cmd_help(void);
static void write_prompt(void);
static void cmd_clear(void);
static void execute(char *sql);

static int g_interactive = 0;
static int g_prompt_written = 0;
static char g_last_statement[MAX_STATEMENT];

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
    printf("  DROP INDEX idx;\n");
    printf("  INSERT INTO name VALUES (v1, v2, ...);\n");
    printf("  SELECT *|cols|COUNT(*) FROM name [WHERE expression]\n");
    printf("    [GROUP BY col] [ORDER BY col|COUNT [ASC|DESC]] [LIMIT n];\n");
    printf("  SELECT * FROM a JOIN b ON a.col=b.col;\n");
    printf("  EXPLAIN SELECT ...;\n");
    printf("  WHERE: =, <, >, <=, >=, <>, !=, LIKE, BETWEEN, AND, OR\n");
    printf("  GROUP BY supports one column and returns column | COUNT\n");
    printf("  UPDATE name SET col=value WHERE expression;\n");
    printf("  DELETE FROM name WHERE expression;\n");
    printf("  DROP TABLE name;\n");
    printf("  BEGIN; COMMIT; ROLLBACK;\n");
    printf("  .CLEAR or //CLEAR\n");
    printf("  PF12, !! or .REPEAT  recall SQL for editing (TSO)\n");
    printf("  .TABLES\n");
    printf("  .SCHEMA name\n");
    printf("  DESC name or DESCRIBE name\n");
    printf("  .HELP or //HELP\n");
    printf("  VERSION or .VERSION\n");
    printf("  .QUIT\n");
}

static void cmd_version(void)
{
    printf("%s %s (%s)\n", MBT_PROJECT, MBT_VERSION, MBT_COMMIT);
}

static void write_prompt(void)
{
    if (!g_interactive) {
        return;
    }
    printf("SQL> ");
    fflush(stdout);
    g_prompt_written = 1;
}

static void cmd_clear(void)
{
#if defined(__MVS__)
    if (g_interactive) {
        fflush(stdout);
        if (msqtclr() != 0) {
            printf("ERR CANNOT RESET TERMINAL SCREEN\n");
        }
    } else
#endif
    {
        printf("\033[2J\033[H");
    }
    write_prompt();
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
    if (eqi(s, "VERSION") || eqi(s, ".VERSION") ||
        eqi(s, "//VERSION")) {
        cmd_version();
        return;
    }
    if (eqi(s, ".CLEAR") || eqi(s, "//CLEAR") || eqi(s, "CLEAR")) {
        cmd_clear();
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

    if (!tx_active() &&
        (starts_i(s, "CREATE TABLE") || starts_i(s, "CREATE INDEX") ||
         starts_i(s, "DROP INDEX") ||
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
    } else if (starts_i(s, "DROP INDEX")) {
        cmd_drop_index(tables, table_count, s);
    } else if (starts_i(s, "INSERT INTO")) {
        cmd_insert(tables, table_count, s);
    } else if (starts_i(s, "SELECT")) {
        cmd_select(tables, table_count, s);
    } else if (starts_i(s, "EXPLAIN")) {
        cmd_explain(tables, table_count, s);
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

int run_processor(int interactive)
{
    char line[MAX_STATEMENT + 32];
    char stmt[MAX_STATEMENT];
    char *p;
    int got_line;
#if defined(__MVS__)
    int recall_active = 0;
#endif

    g_interactive = interactive;
    g_prompt_written = 0;
    g_last_statement[0] = '\0';
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
        write_prompt();
    }
    while (1) {
#if defined(__MVS__)
        if (interactive) {
            unsigned char raw[MAX_STATEMENT + 32];
            memset(line, 0, sizeof(line));
            got_line = msqtget((char *)raw, sizeof(raw));
            if (got_line > 0 && raw[0] == TERM_PF12) {
                strcpy(line, ".REPEAT");
            } else if (got_line > 0 && raw[0] == TERM_CLEAR) {
                strcpy(line, ".CLEAR");
                recall_active = 0;
            } else if (got_line >= 3 && raw[0] == TERM_ENTER) {
                got_line = term_input(line, sizeof(line), raw,
                                      got_line, recall_active);
                recall_active = 0;
                if (got_line < 0) {
                    /* A malformed screen response is not end of input. */
                    msqtclr();
                    printf("ERR INVALID TERMINAL INPUT; USE PF12 TO RECALL\n");
                    write_prompt();
                    continue;
                }
            } else if (got_line >= 0 && !recall_active) {
                memcpy(line, raw, got_line);
                line[got_line] = '\0';
            } else if (got_line >= 0) {
                /* Other PF keys must never execute the recalled SQL. */
                unsigned char screen[MAX_STATEMENT + 64];
                int size = term_recall_screen(screen, g_last_statement);
                if (size >= 0) msqtscr((char *)screen, size);
                continue;
            }
        } else
#endif
        {
            got_line = fgets(line, sizeof(line), stdin) != NULL ? 1 : -1;
        }
        if (got_line < 0) {
            break;
        } else {
            p = trim(line);
        }
        if (line_is_empty_input(p)) {
            if (interactive) {
                write_prompt();
            }
            continue;
        }
        if (interactive && stmt[0] == '\0' &&
            !line_starts_command(p) && strchr(p, ';') == NULL) {
            write_prompt();
            continue;
        }
        if (eqi(p, ".QUIT") || eqi(p, "//QUIT") || eqi(p, "QUIT")) {
            break;
        }
        if (eqi(p, "CLEAR") || eqi(p, ".CLEAR") || eqi(p, "//CLEAR") ||
            eqi(p, "CLEAR;") || eqi(p, ".CLEAR;") || eqi(p, "//CLEAR;")) {
            stmt[0] = '\0';
            cmd_clear();
            continue;
        }
        /* Recall displays an input field; only returned input is executed. */
        if (eqi(p, "!!") || eqi(p, "PF12") || eqi(p, "REPEAT") ||
            eqi(p, ".REPEAT") || eqi(p, "REPEAT;") || eqi(p, ".REPEAT;")) {
            if (g_last_statement[0] == '\0') {
                printf("ERR NO LAST COMMAND\n");
                if (interactive) {
                    write_prompt();
                }
                continue;
            }
#if defined(__MVS__)
            if (interactive) {
                unsigned char screen[MAX_STATEMENT + 64];
                int size = term_recall_screen(screen, g_last_statement);
                if (size < 0) {
                    printf("ERR COMMAND TOO LONG FOR RECALL SCREEN\n");
                    write_prompt();
                } else {
                    fflush(stdout);
                    if (msqtscr((char *)screen, size) == 0) {
                        stmt[0] = '\0';
                        recall_active = 1;
                    } else {
                        printf("ERR CANNOT OPEN RECALL SCREEN\n");
                        write_prompt();
                    }
                }
                continue;
            }
#endif
            printf("SQL> %s\n", g_last_statement);
            fflush(stdout);
            continue;
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
            g_prompt_written = 0;
#if defined(__MVS__)
            if (interactive) {
                /* TSO line mode does not reliably track echoed input rows.
                 * Always clear before executing an interactive statement so
                 * its result cannot overlap the command text. */
                if (msqtclr() != 0) {
                    msqtline(1);
                }
            }
#endif
            if (p[0] != '.') {
                strncpy(g_last_statement, stmt, MAX_STATEMENT - 1);
                g_last_statement[MAX_STATEMENT - 1] = '\0';
            }
            execute(stmt);
            stmt[0] = '\0';
            if (interactive && !g_prompt_written) {
                write_prompt();
            }
        }
    }

    kv_close();
    return 0;
}

int sql_interactive(void)
{
    return g_interactive;
}
