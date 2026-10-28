#!/usr/bin/env python3
"""Check that every extension's SQL scripts and versions hang together.

Usage: test/upgrade/lint.py [BASE]

For each extension the Makefile builds (DESIGN.md section 23, "Extension
versions"):

  - default_version is X.Y.Z or X.Y.Z-devN, and META.json's version is that
    without the -devN, as are the extension's version under "provides", whose
    "file" is the install script;
  - the install script of default_version is the only install script;
  - every upgrade script is named <ext>--<from>--<to>.sql, and a chain of them
    leads from every version that has one to default_version.

With BASE, a git revision (the pull request's base branch), also:

  - an install script BASE has under default_version is unchanged: a change to
    the SQL needs a new version and an upgrade script to it;
  - BASE's default_version has a chain of upgrade scripts to today's;
  - every upgrade script BASE has is still here, unchanged.
"""

import json
import os
import re
import subprocess
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
VERSION_RE = re.compile(r'^(\d+\.\d+\.\d+)(-dev\d+)?$')
errors = []


def fail(msg):
    errors.append(msg)


def git_show(rev, path):
    r = subprocess.run(['git', 'show', '%s:%s' % (rev, path)], cwd=ROOT,
                       capture_output=True)
    return r.stdout if r.returncode == 0 else None


def control_version(text):
    m = re.search(r"^default_version = '([^']*)'$", text, re.M)
    return m.group(1) if m else None


def scripts(ext, names):
    """Install scripts {version: name} and upgrade scripts {(from, to): name}."""
    install, upgrade = {}, {}
    prefix = ext + '--'
    for name in names:
        if not (name.startswith(prefix) and name.endswith('.sql')):
            continue
        parts = name[len(prefix):-len('.sql')].split('--')
        if len(parts) == 1:
            install[parts[0]] = name
        elif len(parts) == 2:
            upgrade[tuple(parts)] = name
        else:
            fail('%s: not a script name PostgreSQL understands' % name)
    return install, upgrade


def reaches(upgrade, start, goal):
    seen, todo = {start}, [start]
    while todo:
        v = todo.pop()
        if v == goal:
            return True
        for (a, b) in upgrade:
            if a == v and b not in seen:
                seen.add(b)
                todo.append(b)
    return False


def main():
    base = sys.argv[1] if len(sys.argv) > 1 else None
    with open(os.path.join(ROOT, 'Makefile')) as f:
        exts = re.search(r'^EXTENSION = (.*)$', f.read(), re.M).group(1).split()
    with open(os.path.join(ROOT, 'META.json')) as f:
        meta = json.load(f)
    files = os.listdir(ROOT)
    if base:
        base_files = subprocess.run(['git', 'ls-tree', '--name-only', base], cwd=ROOT,
                                    capture_output=True, text=True, check=True).stdout.split()

    for ext in exts:
        with open(os.path.join(ROOT, ext + '.control')) as f:
            cur = control_version(f.read())
        m = VERSION_RE.match(cur or '')
        if not m:
            fail('%s.control: default_version %r is not X.Y.Z or X.Y.Z-devN' % (ext, cur))
            continue
        release = m.group(1)
        if meta.get('version') != release:
            fail('META.json: version %r, but %s.control says %s' % (meta.get('version'), ext, cur))
        prov = meta.get('provides', {}).get(ext)
        if prov is None:
            fail('META.json: %s is not under "provides"' % ext)
        else:
            if prov.get('version') != release:
                fail('META.json: provides.%s.version %r, expected %r' % (ext, prov.get('version'), release))
            if prov.get('file') != '%s--%s.sql' % (ext, cur):
                fail('META.json: provides.%s.file %r, expected %r' % (ext, prov.get('file'), '%s--%s.sql' % (ext, cur)))

        install, upgrade = scripts(ext, files)
        if set(install) != {cur}:
            fail('%s: the install scripts are %s; there must be exactly one, %s--%s.sql (older versions are reached by upgrade scripts)'
                 % (ext, sorted(install.values()) or 'none', ext, cur))
        for (a, b) in upgrade:
            if not reaches(upgrade, a, cur):
                fail('%s: no upgrade path from %s to %s' % (ext, a, cur))

        if not base:
            continue
        bctl = git_show(base, ext + '.control')
        if bctl is None:
            continue    # a new extension
        bver = control_version(bctl.decode())
        binstall, bupgrade = scripts(ext, base_files)
        if bver == cur:
            old = git_show(base, '%s--%s.sql' % (ext, cur))
            with open(os.path.join(ROOT, '%s--%s.sql' % (ext, cur)), 'rb') as f:
                new = f.read()
            if old is not None and old != new:
                fail('%s--%s.sql changed, but %s.control still says %s: give the change a new version '
                     '(default_version, META.json) and an upgrade script %s--%s--<new>.sql from this one '
                     '(DESIGN.md section 23, "Extension versions")' % (ext, cur, ext, cur, ext, cur))
        elif not reaches(upgrade, bver, cur):
            fail('%s: %s has version %s and this tree %s, but no upgrade scripts lead from one to the other'
                 % (ext, base, bver, cur))
        for pair, name in bupgrade.items():
            if pair not in upgrade:
                fail('%s: %s has it, and it is gone: an upgrade script is never removed' % (name, base))
                continue
            with open(os.path.join(ROOT, name), 'rb') as f:
                if f.read() != git_show(base, name):
                    fail('%s changed: an upgrade script, once on %s, is never changed; '
                         'add a new version instead' % (name, base))

    for e in errors:
        print('lint: ' + e, file=sys.stderr)
    return 1 if errors else 0


if __name__ == '__main__':
    sys.exit(main())
