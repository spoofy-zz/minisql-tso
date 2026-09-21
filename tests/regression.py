"""Compare full SQL output with snapshots captured before modularization."""
from pathlib import Path
import difflib
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
batch = (root / 'jcl/MINISQL.jcl').read_text().split('//SYSIN    DD *\n', 1)[1]
batch = batch.split('\n/*', 1)[0] + '\n'
cases = {
    'batch': batch,
    'joins': (root / 'tests/joins.sql').read_text(),
    'transactions': (root / 'tests/transactions.sql').read_text(),
}
for name, sql in cases.items():
    result = subprocess.run([sys.argv[1]], input=sql, text=True,
                            capture_output=True, check=True)
    expected = (root / f'tests/{name}.expected').read_text()
    if result.stdout != expected:
        sys.stderr.writelines(difflib.unified_diff(
            expected.splitlines(True), result.stdout.splitlines(True),
            fromfile=f'{name}.expected', tofile=f'{name}.actual'))
        raise SystemExit(1)
    assert not result.stderr, result.stderr
print('Batch SQL, join and transaction regression snapshots passed')
