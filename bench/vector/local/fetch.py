#!/usr/bin/env python3
# Download the first N shards (sorted by name) of the public Hugging Face
# dataset Qdrant/dbpedia-entities-openai3-text-embedding-3-large-1536-1M into
# DIR as train-XXXXX-of-00026.parquet, the names knn_dbpedia.py globs.  A shard
# already there that pyarrow can read is kept, so a broken download resumes.
#   fetch.py DIR N
import json, os, re, subprocess, sys, urllib.request
import pyarrow.parquet as pq

DATASET = 'Qdrant/dbpedia-entities-openai3-text-embedding-3-large-1536-1M'


def readable(path):
    try:
        pq.ParquetFile(path).metadata
        return True
    except Exception:
        return False


def main(out, n):
    os.makedirs(out, exist_ok=True)
    with urllib.request.urlopen(f'https://huggingface.co/api/datasets/{DATASET}') as r:
        names = sorted(s['rfilename'] for s in json.load(r)['siblings']
                       if s['rfilename'].endswith('.parquet') and 'train-' in s['rfilename'])
    if len(names) < n:
        raise SystemExit(f'the dataset lists {len(names)} parquet shards, fewer than {n}: {names}')
    for name in names[:n]:
        g = re.search(r'train-(\d+)-of-(\d+)', name)
        dest = os.path.join(out, f'train-{g.group(1)}-of-{g.group(2)}.parquet')
        if os.path.exists(dest) and readable(dest):
            print(f'{dest}: there', flush=True)
            continue
        part = dest + '.part'
        url = f'https://huggingface.co/datasets/{DATASET}/resolve/main/{name}'
        print(f'{url} -> {dest}', flush=True)
        subprocess.run(['curl', '-fL', '--retry', '5', '-C', '-', '-o', part, url], check=True)
        if not readable(part):
            os.remove(part)
            raise SystemExit(f'{name}: the download is not a readable parquet file; run again')
        os.replace(part, dest)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: fetch.py DIR N')
    main(sys.argv[1], int(sys.argv[2]))
