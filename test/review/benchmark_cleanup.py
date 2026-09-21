#!/usr/bin/env python3
"""Reproduce cleanup after startup failure using fake executables, no database."""
from pathlib import Path
import subprocess
import tempfile

helper = Path(__file__).resolve().parents[2]/'bench/lib.sh'
with tempfile.TemporaryDirectory(prefix='rbi-cleanup-review-') as tmp:
    root = Path(tmp)
    (root/'bin').mkdir()
    (root/'data').mkdir()
    ctl = root/'bin/pg_ctl'
    ctl.write_text('#!/bin/sh\nexit 1\n')
    ctl.chmod(0o700)
    psql = root/'bin/psql'
    psql.write_text('#!/bin/sh\nprintf "%s\\n" "$@" >> "'+str(root/'calls')+'"\nexit 0\n')
    psql.chmod(0o700)
    result = subprocess.run(['bash','-c',
        'source "$1"; BENCH_SOCKDIR="$2/socket"; bench_start_cluster "$2" "$2/data"',
        'review',str(helper),str(root)], capture_output=True,text=True)
    calls = (root/'calls').read_text() if (root/'calls').exists() else ''
    print('Startup exit:',result.returncode)
    print('SQL attempted after failed startup:',calls.strip() or '(none)')
    assert result.returncode != 0
    assert 'update pg_index' in calls, 'Finding no longer reproduces; update this review reproducer'
