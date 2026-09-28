# pg_lion explained (video)

`pg_lion_explained.mp4` is a 7-minute narrated explainer of Lion for people who *use* PostgreSQL:
DBAs and application developers, not people who build database internals.  It covers what Lion is,
how it differs from the B-tree and GIN indexes they already use, where it wins, where it doesn't,
and how to try it.  Every number in it comes from the README's "Latest benchmarks" (PostgreSQL
18.6, commit `118f623`).

| # | Chapter | What it shows |
|---|---|---|
| 0 | Introduction | the one question Lion answers: how many rows match? |
| 1 | The question dashboards ask | `count(*)` + `WHERE` + `GROUP BY`; 5M rows, half of them matching |
| 2 | B-tree: a sorted list | a quick descent, then one step per matching row: 157 ms |
| 3 | GIN: an inverted index | key → rows; arrays, JSONB, full text; counting reads the table: 665 ms |
| 4 | Lion: sets of rows, in chunks | 64-page chunks, one container each: sorted list, bitmap or runs |
| 5 | Counting without walking rows | adding container totals; the visibility map and VACUUM; 1.7 ms |
| 6 | Filters and groups | AND of two sets, GROUP BY, and the other measured speedups |
| 7 | Where Lion doesn't win | rare keys, fetching rows, ORDER BY, phrase/prefix, dirty pages, writes |
| 8 | Using it | `CREATE INDEX ... USING lion`, `EXPLAIN` showing `LionCount`, `summaries = auto` |
| 9 | Which index, when | the cheat sheet |

The MP4 carries chapters and an English subtitle track, and the captions are also burned into the
picture.

## Rebuilding it

The video is generated, not edited: the narration is `script.json`, the pictures are
`scenes.html`, and two scripts turn them into the MP4.

    python3 -m venv venv && . venv/bin/activate
    pip install kokoro-onnx soundfile numpy imageio-ffmpeg playwright
    # the Kokoro TTS model (Apache-2.0) and its voices
    curl -LO https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0/kokoro-v1.0.onnx
    curl -LO https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0/voices-v1.0.bin
    python build_audio.py kokoro-v1.0.onnx voices-v1.0.bin build    # narration.wav, timeline.js, captions.srt
    python render.py build stills 60 190 232                         # check a few frames
    python render.py build video --jobs 4                            # build/pg_lion_explained.mp4

`build_audio.py` speaks each sentence of `script.json` separately and lays them out on a timeline
(`timeline.js`), so every animation in `scenes.html` is keyed to the moment a phrase is spoken:
change the words and the pictures follow.  `scenes.html` is a deterministic page, where
`renderAt(t)` poses the stage for time `t`; `render.py` screenshots it at 30 frames a second in
headless Chromium and encodes the frames with ffmpeg.  Open `build/scenes.html?play` in a browser
to watch the animation live (without sound), or `?t=190` to see a single instant.

Words the voice gets wrong are spelled for it in `say` ("B tree", "gin", "P G Lion", "JSON-B"),
and the caption keeps the written form (`cap`, or a default mapping in `build_audio.py`).  Set
`CHROMIUM=/path/to/chrome` when Playwright's own browser download is not the one installed.

Fonts: Inter and JetBrains Mono, both under the SIL Open Font License (`fonts/LICENSE-*.txt`).
