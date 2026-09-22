"""Run against a host build: python3 tests/select_alignment.py /tmp/minisql."""
import subprocess
import sys

setup = """
CREATE TABLE PEOPLE (ID INT PRIMARY KEY, NAME VARCHAR(32), CITY VARCHAR(32));
INSERT INTO PEOPLE VALUES (1, 'ANA', 'ZAGREB');
INSERT INTO PEOPLE VALUES (222, 'ALEKSANDRA', 'X');
INSERT INTO PEOPLE VALUES (3, '', 'ZAGREB');
CREATE TABLE ORDERS (PID INT, ITEM VARCHAR(32));
INSERT INTO ORDERS VALUES (1, 'BOOK');
INSERT INTO ORDERS VALUES (222, ABCDEFGHIJKLMNOPQRSTUVWXYZ12);
"""
queries = [
    ('SELECT * FROM PEOPLE;', 3),
    ('SELECT NAME,ID,CITY FROM PEOPLE ORDER BY ID DESC;', 3),
    ('SELECT NAME,ID FROM PEOPLE ORDER BY ID LIMIT 1;', 1),
    ('SELECT * FROM PEOPLE WHERE ID=999;', 0),
    ('SELECT * FROM PEOPLE LIMIT 0;', 0),
    ('SELECT * FROM PEOPLE GROUP BY CITY;', 2),
    ('SELECT CITY,COUNT(*) FROM PEOPLE GROUP BY CITY ORDER BY COUNT DESC;', 2),
    ('SELECT * FROM PEOPLE JOIN ORDERS ON PEOPLE.ID=ORDERS.PID;', 2),
    ('SELECT P.NAME,O.ITEM,P.ID FROM PEOPLE P JOIN ORDERS O ON P.ID=O.PID;', 2),
]
for indexed in (False, True):
    for query, count in queries:
        sql = setup
        if indexed:
            sql += 'CREATE INDEX IXID ON PEOPLE (ID);\n'
            sql += 'CREATE INDEX IXPID ON ORDERS (PID);\n'
        sql += query + '\n.QUIT\n'
        result = subprocess.run([sys.argv[1]], input=sql, text=True,
                                capture_output=True, check=True)
        assert 'ERR ' not in result.stdout, result.stdout
        output = result.stdout.replace('SQL> ', '').splitlines()
        lines = [line for line in output if ' | ' in line]
        header_at = output.index(lines[0])
        rule = output[header_at + 1]
        assert set(rule) <= {'-', '|'} and '-' in rule, (query, rule)
        assert [i for i, ch in enumerate(rule) if ch == '|'] == [
            i for i, ch in enumerate(lines[0]) if ch == '|'], (query, rule)
        assert len(rule) == max(map(len, lines)), (query, rule, lines)
        assert len(lines) == count + 1, (query, result.stdout)
        positions = [[i for i, ch in enumerate(line) if ch == '|'] for line in lines]
        assert all(p == positions[0] for p in positions), (query, lines)
        assert f'OK {count} ' in result.stdout, result.stdout
        if 'JOIN' in query:
            assert 'ABCDEFGHIJKLMNOPQRSTUVWXYZ12' in result.stdout, result.stdout
        if query == 'SELECT * FROM PEOPLE;':
            assert lines == ['ID  | NAME       | CITY',
                             '1   | ANA        | ZAGREB',
                             '222 | ALEKSANDRA | X',
                             '3   |            | ZAGREB'], lines
for query, header, count in [
    ('SELECT COUNT(*) FROM PEOPLE;', 'COUNT', 1),
    ('SELECT COUNT(*) FROM PEOPLE GROUP BY CITY;', 'COUNT', 2),
    ('SELECT CITY FROM PEOPLE GROUP BY CITY;', 'CITY', 2),
    ('SELECT NAME FROM PEOPLE WHERE ID=999;', 'NAME', 0),
]:
    result = subprocess.run([sys.argv[1]], input=setup + query + '\n.QUIT\n',
                            text=True, capture_output=True, check=True)
    output = result.stdout.replace('SQL> ', '').splitlines()
    start = output.index(header)
    rule = output[start + 1]
    assert set(rule) == {'-'}, (query, output)
    assert len(rule) == max(map(len, [header] + output[start + 2:start + 2 + count]))
aggregate_setup = """
CREATE TABLE METRICS (ID INT PRIMARY KEY, CITY VARCHAR(8), VALUE INT);
INSERT INTO METRICS VALUES (1, 'A', 10);
INSERT INTO METRICS VALUES (2, 'A', 20);
INSERT INTO METRICS VALUES (3, 'B', 30);
"""
for query, header, value in [
    ('SELECT SUM(VALUE) FROM METRICS;', 'SUM(VALUE)', '60'),
    ('SELECT AVG(VALUE) FROM METRICS;', 'AVG(VALUE)', '20.00'),
    ('SELECT MIN(VALUE) FROM METRICS;', 'MIN(VALUE)', '10'),
    ('SELECT MAX(VALUE) FROM METRICS;', 'MAX(VALUE)', '30'),
    ('SELECT CITY,SUM(VALUE) FROM METRICS GROUP BY CITY;', 'SUM(VALUE)', '30'),
]:
    result = subprocess.run([sys.argv[1]], input=aggregate_setup + query + '\n.QUIT\n',
                            text=True, capture_output=True, check=True)
    assert 'ERR ' not in result.stdout, result.stdout
    assert header in result.stdout, result.stdout
    if 'GROUP BY' in query:
        assert f'| {value}' in result.stdout, result.stdout
    else:
        assert f'\n{value}\n' in result.stdout, result.stdout
print('27 SELECT alignment and aggregate checks passed')
