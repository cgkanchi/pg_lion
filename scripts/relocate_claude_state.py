#!/usr/bin/env python3
"""Relocate Claude Code per-project state after a project directory is renamed.

Claude Code keys per-project state by the absolute project path:
  ~/.claude/projects/<path with / replaced by ->/   transcripts (*.jsonl), tool results, subagent
                                                    transcripts, memory/*.md
  ~/.claude.json                                    projects[<path>] settings (trust, allowed tools)
  ~/.claude/history.jsonl                           prompt history entries with "project": <path>

Renaming the directory would leave all of that behind.  This script copies the state under the new
key and rewrites the old absolute path to the new one inside it.  Only the slash form of the path is
rewritten, and only when not followed by a word character or '-', so sibling directories such as
<old>-partial and the dashed scratchpad key /tmp/claude-1000/-home-...-<old>/ are left alone.

Run it AFTER the last Claude session under the old path has ended (transcripts are appended live) and
AFTER renaming the directory.  It is idempotent: re-running refreshes the copy.

    python3 scripts/relocate_claude_state.py /home/me/code/pg_roaring_index /home/me/code/pg_lion
    python3 scripts/relocate_claude_state.py OLD NEW --map /home/me/code/pg_roaring_index-partial=/home/me/code/pg_lion-partial

The old state is never deleted; remove it by hand once the new path has been used successfully.
"""
import argparse, json, os, re, shutil, sys, time

def key_of(path):
    # Claude Code derives the per-project directory name by replacing every
    # non-alphanumeric character of the absolute path with '-' (so '/' and '_' both map to '-').
    return re.sub(r'[^A-Za-z0-9]', '-', path)

def build_rewriter(mappings):
    # longest old path first so that OLD-partial maps before OLD when both are given
    pairs = sorted(mappings, key=lambda p: -len(p[0]))
    pats = [(re.compile(re.escape(o) + r'(?![\w-])'), n) for o, n in pairs]
    def rewrite(text):
        count = 0
        for pat, new in pats:
            text, c = pat.subn(new, text)
            count += c
        return text, count
    return rewrite

def copy_tree_rewriting(src, dst, rewrite, exts):
    files = 0; hits = 0
    for root, dirs, names in os.walk(src):
        rel = os.path.relpath(root, src)
        outdir = os.path.join(dst, rel) if rel != '.' else dst
        os.makedirs(outdir, exist_ok=True)
        for name in names:
            s = os.path.join(root, name); d = os.path.join(outdir, name)
            if any(name.endswith(e) for e in exts):
                with open(s, 'r', encoding='utf-8', errors='surrogateescape') as f: text = f.read()
                text, c = rewrite(text); hits += c
                with open(d, 'w', encoding='utf-8', errors='surrogateescape') as f: f.write(text)
                shutil.copystat(s, d)
            else:
                shutil.copy2(s, d)
            files += 1
    return files, hits

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('old'); ap.add_argument('new')
    ap.add_argument('--map', action='append', default=[], help='extra OLD=NEW path rewrite (e.g. a worktree)')
    ap.add_argument('--claude-dir', default=os.path.expanduser('~/.claude'))
    ap.add_argument('--claude-json', default=os.path.expanduser('~/.claude.json'))
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--state-only', action='store_true', help='copy the project state only; leave ~/.claude.json and history.jsonl untouched (use while sessions under the old path are still active)')
    a = ap.parse_args()
    old = os.path.abspath(a.old.rstrip('/')); new = os.path.abspath(a.new.rstrip('/'))
    mappings = [(old, new)] + [tuple(m.split('=', 1)) for m in a.map]
    rewrite = build_rewriter(mappings)

    src = os.path.join(a.claude_dir, 'projects', key_of(old))
    dst = os.path.join(a.claude_dir, 'projects', key_of(new))
    if not os.path.isdir(src):
        sys.exit(f"no project state at {src}")
    print(f"project state: {src}\n           -> {dst}")
    if not a.dry_run:
        files, hits = copy_tree_rewriting(src, dst, rewrite, exts=('.jsonl', '.md', '.txt', '.json'))
        print(f"  copied {files} files, rewrote {hits} path occurrences")

    # ~/.claude.json: duplicate the per-project settings entry
    if a.state_only:
        print('state-only: ~/.claude.json and history.jsonl left untouched'); print('done'); return
    if os.path.exists(a.claude_json):
        with open(a.claude_json) as f: cfg = json.load(f)
        projects = cfg.setdefault('projects', {})
        if old in projects:
            if new in projects:
                print(f"~/.claude.json already has an entry for {new}; left as is")
            else:
                projects[new] = json.loads(json.dumps(projects[old]))
                print(f"~/.claude.json: projects[{new}] created from projects[{old}]")
                if not a.dry_run:
                    shutil.copy2(a.claude_json, a.claude_json + f'.bak-{int(time.time())}')
                    with open(a.claude_json, 'w') as f: json.dump(cfg, f, indent=2)
        else:
            print(f"~/.claude.json has no entry for {old}")

    # history.jsonl: retarget entries' project field
    hist = os.path.join(a.claude_dir, 'history.jsonl')
    if os.path.exists(hist):
        out = []; changed = 0
        with open(hist, encoding='utf-8', errors='surrogateescape') as f:
            for line in f:
                try:
                    e = json.loads(line)
                except Exception:
                    out.append(line); continue
                if e.get('project') == old:
                    e['project'] = new; changed += 1
                    out.append(json.dumps(e, ensure_ascii=False) + '\n')
                else:
                    out.append(line)
        print(f"history.jsonl: {changed} entries retargeted")
        if changed and not a.dry_run:
            shutil.copy2(hist, hist + f'.bak-{int(time.time())}')
            with open(hist, 'w', encoding='utf-8', errors='surrogateescape') as f: f.writelines(out)
    print("done" + (" (dry run)" if a.dry_run else ""))

if __name__ == '__main__':
    main()
