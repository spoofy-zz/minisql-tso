# minisql

`minisql` je jednostavna SQL baza za MVS 3.8j / TK5. Program je pisan u C-u
za `cc370`, a build koristi lokalni `mbt` koji se nalazi u ovom direktoriju.

Baza se na MVS-u sprema u VSAM KSDS cluster `IBMUSER.MINISQL.KV`. Program ga
otvara preko DD imena `MINIKV`, pa se isti load modul moze pokretati nad
razlicitim VSAM bazama ako se u JCL-u promijeni `MINIKV DD`.

## Funkcije projekta

Projekt podrzava osnovni SQL tok za male tablice:

```sql
CREATE TABLE LJUDI (ID PRIMARY KEY, IME, GRAD);
INSERT INTO LJUDI VALUES (1, 'ANA', 'ZAGREB');
SELECT * FROM LJUDI;
CREATE INDEX IDXGRAD ON LJUDI (GRAD);
SELECT * FROM LJUDI WHERE GRAD='ZAGREB';
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
- `src/msqltso.c` - TSO build wrapper za isti SQL engine.
- `asm/msqtget.asm` - C-callable TSO `TGET` wrapper za interaktivni input.
- `asm/msqtput.asm` - C-callable TSO `TPUT` wrapper za normalan TSO output.
- `Makefile` - ukljucuje lokalni `mbt/mk/mbt.mk`.
- `project.toml` - MBT projekt, modul `MINISQL`, deploy target
  `IBMUSER.MINISQL.LOAD`.
- `jcl/COMPILE.jcl` - prima MBT XMIT paket u load biblioteku
  `IBMUSER.MINISQL.LOAD`.
- `jcl/ALLOCVS.jcl` - kreira i inicijalizira VSAM KSDS
  `IBMUSER.MINISQL.KV`.
- `jcl/MINISQL.jcl` - primjer pokretanja programa s SQL komandama u `SYSIN`.
- `jcl/MSQLTSO.jcl` - samo primjer TSO `CALL` okruzenja. Interaktivni
  `MSQLTSO` treba pokrenuti iz foreground TSO sesije.
- `jcl/RECEIVE.jcl` - prima MBT XMIT paket u load biblioteku
  `IBMUSER.MINISQL.LOAD`.

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

Zatim submitaj lokalni RECEIVE job:

```bash
zowe zos-jobs submit local-file jcl/RECEIVE.jcl \
  --wait-for-output \
  --zosmf-profile hercules
```

Ako radis iz MVS PDS-a, isti posao radi member `IBMUSER.MINISQL(COMPILE)`.
U njemu je RECEIVE dio za `IBMUSER.MBT.XMIT.IN -> IBMUSER.MINISQL.LOAD`.

Ako job zavrsi s `CC 0000`, load moduli su primljeni u:

```text
IBMUSER.MINISQL.LOAD(MINISQL)
IBMUSER.MINISQL.LOAD(MSQLTSO)
```

## Kreiranje VSAM baze

`jcl/ALLOCVS.jcl` brise stari cluster i kreira novi. Cluster koristi
`KEYS(64 0)` i `RECORDSIZE(256 256)`, sto odgovara trenutnom `KV_KEY` i
`KV_DATA` layoutu u programu:

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
CREATE TABLE LJUDI (ID PRIMARY KEY, IME, GRAD);
INSERT INTO LJUDI VALUES (1, 'ANA', 'ZAGREB');
CREATE INDEX IDXGRAD ON LJUDI (GRAD);
SELECT * FROM LJUDI;
SELECT * FROM LJUDI WHERE GRAD='ZAGREB';
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

## Testiranje u JCL-u

Standardni batch test je `jcl/MINISQL.jcl`. Taj job:

- kreira tablicu `LJUDI` s `ID PRIMARY KEY`
- ubacuje dva retka
- kreira sekundarni indeks `IDXGRAD` na koloni `GRAD`
- prikazuje `.SCHEMA LJUDI`
- testira `SELECT * FROM LJUDI`
- testira `SELECT * FROM LJUDI WHERE GRAD='RIJEKA'`
- testira `UPDATE` i `DELETE`

Prvo resetiraj VSAM bazu ako zelis cist test:

```bash
zowe zos-jobs submit local-file jcl/ALLOCVS.jcl \
  --wait-for-output \
  --zosmf-profile hercules
```

Zatim submitaj SQL test:

```bash
zowe zos-jobs submit local-file jcl/MINISQL.jcl \
  --wait-for-output \
  --zosmf-profile hercules
```

Primjer uspjesnog rezultata:

```text
jobid:   JOB00805
retcode: CC 0000
jobname: MINISQL
status:  OUTPUT
```

`JOB00805` ce na tvom sistemu biti drugi broj. Taj broj koristi za dohvat
spool outputa.

Prvo izlistaj spool datoteke za job:

```bash
zowe zos-jobs list spool-files-by-jobid JOB00805 \
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

Rezultat programa je u `SYSPRINT`. Procitaj ga preko DDID-a iz liste:

```bash
zowe zos-jobs view spool-file-by-id JOB00805 103 \
  --zosmf-profile hercules
```

Ocekivani bitni dijelovi outputa:

```text
OK TABLE CREATED
LJUDI(ID PRIMARY KEY, IME, GRAD)
OK INDEX CREATED
INDEX IDXGRAD ON LJUDI(GRAD)
ID | IME | GRAD
2 | IVO | RIJEKA
OK 1 ROWS
```

## Testiranje u TSO-u

Interaktivni processor je load modul `MSQLTSO`. On koristi isti SQL engine kao
batch `MINISQL`, ali ulaz cita preko TSO `TGET`, a output ispisuje preko TSO
`TPUT`.

U foreground TSO sesiji prvo alociraj VSAM bazu:

```text
ALLOC FI(MINIKV) DA('IBMUSER.MINISQL.KV') OLD
```

Zatim pokreni processor:

```text
CALL 'IBMUSER.MINISQL.LOAD(MSQLTSO)'
```

Primjer interaktivnog testa:

```sql
.TABLES
.SCHEMA LJUDI
SELECT * FROM LJUDI;
SELECT * FROM LJUDI WHERE GRAD='RIJEKA';
INSERT INTO LJUDI VALUES (3, 'PERO', 'RIJEKA');
SELECT * FROM LJUDI WHERE GRAD='RIJEKA';
UPDATE LJUDI SET GRAD='SISAK' WHERE ID=3;
SELECT * FROM LJUDI WHERE GRAD='RIJEKA';
SELECT * FROM LJUDI WHERE GRAD='SISAK';
INSERT INTO LJUDI VALUES (3, 'DUP', 'OSIJEK');
UPDATE LJUDI SET ID=4 WHERE ID=3;
.QUIT
```

Ocekivano ponasanje:

- `.SCHEMA LJUDI` prikazuje `ID PRIMARY KEY` i `INDEX IDXGRAD ON LJUDI(GRAD)`
- `SELECT ... WHERE GRAD='RIJEKA'` koristi sekundarni indeks ako postoji
- dupli `ID=3` vraca `ERR DUPLICATE PRIMARY KEY`
- promjena primary key kolone vraca `ERR CANNOT UPDATE PRIMARY KEY`
- promjena ne-key kolone, npr. `GRAD`, radi i automatski obnavlja indeks

Na kraju mozes osloboditi DD:

```text
FREE FI(MINIKV)
```

Napomena: `MSQLTSO` nije batch SQL runner. Nakon prelaska na `TGET`, Zowe
batch submit preko `IKJEFT01` ne moze glumiti pravu interaktivnu terminalsku
sesiju. Za batch SQL koristi `MINISQL` i `jcl/MINISQL.jcl`.

## Submit SQL joba preko Zowe

Kratki primjer pokretanja postojeceg JCL-a:

```bash
zowe zos-jobs submit local-file jcl/MINISQL.jcl \
  --wait-for-output \
  --zosmf-profile hercules
```

Za dohvat outputa prvo izlistaj DD-ove:

```bash
zowe zos-jobs list spool-files-by-jobid JOBID --zosmf-profile hercules
```

Zatim procitaj `SYSPRINT` DDID, npr. `103`:

```bash
zowe zos-jobs view spool-file-by-id JOBID 103 --zosmf-profile hercules
```

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

## Ogranicenja

- Svi podaci su tekstualne vrijednosti. Nema SQL tipova kao `INTEGER`,
  `DATE`, `DECIMAL`, `CHAR(n)` ili `VARCHAR(n)`.
- Vrijednosti se ciste i spremaju uppercase; navodnici se koriste samo za
  parsiranje vrijednosti s razmacima ili jasniji SQL izgled.
- Nema `NULL`, default vrijednosti, constrainta osim jednog primary keya,
  foreign keyeva ni check constrainta.
- Maksimalno 32 tablice.
- Maksimalno 8 kolona po tablici.
- Maksimalno 32 retka po tablici.
- Maksimalno 4 sekundarna indeksa po tablici.
- `PRIMARY KEY` podrzava samo jednu kolonu.
- `CREATE INDEX` podrzava samo jednu kolonu po indeksu.
- Imena tablica, kolona i indeksa mogu imati najvise 16 znakova.
- Vrijednost jedne kolone moze imati najvise 32 znaka.
- Jedan spremljeni row payload mora stati u 192 bajta, ukljucujuci zareze
  izmedju vrijednosti.
- VSAM record layout je fiksan: key 64 bajta, data 192 bajta, ukupno 256
  bajtova. Ako se taj layout promijeni, `IBMUSER.MINISQL.KV` treba ponovno
  kreirati s `jcl/ALLOCVS.jcl`.
- Podrzan je samo `SELECT * FROM table` i opcionalni
  `WHERE kolona=vrijednost`.
- `WHERE` podrzava samo jednakost, bez `AND`, `OR`, `LIKE`, `<`, `>`,
  `BETWEEN` ili izraza.
- Sekundarni indeks se koristi samo za `SELECT * FROM table WHERE col=value`
  kada postoji indeks na `col`; nema opceg SQL optimizatora.
- `UPDATE` podrzava jedan `SET col=value` i opcionalni `WHERE`.
- `DELETE` podrzava opcionalni `WHERE`; bez `WHERE` brise sve retke tablice.
- Nema joinova, order by, group by, agregacija, pogleda, stored procedura,
  transakcija, rollbacka ni recovery loga.
- `MSQLTSO` je interaktivni foreground TSO program. Za batch SQL koristi se
  `MINISQL`, ne `MSQLTSO`.
