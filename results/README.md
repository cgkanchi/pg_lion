Raw benchmark logs. The results directory was deleted twice during agent development, so only the
runs still present in the session scratchpad could be restored: 01 (dataset generation), 02 (simulated
roaring sizes under four TID encodings), 04 (write path), 08 (final 20M-row run with the real index and
the per-container visibility-map read). The 4 GB / 10 GB shared_buffers baseline runs, the first real-index
run (06/06b) and the 14-bit container experiment (07) survive only as the tables in ../README.md.
