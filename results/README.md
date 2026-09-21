Raw benchmark logs. This directory was deleted several times during agent development (it was
untracked and matched an ignore pattern, so `git clean`/stash-style operations removed it); only the
runs still present in the session scratchpad could be restored: 01 (dataset generation), 02 (simulated
roaring sizes under four TID encodings), 04 (write path), 08 (20M-row run with the real index and the
per-container visibility-map read), 09 (final run with the current format). Earlier runs survive as
the tables in ../README.md.
