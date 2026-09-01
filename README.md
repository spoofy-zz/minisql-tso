# minisql

`minisql` je jednostavna SQL baza za MVS 3.8j / TK5. Program je pisan u C-u
za `cc370`, a build koristi lokalni `mbt` koji se nalazi u ovom direktoriju.

Baza se na MVS-u sprema u VSAM KSDS cluster `IBMUSER.MINISQL.KV`. Program ga
otvara preko DD imena `MINIKV`, pa se isti load modul moze pokretati nad
razlicitim VSAM bazama ako se u JCL-u promijeni `MINIKV DD`.

## Funkcije projekta

Projekt podrzava osnovni SQL tok za male tablice:

```sql
CREATE TABLE LJUDI (ID, IME, GRAD);
INSERT INTO LJUDI VALUES (1, 'ANA', 'ZAGREB');
SELECT * FROM LJUDI;
UPDATE LJUDI SET GRAD='RIJEKA' WHERE ID=1;
DELETE FROM LJUDI WHERE ID=1;
DROP TABLE LJUDI;
```

Dodatne komande:

```text
.TABLES
.SCHEMA LJUDI
.HELP
.QUIT
```

Datoteke:

- `src/minisql.c` - SQL parser, izvrsavanje komandi i VSAM key/value storage.
- `Makefile` - ukljucuje lokalni `mbt/mk/mbt.mk`.
- `project.toml` - MBT projekt, modul `MINISQL`, deploy target
  `IBMUSER.MINISQL.LOAD`.
- `jcl/ALLOCVS.jcl` - kreira i inicijalizira VSAM KSDS
  `IBMUSER.MINISQL.KV`.
- `jcl/MINISQL.jcl` - primjer pokretanja programa s SQL komandama u `SYSIN`.
- `jcl/MSQLTSO.jcl` - samo primjer TSO `CALL` okruzenja. Interaktivni
  `MSQLTSO` treba pokrenuti iz foreground TSO sesije.
- `jcl/RECEIVE.jcl` - prima MBT XMIT paket u load biblioteku
  `IBMUSER.MINISQL.LOAD`.

## Ogranicenja

- Samo tekstualne vrijednosti; nema SQL tipova podataka.
- Nema joinova, indeksa po kolonama, transakcija ni SQL optimizatora.
- Maksimalno 32 tablice.
- Maksimalno 8 kolona po tablici.
- Maksimalno 32 retka po tablici.
- Imena tablica i kolona mogu imati najvise 16 znakova.
- Vrijednost jedne kolone moze imati najvise 32 znaka.
- Jedan spremljeni red mora stati u VSAM payload od 48 bajtova.
- `WHERE` podrzava jednostavan oblik `KOLONA=VRIJEDNOST`.

## Build lokalnim MBT-om

Build se pokrece iz root direktorija projekta:

```bash
make VERBOSE=1
```

Ocekivani rezultat su load moduli:

```text
build/MINISQL
build/MSQLTSO
```

Za generiranje XMIT deploy paketa:

```bash
make deploy ARGS=--dry-run VERBOSE=1
```

Ocekivani rezultat je:

```text
build/minisql.deploy.xmit
```

## Priprema PDS-a na MVS-u

Ako PDS jos ne postoji, kreiraj ga preko Zowe:

```bash
zowe zos-files create data-set-partitioned IBMUSER.MINISQL \
  --record-format FB \
  --record-length 80 \
  --block-size 3120 \
  --directory-blocks 10 \
  --primary-space 5 \
  --secondary-space 2 \
  --allocation-space-unit TRK \
  --zosmf-profile hercules
```

Upload source/JCL membera u PDS:

```bash
zowe zos-files upload stdin-to-data-set "IBMUSER.MINISQL(MINISQL)" \
  --zosmf-profile hercules < src/minisql.c

zowe zos-files upload stdin-to-data-set "IBMUSER.MINISQL(ALLOCVS)" \
  --zosmf-profile hercules < jcl/ALLOCVS.jcl

zowe zos-files upload stdin-to-data-set "IBMUSER.MINISQL(RUNJCL)" \
  --zosmf-profile hercules < jcl/MINISQL.jcl

zowe zos-files upload stdin-to-data-set "IBMUSER.MINISQL(RECEIVE)" \
  --zosmf-profile hercules < jcl/RECEIVE.jcl
```

Lista membera u PDS-u:

```bash
zowe zos-files list all-members IBMUSER.MINISQL --zosmf-profile hercules
```

## Deploy load modula

Nakon `make deploy ARGS=--dry-run VERBOSE=1`, upload XMIT paketa u staging
dataset:

```bash
zowe zos-files upload file-to-data-set build/minisql.deploy.xmit \
  IBMUSER.MBT.XMIT.IN \
  --binary \
  --zosmf-profile hercules
```

Zatim submitaj RECEIVE job:

```bash
zowe zos-jobs submit local-file jcl/RECEIVE.jcl \
  --wait-for-output \
  --zosmf-profile hercules
```

Ako job zavrsi s `CC 0000`, load moduli su primljeni u:

```text
IBMUSER.MINISQL.LOAD(MINISQL)
IBMUSER.MINISQL.LOAD(MSQLTSO)
```

## Kreiranje VSAM baze

`jcl/ALLOCVS.jcl` brise stari cluster i kreira novi:

```text
IBMUSER.MINISQL.KV
```

Submit:

```bash
zowe zos-jobs submit local-file jcl/ALLOCVS.jcl \
  --wait-for-output \
  --zosmf-profile hercules
```

Pokreni ovaj job kada zelis resetirati bazu. Ako zelis sacuvati podatke, nemoj
ponovno submitati `ALLOCVS.jcl`.

## Koristenje unutar JCL-a

Program se pokrece kao batch program:

```jcl
//RUN      EXEC PGM=MINISQL
//STEPLIB  DD DISP=SHR,DSN=IBMUSER.MINISQL.LOAD
//MINIKV   DD DISP=OLD,DSN=IBMUSER.MINISQL.KV
//SYSOUT   DD SYSOUT=*
//SYSPRINT DD SYSOUT=*
//SYSIN    DD *
CREATE TABLE LJUDI (ID, IME, GRAD);
INSERT INTO LJUDI VALUES (1, 'ANA', 'ZAGREB');
SELECT * FROM LJUDI;
.QUIT
/*
```

Bitni DD statementi:

- `STEPLIB` pokazuje na load biblioteku gdje je `MINISQL`.
- `MINIKV` pokazuje na VSAM KSDS koji program koristi kao bazu.
- `SYSIN` sadrzi SQL komande.
- `SYSPRINT` sadrzi rezultat programa.

Svaka SQL komanda treba zavrsiti sa `;`. Komande `.TABLES`, `.SCHEMA`,
`.HELP` i `.QUIT` mogu biti bez `;`. U TSO sesiji `//HELP` i `//QUIT` rade
kao aliasi za `.HELP` i `.QUIT`.

## Submit SQL joba preko Zowe

Primjer pokretanja postojeceg JCL-a:

```bash
zowe zos-jobs submit local-file jcl/MINISQL.jcl \
  --wait-for-output \
  --zosmf-profile hercules
```

Primjer uspjesnog rezultata:

```text
jobid:   JOB00763
retcode: CC 0000
jobname: MINISQL
status:  OUTPUT
```

`JOB00763` ce na tvom sistemu biti drugi broj. Taj broj koristi za dohvat
spool outputa.

## Dohvacanje outputa preko Zowe

Prvo izlistaj spool datoteke za job:

```bash
zowe zos-jobs list spool-files-by-jobid JOB00763 \
  --zosmf-profile hercules
```

Primjer:

```text
1   JESJCLIN   JES2
2   JESMSGLG   JES2
3   JESJCL     JES2
4   JESYSMSG   JES2
101 SYSIN      RUN
103 SYSPRINT   RUN
```

Rezultat `minisql` programa je u `SYSPRINT`. Procitaj ga preko DDID-a iz liste:

```bash
zowe zos-jobs view spool-file-by-id JOB00763 103 \
  --zosmf-profile hercules
```

Primjer outputa:

```text
MINISQL MVS READY
END STATEMENTS WITH ;  USE .QUIT TO EXIT
OK TABLE CREATED
OK 1 ROW INSERTED
OK 1 ROW INSERTED
ID | IME | GRAD 1 | ANA | ZAGREB 2 | IVO | SPLIT OK 2 ROWS
OK 1 ROWS UPDATED
ID | IME | GRAD 1 | ANA | ZAGREB 2 | IVO | RIJEKA OK 2 ROWS
OK 1 ROWS DELETED
ID | IME | GRAD 2 | IVO | RIJEKA OK 1 ROWS
```

## TSO interaktivni processor

Jednostavna TSO varijanta je load modul `MSQLTSO`. Koristi isti SQL engine i
isti VSAM DD `MINIKV`, ali ulaz cita preko TSO `TGET` i ispisuje TSO prompt:

```text
SQL>
```

U pravoj TSO sesiji prvo alociraj VSAM bazu, pa pozovi load modul:

```text
ALLOC FI(MINIKV) DA('IBMUSER.MINISQL.KV') OLD
CALL 'IBMUSER.MINISQL.LOAD(MSQLTSO)'
```

Primjer rada:

```text
MINISQL TSO READY
END STATEMENTS WITH ;  USE .QUIT TO EXIT
SQL> CREATE TABLE LJUDI (ID, IME);
OK TABLE CREATED
SQL> INSERT INTO LJUDI VALUES (1, 'ANA');
OK 1 ROW INSERTED
SQL> SELECT * FROM LJUDI;
ID | IME
1 | ANA
OK 1 ROWS
SQL> //HELP
COMMANDS:
  CREATE TABLE name (col1, col2, ...);
  INSERT INTO name VALUES (v1, v2, ...);
  SELECT * FROM name [WHERE col=value];
  UPDATE name SET col=value WHERE col=value;
  DELETE FROM name WHERE col=value;
  DROP TABLE name;
  .TABLES
  .SCHEMA name
  .HELP or //HELP
  .QUIT
SQL> .QUIT
```

Napomena: `MSQLTSO` nije batch SQL runner. Nakon prelaska na `TGET`, Zowe
batch submit preko `IKJEFT01` ne moze glumiti pravu interaktivnu terminalsku
sesiju. Za batch SQL koristi `MINISQL` i `jcl/MINISQL.jcl`.

## Brzi end-to-end tok

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

Zatim dohvati `SYSPRINT`:

```bash
zowe zos-jobs list spool-files-by-jobid JOBID --zosmf-profile hercules
zowe zos-jobs view spool-file-by-id JOBID DDID --zosmf-profile hercules
```
