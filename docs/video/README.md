# pg_lion explained (video)

`pg_lion_explained.mp4` (1:07, 1080p, 5.8 MB) is a narrated, animated explainer of Lion for the
developers and DBAs who choose a table's indexes.  One cast of objects carries it: 800 dots are a
5-million-row table, the matching half is counted by a B-tree and then by GIN, flies together into
a pixel lion, and breaks apart into the chunk bitmaps Lion adds up.  Every number comes from the
README's "Latest benchmarks" (PostgreSQL 18.6, commit `118f623`), and the end card carries the
caveats: a prototype, fastest on vacuumed tables, slower inserts than a B-tree.

| Time | Beat | What moves |
|---|---|---|
| 0:00 | How many rows match? | dashboard widgets collapse into the question |
| 0:06 | Counting row by row | the rows fall into a table; a B-tree walks the matches (157 ms), GIN sweeps the pages (665 ms) |
| 0:18 | Meet Lion | the matching rows fly together into a pixel lion |
| 0:22 | Chunks that know their count | the lion packs into chunk bitmaps with counts; a conveyor adds them to 2,500,000; a changed page is checked row by row |
| 0:39 | The result | a race: Lion 1.7 ms, B-tree 157 ms, GIN 665 ms; 90× |
| 0:46 | More than one filter | combined filters (9×), GROUP BY (24×), joins, partitioned tables |
| 0:52 | Where it fits | keep B-tree and GIN; `CREATE INDEX ON events USING lion (country)` |
| 0:58 | Try it | the lion again, the repository and the fine print |

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
    python render.py build stills 11 20 30                           # check a few frames
    python render.py build video --jobs 4                            # build/pg_lion_explained.mp4

`build_audio.py` speaks each sentence of `script.json` separately and lays them out on a timeline
(`timeline.js`), so every animation in `scenes.html` is keyed to the moment a phrase is spoken:
change the words and the pictures follow.  `scenes.html` is a deterministic page, where
`renderAt(t)` poses the stage for time `t`; `render.py` screenshots it at 30 frames a second in
headless Chromium and encodes the frames with ffmpeg.  Open `build/scenes.html?play` in a browser
to watch the animation live (without sound), or `?t=20` to see a single instant.

Words the voice gets wrong are spelled for it in `say` ("Postgress", "B tree", "gin", "P G Lion"),
and the caption keeps the written form (`cap`, or a default mapping in `build_audio.py`).  Set
`CHROMIUM=/path/to/chrome` when Playwright's own browser download is not the one installed.

Fonts: Bricolage Grotesque, Inter and JetBrains Mono, all under the SIL Open Font License
(`fonts/LICENSE-*.txt`).
