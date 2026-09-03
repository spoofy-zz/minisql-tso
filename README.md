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
CREATE INDEX IDXCITY ON PEOPLE (CITY);
SELECT * FROM PEOPLE WHERE CITY='ZAGREB';
SELECT * FROM PEOPLE WHERE ID BETWEEN 1 AND 3;
SELECT * FROM PEOPLE WHERE NAME LIKE 'A%' OR CITY='RIJEKA';
CREATE TABLE ORDERS (OID INT PRIMARY KEY, PERSON_ID INT,
ITEM VARCHAR(8), FOREIGN KEY (PERSON_ID) REFERENCES PEOPLE(ID));
INSERT INTO ORDERS VALUES (100, 1, 'BOOK');
UPDATE PEOPLE SET CITY='RIJEKA' WHERE ID=1;
DELETE FROM PEOPLE WHERE ID=1;
DROP TABLE PEOPLE;
```

Utility commands:

```text
.TABLES
.SCHEMA PEOPLE
.HELP
.QUIT
```

Files:

- `src/minisql.c` - SQL parser, command execution, and VSAM key/value storage.
- `src/msqltso.c` - TSO build wrapper for the same SQL engine.
- `asm/msqtget.asm` - C-callable TSO `TGET` wrapper for interactive input.
- `asm/msqtput.asm` - C-callable TSO `TPUT` wrapper for normal TSO output.
- `clist/MSQL.clist` - TSO CLIST that allocates `MINIKV`, starts `MSQLTSO`,
  and frees the DD after exit.
- `Makefile` - includes the local `mbt/mk/mbt.mk`.
- `project.toml` - MBT project definition and deploy target.
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

Generate the XMIT deploy package:

```bash
make deploy ARGS=--dry-run VERBOSE=1
```

Expected output:

```text
build/minisql.deploy.xmit
```

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

Upload source and JCL members:

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

Upload the TSO launcher CLIST:

```bash
zowe zos-files upload file-to-data-set clist/MSQL.clist \
  "SYS2.CMDPROC(MSQL)" \
  --zosmf-profile hercules
```

## Deploy Load Modules

After `make deploy ARGS=--dry-run VERBOSE=1`, upload the XMIT package to the
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
SELECT * FROM PEOPLE WHERE CITY='ZAGREB';
SELECT * FROM PEOPLE WHERE ID > 0 AND NAME LIKE 'A%';
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
- prints `.SCHEMA PEOPLE`
- tests `SELECT * FROM PEOPLE`
- tests `SELECT * FROM PEOPLE WHERE CITY='RIJEKA'`
- tests `WHERE` expressions with `AND`, `OR`, `LIKE`, `<`, `>` and `BETWEEN`
- creates `ORDERS` with a single-column foreign key to `PEOPLE(ID)`
- tests valid and invalid foreign key inserts
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
ID | NAME | CITY | AGE | EMAIL
2 | IVO | RIJEKA | 41 | IVO@EX
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
It does not add an empty line to the pending SQL statement.

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
SELECT * FROM PEOPLE;
SELECT * FROM PEOPLE WHERE CITY='RIJEKA';
INSERT INTO PEOPLE VALUES (3, 'PERO', 'RIJEKA', 22, 'PERO@EX');
INSERT INTO PEOPLE VALUES ('ABC', 'PERO', 'RIJEKA', 22, 'PERO@EX');
INSERT INTO PEOPLE VALUES (4, 'PREDUGOIME', 'RIJEKA', 22, 'LONG@EX');
CREATE TABLE ORDERS (OID INT PRIMARY KEY, PERSON_ID INT,
ITEM VARCHAR(8), FOREIGN KEY (PERSON_ID) REFERENCES PEOPLE(ID));
INSERT INTO ORDERS VALUES (100, 3, 'BOOK');
INSERT INTO ORDERS VALUES (101, 99, 'BAD');
SELECT * FROM PEOPLE WHERE ID > 1 AND CITY LIKE 'RI%';
SELECT * FROM PEOPLE WHERE ID BETWEEN 1 AND 3;
SELECT * FROM PEOPLE WHERE NAME LIKE 'P%' OR CITY='SPLIT';
SELECT * FROM PEOPLE WHERE CITY='RIJEKA';
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
- single-column foreign keys validate child values against parent primary keys.
- `ID` accepts only whole numbers because it is `INT`.
- `NAME` accepts at most 8 characters because it is `VARCHAR(8)`.
- duplicate `ID=3` returns `ERR DUPLICATE PRIMARY KEY`.
- changing a primary key column returns `ERR CANNOT UPDATE PRIMARY KEY`.
- changing a non-key column such as `CITY` works and rebuilds indexes.

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
  `WHERE` operators `<`, `>` and `BETWEEN`.
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
- Maximum 256 rows per table.
- Maximum 4 secondary indexes per table.
- `PRIMARY KEY` supports one column only.
- A table may define up to 4 single-column foreign keys.
- Foreign keys must reference the primary key column of an existing table.
- Foreign keys reject invalid child inserts/updates and referenced parent
  deletes. There is no cascade update/delete.
- `CREATE INDEX` supports one column per index.
- Table, column, and index names may have at most 16 characters.
- One column value may have at most 32 characters.
- One stored row payload must fit in 960 bytes, including commas between
  values.
- VSAM record layout is fixed: 64-byte key, 960-byte data, 1024 bytes total.
  If this layout changes, recreate `IBMUSER.MINISQL.KV` with
  `jcl/ALLOCVS.jcl`.
- Only `SELECT * FROM table` is supported, with an optional `WHERE` expression.
- `WHERE` supports `=`, `<`, `>`, `LIKE`, `BETWEEN`, `AND` and `OR`.
- `WHERE` does not support parentheses, `NOT`, `<=`, `>=`, `<>`, `!=`, `IN`,
  `IS NULL`, or functions.
- `AND` has higher precedence than `OR`, as in SQL.
- A secondary index is used only for `SELECT * FROM table WHERE col=value`
  when an index exists on `col`; complex `WHERE` expressions use a linear scan.
- `UPDATE` supports one `SET col=value` and an optional `WHERE` expression.
- `DELETE` supports an optional `WHERE` expression; without `WHERE`, it deletes
  all rows in the table.
- There are no joins, order by, group by, aggregates, views, stored
  procedures, transactions, rollback, or recovery log.
- `MSQLTSO` is an interactive foreground TSO program. Use `MINISQL`, not
  `MSQLTSO`, for batch SQL.
