# pg_lion explained (video)

`pg_lion_explained.mp4` (1:10, 1080p, 2.6 MB) is a short, narrated pitch for Lion, aimed at the
people who decide which index a table gets: developers, DBAs and their leads.  It shows the problem
(counting matches row by row), how Lion counts instead, the headline numbers, how little it takes to
adopt, and where B-tree and GIN still fit.  Every number comes from the README's "Latest
benchmarks" (PostgreSQL 18.6, commit `118f623`), and the end card carries the caveats: a
prototype, fastest on vacuumed tables, slower inserts than a B-tree.

| Time | Beat | On screen |
|---|---|---|
| 0:00 | How many rows match? | every dashboard, filter and report asks it |
| 0:08 | Counting is slow | 5M rows, half matching: B-tree 157 ms, GIN 665 ms |
| 0:16 | Meet Lion | a roaring-bitmap index for Postgres |
| 0:21 | How it counts | 64-page chunks, compressed containers, each with its count; add them up |
| 0:31 | The numbers | 1.7 ms, 90× B-tree; 9× two filters, 24× GROUP BY, 168× array tags vs GIN |
| 0:47 | Just an index | `CREATE INDEX ... USING lion`, `VACUUM`, and the plan says `LionCount` |
| 0:55 | Where it fits | B-tree for lookups and ORDER BY, GIN for text, Lion for counts |
| 1:02 | Try it | the repository, and the fine print |

The MP4 carries chapters and an English subtitle track, and the captions are also burned into the
picture for muted playback.

## Rebuilding it

The video is generated, not edited: the narration is `script.json`, the pictures are
`scenes.html`, and two scripts turn them into the MP4.

    python3 -m venv venv && . venv/bin/activate
    pip install kokoro-onnx soundfile numpy imageio-ffmpeg playwright
    # the Kokoro TTS model (Apache-2.0) and its voices
    curl -LO https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0/kokoro-v1.0.onnx
    curl -LO https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0/voices-v1.0.bin
    python build_audio.py kokoro-v1.0.onnx voices-v1.0.bin build    # narration.wav, timeline.js, captions.srt
    python render.py build stills 14 34 50                           # check a few frames
    python render.py build video --jobs 4                            # build/pg_lion_explained.mp4

`build_audio.py` speaks each sentence of `script.json` separately and lays them out on a timeline
(`timeline.js`), so every animation in `scenes.html` is keyed to the moment a phrase is spoken:
change the words and the pictures follow.  `scenes.html` is a deterministic page, where
`renderAt(t)` poses the stage for time `t`; `render.py` screenshots it at 30 frames a second in
headless Chromium and encodes the frames with ffmpeg.  Open `build/scenes.html?play` in a browser
to watch the animation live (without sound), or `?t=190` to see a single instant.

Words the voice gets wrong are spelled for it in `say` ("Postgress", "B tree", "gin", "P G Lion"),
and the caption keeps the written form (`cap`, or a default mapping in `build_audio.py`).  Set
`CHROMIUM=/path/to/chrome` when Playwright's own browser download is not the one installed.

Fonts: Inter and JetBrains Mono, both under the SIL Open Font License (`fonts/LICENSE-*.txt`).
