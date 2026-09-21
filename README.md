# minisql

`minisql` is a small SQL database for MVS 3.8j / TK5. The program is written
in C for `cc370`, and the build uses the local `mbt` toolchain in this
directory.

The database is stored on MVS in the VSAM KSDS cluster `IBMUSER.MINISQL.KV`.
The program opens it through DD name `MINIKV`, so the same load module can run
against a different VSAM database by changing the `MINIKV DD` statement.

## Project Features

The project supports a small SQL workflow for small tables:

```sql
CREATE TABLE PEOPLE (ID INT PRIMARY KEY, NAME VARCHAR(8), CITY CHAR(8),
AGE INT, EMAIL VARCHAR(16));
INSERT INTO PEOPLE VALUES (1, 'ANA', 'ZAGREB', 30, 'ANA@EX');
SELECT * FROM PEOPLE;
SELECT COUNT(*) FROM PEOPLE;
SELECT NAME, CITY FROM PEOPLE;
CREATE INDEX IDXCITY ON PEOPLE (CITY);
SELECT * FROM PEOPLE WHERE CITY='ZAGREB';
EXPLAIN SELECT * FROM PEOPLE WHERE CITY='ZAGREB';
SELECT * FROM PEOPLE WHERE ID BETWEEN 1 AND 3;
SELECT * FROM PEOPLE WHERE AGE>=30 ORDER BY AGE DESC LIMIT 1;
SELECT * FROM PEOPLE WHERE NAME LIKE 'A%' OR CITY='RIJEKA';
SELECT * FROM PEOPLE ORDER BY AGE DESC;
SELECT * FROM PEOPLE GROUP BY CITY ORDER BY COUNT DESC;
CREATE TABLE ORDERS (OID INT PRIMARY KEY, PERSON_ID INT,
ITEM VARCHAR(8), FOREIGN KEY (PERSON_ID) REFERENCES PEOPLE(ID));
INSERT INTO ORDERS VALUES (100, 1, 'BOOK');
BEGIN;
INSERT INTO ORDERS VALUES (101, 1, 'PEN');
ROLLBACK;
CREATE INDEX IDXPID ON ORDERS (PERSON_ID);
SELECT * FROM PEOPLE JOIN ORDERS ON PEOPLE.ID=ORDERS.PERSON_ID;
SELECT P.NAME,O.ITEM FROM PEOPLE P JOIN ORDERS O ON P.ID=O.PERSON_ID;
DROP INDEX IDXPID;
UPDATE PEOPLE SET CITY='RIJEKA' WHERE ID=1;
DELETE FROM PEOPLE WHERE ID=1;
DROP TABLE PEOPLE;
```

Utility commands:

```text
.TABLES
.SCHEMA PEOPLE
DESC PEOPLE
BEGIN
COMMIT
ROLLBACK
.CLEAR
.HELP
.QUIT
```

## Transaction Support

`BEGIN`, `COMMIT` and `ROLLBACK` provide a small KV-level transaction layer.
Before each write or delete, minisql records the previous value in journal
records in the same VSAM KSDS. `ROLLBACK` replays that journal backward and
restores the prior state; `COMMIT` removes the journal records.

Mutating statements outside an explicit transaction run inside an implicit
transaction, so a single `CREATE`, `INSERT`, `UPDATE`, `DELETE` or `DROP`
either finishes with its journal cleared or can be recovered on the next run.
At startup, minisql checks for an active journal and rolls it back before
reading the catalog.

## Storage And Indexes

Rows are no longer capped by a fixed 256-row array in the engine. Table scans
load matching row records into dynamically growing rowsets, and row records
are addressed through the six-digit row-slot portion of the VSAM key.
Practical table size is therefore limited by VSAM space, available memory, and
the `999999` row-slot key space.

Secondary indexes are stored as their own sorted KV entries using the indexed
value and row slot in the key. Equality predicates scan the selected index
namespace and then re-check the matching row value:

```sql
CREATE INDEX IDXCITY ON PEOPLE (CITY);
SELECT * FROM PEOPLE WHERE CITY='ZAGREB';
EXPLAIN SELECT * FROM PEOPLE WHERE CITY='ZAGREB';
DROP INDEX IDXCITY;
```

Indexes are maintained by rebuilding the table's secondary index records after
row changes. This is simple and robust for small MVS/TK5 workloads, but it is
not a SQLite-style page-level B-tree implementation.

`EXPLAIN SELECT ...` prints a compact plan summary. It reports whether a
single-table query uses a secondary index namespace or a table scan, and
whether a join uses the right-hand table's index namespace or falls back to a
nested-loop scan.

## Simple Joins

minisql supports one equality inner join form:

```sql
SELECT * FROM PEOPLE JOIN ORDERS ON PEOPLE.ID=ORDERS.PERSON_ID;
SELECT P.NAME,O.ITEM FROM PEOPLE P JOIN ORDERS O ON P.ID=O.PERSON_ID;
```

The join supports `*` or a comma-separated list of qualified columns. Table
aliases may be written as `PEOPLE P` or `PEOPLE AS P`. Output column headers
are qualified, for example `PEOPLE.ID`, `ORDERS.OID`, `P.NAME` or `O.ITEM`.
When the right-hand join column has a secondary index, minisql uses that index
for lookup; otherwise it falls back to a nested-loop scan. Joins currently do
not support unqualified join projections, `WHERE`, `ORDER BY`, `GROUP BY`,
outer joins or more than two tables.

## Interactive Screen Clear

`.CLEAR`, `//CLEAR` or `CLEAR` clears the interactive display and writes a
fresh `SQL> ` prompt immediately. On ANSI-capable local terminals, minisql
uses the standard clear-screen and home-cursor escape sequence. Under the TSO
line-mode wrapper, minisql uses `STLINENO LINE=1,MODE=OFF` to reset the
next output to the first screen line, then writes the prompt through `TPUT`.

Files:

- `src/minisql.c`, `src/msqltso.c` - batch and interactive entry points.
- `src/process.c` - statement input, command dispatch and terminal recall.
- `src/output.c` - standard output and interactive MVS TPUT output.
- `src/storage.c` - VSAM/host key-value storage and transaction journal.
- `src/parser.c` - token parsing, data types and WHERE expressions.
- `src/catalog.c` - table metadata serialization and catalog lookup.
- `src/rows.c` - row sets, row persistence, indexes and foreign-key checks.
- `src/schema.c` - table/index DDL and schema inspection commands.
- `src/mutate.c` - INSERT, UPDATE and DELETE commands.
- `src/select.c` - SELECT, joins, grouping, sorting and EXPLAIN.
- `include/minisql.h` - shared engine types and internal module interfaces.
- `asm/msqtget.asm` - C-callable TSO `TGET` wrapper for interactive input.
- `asm/msqtput.asm` - C-callable TSO `TPUT` wrapper for normal TSO output.
- `clist/MSQL.clist` - TSO CLIST that allocates `MINIKV`, starts `MSQLTSO`,
  and frees the DD after exit.
- `.env.example` - sample local MVS target configuration.
- `Makefile` - includes the local `mbt/mk/mbt.mk`.
- `project.toml` - MBT project definition and deploy target.
- `tools/deploy-mvs.sh` - `.env` driven deploy wrapper that uploads load
  modules, source/JCL members, README, and `SYS2.CMDPROC(MSQL)`.
- `jcl/COMPILE.jcl` - receives the MBT XMIT package into the load library.
- `jcl/ALLOCVS.jcl` - creates and initializes `IBMUSER.MINISQL.KV`.
- `jcl/MINISQL.jcl` - batch SQL test using `SYSIN`.
- `jcl/MSQLTSO.jcl` - example TSO `CALL` environment.
- `jcl/RECEIVE.jcl` - receives the MBT XMIT package into the load library.

## Build With Local MBT

Run the build from the project root:

```bash
make VERBOSE=1
```

Expected load modules:

```text
build/MINISQL
build/MSQLTSO
```

Both entry points link the same separately compiled engine modules. The
processor selects batch or interactive I/O at runtime; on MVS both load
modules link the terminal wrappers, which are used only in interactive mode.
The internal header supplies explicit MVS linker names where long C function
names would otherwise collide after truncation to eight characters.

Build and test locally with the host C compiler (no MVS connection needed):

```bash
make check-host
```

This builds `build/host/minisql` and `build/host/msqltso`, runs the SELECT
formatting and 3270 tests, and compares batch SQL, join and transaction output with
snapshots captured before the module split. Host storage is in memory and
lasts only for the current process. Set `HOST_CC` or `HOST_CFLAGS` to override
the host compiler or its flags.

Generate the XMIT deploy package:

```bash
make deploy ARGS=--dry-run VERBOSE=1
```

Expected output:

```text
build/minisql.deploy.xmit
```

## Configure MVS Deploy Target

Copy the example environment file and edit it for the target MVS host:

```bash
cp .env.example .env
```

Important variables:

```text
MINISQL_TOOLCHAIN_BIN=/home/bnovak/.local/bin

MBT_MVS_HOST=192.168.178.76
MBT_MVS_PORT=1080
MBT_MVS_USER=RVEZ001
MBT_MVS_PASS=CHANGEME
MBT_MVS_HLQ=IBMUSER
MBT_MVS_PROTOCOL=http
MBT_MVS_REJECT_UNAUTHORIZED=false

MINISQL_PDS=IBMUSER.MINISQL
MINISQL_LOADLIB=IBMUSER.MINISQL.LOAD
MINISQL_XMIT_IN=IBMUSER.MBT.XMIT.IN
MINISQL_LOAD_VOLUME=TSO003
MINISQL_CMDPROC=SYS2.CMDPROC
```

`MINISQL_TOOLCHAIN_BIN` is prepended to `PATH` by the Makefile. Set it to the
directory that contains `cc370`, `as370`, `ld370`, and `ar370`.
If it is not set and `$(HOME)/.local/bin/cc370` exists, the Makefile uses
`$(HOME)/.local/bin` automatically.

`make deploy-mvs` reads `.env`, runs the mbt load module deploy to
`MINISQL_LOADLIB` through `MINISQL_XMIT_IN`, uploads the project members to
`MINISQL_PDS`, and uploads the TSO launcher CLIST to
`MINISQL_CMDPROC(MSQL)`.

Dry-run:

```bash
make deploy-mvs-dry-run
```

Live deploy:

```bash
make deploy-mvs
```

For a one-off run with another environment file:

```bash
MINISQL_ENV_FILE=.env.tk5 make deploy-mvs-dry-run
MINISQL_ENV_FILE=.env.tk5 make deploy-mvs
```

If a target uses HTTPS, set `MBT_MVS_PROTOCOL=https` and the HTTPS port in
`.env`. The endpoint must expose z/OSMF-compatible REST paths under `/zosmf`;
otherwise both mbt and Zowe commands will fail with HTTP 404.

## Prepare The MVS PDS

If the PDS does not exist yet, create it with Zowe:

```bash
zowe zos-files create data-set-partitioned IBMUSER.MINISQL \
  --record-format FB \
  --record-length 80 \
  --block-size 3120 \
  --directory-blocks 20 \
  --primary-space 25 \
  --secondary-space 10 \
  --allocation-space-unit TRK \
  --zosmf-profile hercules
```

`make deploy-mvs` uploads the source and JCL members automatically. If you
need to do it manually, use commands like these with the target host/profile
from your environment:

```bash
zowe zos-files upload stdin-to-data-set "IBMUSER.MINISQL(MINISQL)" \
  --zosmf-profile hercules < src/minisql.c

zowe zos-files upload stdin-to-data-set "IBMUSER.MINISQL(MSQLTSO)" \
  --zosmf-profile hercules < src/msqltso.c

zowe zos-files upload stdin-to-data-set "IBMUSER.MINISQL(MSQTGET)" \
  --zosmf-profile hercules < asm/msqtget.asm

zowe zos-files upload stdin-to-data-set "IBMUSER.MINISQL(MSQTPUT)" \
  --zosmf-profile hercules < asm/msqtput.asm

zowe zos-files upload stdin-to-data-set "IBMUSER.MINISQL(COMPILE)" \
  --zosmf-profile hercules < jcl/COMPILE.jcl

zowe zos-files upload stdin-to-data-set "IBMUSER.MINISQL(ALLOCVS)" \
  --zosmf-profile hercules < jcl/ALLOCVS.jcl

zowe zos-files upload stdin-to-data-set "IBMUSER.MINISQL(RUNJCL)" \
  --zosmf-profile hercules < jcl/MINISQL.jcl
```

List PDS members:

```bash
zowe zos-files list all-members IBMUSER.MINISQL --zosmf-profile hercules
```

`make deploy-mvs` also uploads the TSO launcher CLIST automatically. Manual
upload example:

```bash
zowe zos-files upload file-to-data-set clist/MSQL.clist \
  "SYS2.CMDPROC(MSQL)" \
  --zosmf-profile hercules
```

## Manual Load Module Deploy

Normally use `make deploy-mvs`, which reads `.env` and transfers the CLIST as
part of the deploy. For a manual load-module-only deploy, run
`make deploy ARGS=--dry-run VERBOSE=1`, then upload the XMIT package to the
staging data set:

```bash
zowe zos-files upload file-to-data-set build/minisql.deploy.xmit \
  IBMUSER.MBT.XMIT.IN \
  --binary \
  --zosmf-profile hercules
```

Then submit the local RECEIVE job:

```bash
zowe zos-jobs submit local-file jcl/RECEIVE.jcl \
  --wait-for-output \
  --zosmf-profile hercules
```

If you work from the MVS PDS, member `IBMUSER.MINISQL(COMPILE)` performs the
same RECEIVE step for `IBMUSER.MBT.XMIT.IN -> IBMUSER.MINISQL.LOAD`.

When the job ends with `CC 0000`, the load modules are available as:

```text
IBMUSER.MINISQL.LOAD(MINISQL)
IBMUSER.MINISQL.LOAD(MSQLTSO)
```

## Create The VSAM Database

`jcl/ALLOCVS.jcl` deletes the old cluster and creates a new one. The cluster
uses `KEYS(64 0)` and `RECORDSIZE(1024 1024)`, matching the current `KV_KEY`
and `KV_DATA` layout in the program:

```text
IBMUSER.MINISQL.KV
```

Submit:

```bash
zowe zos-jobs submit local-file jcl/ALLOCVS.jcl \
  --wait-for-output \
  --zosmf-profile hercules
```

Run this job when you want to reset the database. Do not submit `ALLOCVS.jcl`
again if you want to keep existing data.

## Use From JCL

The batch program is started as `PGM=MINISQL`:

```jcl
//RUN      EXEC PGM=MINISQL
//STEPLIB  DD DISP=SHR,DSN=IBMUSER.MINISQL.LOAD
//MINIKV   DD DISP=OLD,DSN=IBMUSER.MINISQL.KV
//SYSOUT   DD SYSOUT=*
//SYSPRINT DD SYSOUT=*
//SYSIN    DD *
CREATE TABLE PEOPLE (ID INT PRIMARY KEY, NAME VARCHAR(8), CITY CHAR(8),
AGE INT, EMAIL VARCHAR(16));
INSERT INTO PEOPLE VALUES (1, 'ANA', 'ZAGREB', 30, 'ANA@EX');
CREATE INDEX IDXCITY ON PEOPLE (CITY);
SELECT * FROM PEOPLE;
SELECT COUNT(*) FROM PEOPLE;
SELECT NAME, CITY FROM PEOPLE;
SELECT * FROM PEOPLE WHERE CITY='ZAGREB';
SELECT * FROM PEOPLE WHERE ID > 0 AND NAME LIKE 'A%';
SELECT * FROM PEOPLE ORDER BY AGE DESC;
SELECT * FROM PEOPLE GROUP BY CITY ORDER BY COUNT DESC;
.QUIT
/*
```

Important DD statements:

- `STEPLIB` points to the load library containing `MINISQL`.
- `MINIKV` points to the VSAM KSDS used as the database.
- `SYSIN` contains SQL commands.
- `SYSPRINT` contains program output.

Each SQL command must end with `;`. Commands `.TABLES`, `.SCHEMA`, `.HELP`,
and `.QUIT` may be entered without `;`. In a TSO session, `//HELP` and
`//QUIT` are accepted as aliases for `.HELP` and `.QUIT`. Blank input lines
are ignored and are not appended to the current SQL statement buffer.

## Test From JCL

The standard batch test is `jcl/MINISQL.jcl`. The job:

- creates table `PEOPLE` with `ID PRIMARY KEY` and five columns
- inserts two valid rows
- tests `INT` validation and `VARCHAR` length validation
- creates secondary index `IDXCITY` on column `CITY`
- tests `BEGIN` and `ROLLBACK`
- prints `.SCHEMA PEOPLE`
- tests `DESC PEOPLE` and `DESCRIBE ORDERS`
- tests `SELECT * FROM PEOPLE`
- tests `SELECT COUNT(*) FROM PEOPLE`
- tests selected-column output such as `SELECT NAME,CITY FROM PEOPLE`
- tests `SELECT * FROM PEOPLE WHERE CITY='RIJEKA'`
- tests `WHERE` expressions with `AND`, `OR`, `LIKE`, `<`, `>` and `BETWEEN`
- tests `ORDER BY` on one column
- tests `GROUP BY` on one column with automatic `COUNT`
- creates `ORDERS` with a single-column foreign key to `PEOPLE(ID)`
- tests valid and invalid foreign key inserts
- creates a secondary index on `ORDERS(PERSON_ID)` and tests a simple join
- tests delete protection for referenced parent rows
- tests `UPDATE` and `DELETE`

First reset the VSAM database if you want a clean test:

```bash
zowe zos-jobs submit local-file jcl/ALLOCVS.jcl \
  --wait-for-output \
  --zosmf-profile hercules
```

Then submit the SQL test:

```bash
zowe zos-jobs submit local-file jcl/MINISQL.jcl \
  --wait-for-output \
  --zosmf-profile hercules
```

Example successful result:

```text
jobid:   JOB00805
retcode: CC 0000
jobname: MINISQL
status:  OUTPUT
```

Your `JOBID` will be different. Use that job id to retrieve spool output.

List spool files:

```bash
zowe zos-jobs list spool-files-by-jobid JOB00805 \
  --zosmf-profile hercules
```

Example:

```text
1   JESJCLIN   JES2
2   JESMSGLG   JES2
3   JESJCL     JES2
4   JESYSMSG   JES2
101 SYSIN      RUN
103 SYSPRINT   RUN
```

Program output is in `SYSPRINT`. Read it by DDID:

```bash
zowe zos-jobs view spool-file-by-id JOB00805 103 \
  --zosmf-profile hercules
```

Expected important output:

```text
OK TABLE CREATED
PEOPLE(ID INT PRIMARY KEY, NAME VARCHAR(8), CITY CHAR(8), AGE INT,
EMAIL VARCHAR(16))
ERR BAD INT VALUE
ERR VALUE TOO LONG
OK INDEX CREATED
INDEX IDXCITY ON PEOPLE(CITY)
FOREIGN KEY PERSON_ID REFERENCES PEOPLE(ID)
ERR FOREIGN KEY NOT FOUND
ID | NAME | CITY   | AGE | EMAIL
---|------|--------|-----|-------
2  | IVO  | RIJEKA | 41  | IVO@EX
OK 1 ROWS
```

## Test From TSO

The interactive processor is load module `MSQLTSO`. The easiest way to start
it is through CLIST `SYS2.CMDPROC(MSQL)`. The CLIST allocates VSAM DD
`MINIKV`, calls the load module, and runs `FREE FI(MINIKV)` after exit.

From a foreground TSO session, run:

```text
MSQL
```

At the `SQL>` prompt, pressing Enter on an empty line just redraws the prompt.
Blank or terminal-control-only input lines do not add anything to the pending
SQL statement.

If the CLIST is not in your `SYSPROC` or `SYSEXEC` search path, run it from
the appropriate command procedure library, or start the processor manually:

```text
ALLOC FI(MINIKV) DA('IBMUSER.MINISQL.KV') OLD
CALL 'IBMUSER.MINISQL.LOAD(MSQLTSO)'
FREE FI(MINIKV)
```

Interactive test:

```sql
.TABLES
.SCHEMA PEOPLE
DESC PEOPLE
SELECT * FROM PEOPLE;
SELECT COUNT(*) FROM PEOPLE;
SELECT NAME,CITY FROM PEOPLE;
SELECT * FROM PEOPLE WHERE CITY='RIJEKA';
INSERT INTO PEOPLE VALUES (3, 'PERO', 'RIJEKA', 22, 'PERO@EX');
INSERT INTO PEOPLE VALUES ('ABC', 'PERO', 'RIJEKA', 22, 'PERO@EX');
INSERT INTO PEOPLE VALUES (4, 'PREDUGOIME', 'RIJEKA', 22, 'LONG@EX');
CREATE TABLE ORDERS (OID INT PRIMARY KEY, PERSON_ID INT,
ITEM VARCHAR(8), FOREIGN KEY (PERSON_ID) REFERENCES PEOPLE(ID));
DESCRIBE ORDERS
INSERT INTO ORDERS VALUES (100, 3, 'BOOK');
INSERT INTO ORDERS VALUES (101, 99, 'BAD');
SELECT * FROM PEOPLE WHERE ID > 1 AND CITY LIKE 'RI%';
SELECT * FROM PEOPLE WHERE ID BETWEEN 1 AND 3;
SELECT * FROM PEOPLE WHERE NAME LIKE 'P%' OR CITY='SPLIT';
SELECT * FROM PEOPLE ORDER BY AGE DESC;
SELECT * FROM PEOPLE GROUP BY CITY ORDER BY COUNT DESC;
SELECT * FROM PEOPLE WHERE CITY='RIJEKA';
//CLEAR
UPDATE PEOPLE SET CITY='SISAK' WHERE ID=3;
SELECT * FROM PEOPLE WHERE CITY='RIJEKA';
SELECT * FROM PEOPLE WHERE CITY='SISAK';
INSERT INTO PEOPLE VALUES (3, 'DUP', 'OSIJEK');
UPDATE PEOPLE SET ID=4 WHERE ID=3;
.QUIT
```

Expected behavior:

- `.SCHEMA PEOPLE` shows `ID PRIMARY KEY` and `INDEX IDXCITY ON PEOPLE(CITY)`.
- `SELECT ... WHERE CITY='RIJEKA'` uses the secondary index when available.
- `WHERE` supports `AND`, `OR`, `LIKE`, `<`, `>` and `BETWEEN`.
- `ORDER BY` sorts by one table column, or by `COUNT` for grouped results.
- `GROUP BY` supports one column and returns `column | COUNT`.
- single-column foreign keys validate child values against parent primary keys.
- `ID` accepts only whole numbers because it is `INT`.
- `NAME` accepts at most 8 characters because it is `VARCHAR(8)`.
- duplicate `ID=3` returns `ERR DUPLICATE PRIMARY KEY`.
- changing a primary key column returns `ERR CANNOT UPDATE PRIMARY KEY`.
- changing a non-key column such as `CITY` works and rebuilds indexes.
- `CLEAR`, `.CLEAR` or `//CLEAR` refreshes the interactive display and leaves a
  new `SQL> ` prompt ready for input.
- In TSO, PF12, `!!`, `REPEAT` and `.REPEAT` recall the last SQL statement
  into an editable 3270 input field after `SQL>`. The cursor is placed at
  the end. Edit the text and press Enter to submit it; recall itself never
  executes SQL. The recall screen supports up to 1913 characters, including
  statements originally entered over multiple lines. Longer statements
  report an error instead of being truncated.

At the end, release the DD if you started `MSQLTSO` manually:

```text
FREE FI(MINIKV)
```

Note: `MSQLTSO` is a foreground interactive TSO program. After switching to
`TGET`, a Zowe batch submit through `IKJEFT01` cannot emulate a real
interactive terminal session. For batch SQL, use `MINISQL` and
`jcl/MINISQL.jcl`.

## Submit SQL Jobs With Zowe

Submit the existing JCL:

```bash
zowe zos-jobs submit local-file jcl/MINISQL.jcl \
  --wait-for-output \
  --zosmf-profile hercules
```

List DDs for the job:

```bash
zowe zos-jobs list spool-files-by-jobid JOBID --zosmf-profile hercules
```

Read the `SYSPRINT` DDID, for example `103`:

```bash
zowe zos-jobs view spool-file-by-id JOBID 103 --zosmf-profile hercules
```

## Quick End-To-End Flow

```bash
make VERBOSE=1

make deploy ARGS=--dry-run VERBOSE=1

zowe zos-files upload file-to-data-set build/minisql.deploy.xmit \
  IBMUSER.MBT.XMIT.IN \
  --binary \
  --zosmf-profile hercules

zowe zos-jobs submit local-file jcl/RECEIVE.jcl \
  --wait-for-output \
  --zosmf-profile hercules

zowe zos-jobs submit local-file jcl/ALLOCVS.jcl \
  --wait-for-output \
  --zosmf-profile hercules

zowe zos-jobs submit local-file jcl/MINISQL.jcl \
  --wait-for-output \
  --zosmf-profile hercules
```

Then retrieve `SYSPRINT`:

```bash
zowe zos-jobs list spool-files-by-jobid JOBID --zosmf-profile hercules
zowe zos-jobs view spool-file-by-id JOBID DDID --zosmf-profile hercules
```

## Limitations

- Supported SQL types are `INT`/`INTEGER`, `CHAR(n)`, `VARCHAR(n)` and `TEXT`.
- Types are validation metadata. Values are still stored as text in the VSAM
  row payload.
- `INT` supports optional `+`/`-`, decimal digits, and numeric comparison in
  `WHERE` operators `<`, `>`, `<=`, `>=` and `BETWEEN`.
- `CHAR(n)` and `VARCHAR(n)` validate maximum length. `CHAR(n)` is not padded
  with spaces to a fixed length.
- SQL types `DATE`, `TIME`, `DECIMAL`, `FLOAT`, `BOOLEAN` and `BLOB` are not
  supported.
- Values are normalized and stored uppercase. Quotes are used only for parsing
  values containing spaces or for clearer SQL syntax.
- There is no `NULL`, default values, check constraints, or constraints beyond
  one primary key and single-column foreign keys.
- Maximum 32 tables.
- Maximum 16 columns per table.
- Rows are loaded into dynamic in-memory rowsets. Stored row slots use the
  six-digit row key space, so practical capacity is bounded by VSAM space,
  memory, and the `999999` row-slot ceiling rather than a 256-row array.
- Maximum 4 secondary indexes per table.
- `PRIMARY KEY` supports one column only.
- A table may define up to 4 single-column foreign keys.
- Foreign keys must reference the primary key column of an existing table.
- Foreign keys reject invalid child inserts/updates and referenced parent
  deletes. There is no cascade update/delete.
- `CREATE INDEX` supports one column per index.
- `DROP INDEX name` removes one secondary index and rebuilds the remaining
  indexes for that table.
- Table, column, and index names may have at most 16 characters.
- One column value may have at most 32 characters.
- One stored row payload must fit in 960 bytes, including commas between
  values.
- VSAM record layout is fixed: 64-byte key, 960-byte data, 1024 bytes total.
  If this layout changes, recreate `IBMUSER.MINISQL.KV` with
  `jcl/ALLOCVS.jcl`.
- `SELECT` supports `*`, a comma-separated column list, or `COUNT(*)`.
- Result columns are left-aligned using the widest displayed value or header,
  with ` | ` separators and a horizontal line below the header. This also applies to grouped results and joins.
- `SELECT` supports optional `WHERE`, `GROUP BY`, `ORDER BY` and `LIMIT`
  clauses.
- `SELECT * FROM a JOIN b ON a.col=b.col` supports one equality join.
- Join queries also support qualified projections and table aliases.
- `WHERE` supports `=`, `<`, `>`, `<=`, `>=`, `<>`, `!=`, `LIKE`, `BETWEEN`,
  `AND` and `OR`.
- `WHERE` does not support parentheses, `NOT`, `IN`, `IS NULL`, or functions.
- `AND` has higher precedence than `OR`, as in SQL.
- `ORDER BY` supports one table column with optional `ASC` or `DESC`.
- `LIMIT n` limits row or group output for `SELECT`.
- `GROUP BY` supports one table column and returns that column plus `COUNT`.
- Grouped `ORDER BY` supports the grouped column or `COUNT`.
- `COUNT(*)` supports an optional `WHERE`; with `GROUP BY`, it counts each
  group.
- `BEGIN`, `COMMIT` and `ROLLBACK` are supported. Mutating statements outside
  an explicit transaction run in an implicit transaction. Startup recovery
  rolls back an active journal left by an interrupted run.
- `DESC table` and `DESCRIBE table` show columns, data types, key roles and
  foreign key references.
- A secondary index is used for `SELECT * FROM table WHERE col=value` when an
  index exists on `col` and no `GROUP BY` or `ORDER BY` is used. Simple joins
  use an index on the right-hand join column when available; complex `WHERE`
  expressions use a linear scan.
- `UPDATE` supports one `SET col=value` and an optional `WHERE` expression.
- `DELETE` supports an optional `WHERE` expression; without `WHERE`, it deletes
  all rows in the table.
- There are no outer joins, multi-table planners, general aggregate functions,
  views, stored procedures, triggers, page-level B-trees or WAL mode.
- `MSQLTSO` is an interactive foreground TSO program. Use `MINISQL`, not
  `MSQLTSO`, for batch SQL.
