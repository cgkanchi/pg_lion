#!/usr/bin/env python3
"""Audit launcher failure paths using fake executables; never opens a database.

Exit nonzero if cleanup can write after the original-state query failed.
"""
import json
from pathlib import Path
import subprocess
import tempfile

helper = Path(__file__).resolve().parents[2] / 'bench/lib.sh'
findings = []
for scenario in ['startup_failure', 'identity_failure', 'snapshot_failure', 'success']:
    with tempfile.TemporaryDirectory(prefix='lion-cleanup-review-') as tmp:
        root = Path(tmp)
        (root / 'bin').mkdir()
        (root / 'data').mkdir()
        ctl = root / 'bin/pg_ctl'
        ctl.write_text('#!/usr/bin/env python3\n'
                       'import sys\n'
                       f'scenario = {scenario!r}\n'
                       'sys.exit(1 if "status" in sys.argv or '
                       '("start" in sys.argv and scenario == "startup_failure") else 0)\n')
        ctl.chmod(0o700)
        psql = root / 'bin/psql'
        psql.write_text('#!/usr/bin/env python3\n'
                        'import sys\nfrom pathlib import Path\n'
                        f'root = Path({str(root)!r})\nscenario = {scenario!r}\n'
                        'sql = sys.argv[sys.argv.index("-c") + 1]\n'
                        'with (root / "calls").open("a") as log: log.write(sql + "\\n")\n'
                        'if sql == "show data_directory":\n'
                        '    print(root / ("wrong" if scenario == "identity_failure" else "data"))\n'
                        'elif sql.startswith("select coalesce"):\n'
                        '    if scenario == "snapshot_failure": sys.exit(1)\n'
                        '    print("\'fact_preexisting_invalid\'")\n')
        psql.chmod(0o700)
        result = subprocess.run([
            'bash', '-c',
            'set -euo pipefail; source "$1"; BENCH_SOCKDIR="$2/socket"; '
            'bench_start_cluster "$2" "$2/data"; echo BENCH_BODY_REACHED',
            'review', str(helper), str(root)], capture_output=True, text=True)
        calls = (root / 'calls').read_text().splitlines() if (root / 'calls').exists() else []
        writes = [sql for sql in calls if sql.startswith('update pg_index')]
        unsafe = scenario == 'snapshot_failure' and bool(writes)
        findings.append(unsafe)
        print(json.dumps(dict(scenario=scenario, exit=result.returncode,
                              body_reached='BENCH_BODY_REACHED' in result.stdout,
                              cleanup_writes=writes, unsafe=unsafe)), flush=True)
        if scenario in ['startup_failure', 'identity_failure']:
            assert result.returncode != 0 and not writes
        if scenario == 'success':
            assert result.returncode == 0 and len(writes) == 1
            assert "not in ('fact_preexisting_invalid')" in writes[0]

raise SystemExit(1 if any(findings) else 0)
