#ifndef MINISQL_INTERNAL_H
#define MINISQL_INTERNAL_H

/* Internal engine interface. Module-private helpers and state stay in .c files. */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdarg.h>

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
#define MAX_JOIN_COLS (MAX_COLS * 2)
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
#define OP_LE 6
#define OP_GE 7
#define OP_NE 8
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


/* MVS external names are limited to eight characters. Disambiguate names
 * that cc370 would otherwise truncate to the same linker symbol. */
#ifdef __MVS__
#define SQL_LINK(name) asm(name)
#else
#define SQL_LINK(name)
#endif

/* storage */
void kv_make_key(char *out, const char *kind, const char *name, int seq) SQL_LINK("MQKEY");
void kv_make_index_key(char *out, const char *table,
                              const char *idx, const char *value, int seq) SQL_LINK("MQIXKEY");
void kv_make_index_base_prefix(char *out, const char *table,
                                      const char *idx, int *len) SQL_LINK("MQIXPFX");
int kv_index_slot(const char *key);
void kv_make_prefix(char *out, const char *kind,
                           const char *name, int *len) SQL_LINK("MQPREFIX");
int kv_close(void);
int kv_get(const char *key, char *data, int max);
int tx_recover(void);
int tx_begin(int mode);
int tx_commit(void);
int tx_rollback(void);
int kv_put(const char *key, const char *data);
int kv_delete(const char *key);
int kv_scan(const char *prefix, int prefix_len,
                   int (*cb)(const char *key, const char *data, void *arg),
                   void *arg);
int tx_active(void);

/* parser */
char *ltrim(char *s);
void rtrim(char *s);
char *trim(char *s);
int line_is_empty_input(const char *s);
int eqi(const char *a, const char *b);
int starts_i(const char *s, const char *prefix);
int line_starts_command(const char *s);
int ncmp_i(const char *a, const char *b, int n);
char *find_i(char *s, const char *needle);
void clean_token_max(char *s, int max) SQL_LINK("MQCLMAX");
void clean_token(char *s) SQL_LINK("MQCLEAN");
int valid_name(const char *s);
int parse_column_def(char *text, char *name, int *type, int *len);
void type_to_text(int type, int len, char *out, int max);
int validate_value(struct TableDef *t, int col, const char *value);
int cmp_value(struct TableDef *t, int col,
                     const char *left, const char *right);
int where_match(struct TableDef *t, struct Row *row,
                       struct WhereExpr *expr);
int where_simple_eq(struct WhereExpr *expr, int *col, char *value);
int parse_csv_limit(char *text, char values[][MAX_VALUE + 1],
                           int *count, int limit) SQL_LINK("MQCSVLIM");
int parse_csv(char *text, char values[MAX_COLS][MAX_VALUE + 1],
                     int *count) SQL_LINK("MQCSV");
int parse_def_csv(char *text, char values[MAX_DEFS][MAX_DEF_TEXT + 1],
                         int *count);
int parse_name_after(char *sql, const char *prefix, char *name);
int where_tokenize(char *text,
                          char toks[][MAX_VALUE + 1], int *count);
int parse_where(struct TableDef *t, char *where_text,
                       struct WhereExpr *expr);

/* catalog */
int load_catalog(struct TableDef tables[], int *count);
int save_catalog(struct TableDef tables[], int count);
int find_pk_duplicate(struct TableDef *t, struct Row rows[],
                             int row_count, const char *value,
                             int skip_row);
int find_table(struct TableDef tables[], int count, const char *name);
int find_col(struct TableDef *t, const char *name);
int find_index_name(struct TableDef *t, const char *name) SQL_LINK("MQIXNAME");
int find_index_col(struct TableDef *t, int col) SQL_LINK("MQIXCOL");

/* rows */
void rowset_init(struct RowSet *set);
void rowset_free(struct RowSet *set);
int rowset_reserve(struct RowSet *set, int need);
int rowset_add(struct RowSet *set, struct Row *row);
void keyset_init(struct KeySet *set);
void keyset_free(struct KeySet *set);
int load_rows(struct TableDef *t, struct RowSet *set) SQL_LINK("MQROWS");
int load_row_slot(struct TableDef *t, int slot, struct Row *row,
                         int *found) SQL_LINK("MQSLOT");
int delete_index_rows(struct TableDef *t);
int key_cb(const char *key, const char *data, void *arg);
int rebuild_indexes(struct TableDef *t, struct Row rows[],
                           int row_count);
int save_rows(struct TableDef *t, struct Row rows[], int row_count);
int check_foreign_keys(struct TableDef tables[], int count,
                              struct TableDef *table,
                              char values[][MAX_VALUE + 1]);
int row_is_referenced(struct TableDef tables[], int count,
                             struct TableDef *parent, const char *value);

/* select */
void cmd_select(struct TableDef tables[], int count, char *sql);
void cmd_explain(struct TableDef tables[], int count, char *sql);

/* schema */
void cmd_tables(struct TableDef tables[], int count);
void cmd_schema(struct TableDef tables[], int count, char *sql);
void cmd_desc(struct TableDef tables[], int count, char *sql);
void cmd_create(struct TableDef tables[], int *count, char *sql) SQL_LINK("MQCREATE");
void cmd_create_index(struct TableDef tables[], int count, char *sql) SQL_LINK("MQCRIDX");
void cmd_drop_index(struct TableDef tables[], int count, char *sql) SQL_LINK("MQDRIDX");
void cmd_drop(struct TableDef tables[], int *count, char *sql) SQL_LINK("MQDROP");

/* mutate */
void cmd_insert(struct TableDef tables[], int count, char *sql);
void cmd_delete(struct TableDef tables[], int count, char *sql);
void cmd_update(struct TableDef tables[], int count, char *sql);

/* process */
int run_processor(int interactive);
int sql_interactive(void);

/* output */
int msql_fflush(FILE *fp);
int msql_printf(const char *fmt, ...);

#undef SQL_LINK

/* Keep SQL output independent of the batch/TSO entry point. */
#ifndef MINISQL_OUTPUT_IMPL
#define printf msql_printf
#define fflush msql_fflush
#endif

#endif
