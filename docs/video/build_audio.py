#!/usr/bin/env python3
"""Narrate script.json with Kokoro TTS and lay out the video's timeline.

Writes, into the build directory:
  narration.wav   the whole voice track
  timeline.js     window.TIMELINE: every scene's and cue's start and end, in seconds,
                  which scenes.html animates against
  captions.srt    one caption per spoken sentence
  chapters.txt    ffmetadata chapters, one per scene

Usage: build_audio.py MODEL.onnx VOICES.bin BUILD_DIR
"""
import hashlib
import json
import os
import re
import sys

import numpy as np
import soundfile as sf
from kokoro_onnx import Kokoro

HERE = os.path.dirname(os.path.abspath(__file__))
SR = 24000
SENTENCE_GAP = 0.28


def split_sentences(text):
    # A sentence ends at . ? or ! followed by a space; "1.7" and "2.5" stay whole.
    return [s for s in re.split(r"(?<=[.?!])\s+", text.strip()) if s]


def default_caption(say):
    cap = re.sub(r"\bgin\b", "GIN", say, flags=re.IGNORECASE)
    cap = cap.replace("P G Lion", "pg_lion").replace("count star", "count(*)").replace("B tree", "B-tree")
    return cap


def srt_time(t):
    ms = int(round(t * 1000))
    return "%02d:%02d:%02d,%03d" % (ms // 3600000, ms // 60000 % 60, ms // 1000 % 60, ms % 1000)


def main():
    model, voices, out = sys.argv[1:4]
    os.makedirs(os.path.join(out, "tts"), exist_ok=True)
    with open(os.path.join(HERE, "script.json")) as f:
        script = json.load(f)
    kokoro = Kokoro(model, voices)

    def speak(text):
        key = hashlib.sha1(("%s|%s|%s" % (script["voice"], script["speed"], text)).encode()).hexdigest()[:16]
        path = os.path.join(out, "tts", key + ".wav")
        if not os.path.exists(path):
            samples, sr = kokoro.create(text, voice=script["voice"], speed=script["speed"], lang="en-us")
            assert sr == SR
            sf.write(path, samples, sr)
        samples, _ = sf.read(path, dtype="float32")
        return samples

    t = script["lead_in"]
    clips = []                      # (start seconds, samples)
    scenes = []
    captions = []
    for si, scene in enumerate(script["scenes"]):
        sc = {"id": scene["id"], "title": scene["title"], "start": 0.0 if si == 0 else t, "cues": []}
        if si > 0:
            t += 0.9                # the new scene settles before it speaks
        for cue in scene["cues"]:
            says = split_sentences(cue["say"])
            caps = split_sentences(cue.get("cap") or default_caption(cue["say"]))
            if len(says) != len(caps):
                sys.exit("cue %s: %d spoken sentences but %d captions" % (cue["id"], len(says), len(caps)))
            c = {"id": cue["id"], "start": t, "sentences": []}
            for i, (say, cap) in enumerate(zip(says, caps)):
                if i > 0:
                    t += SENTENCE_GAP
                samples = speak(say)
                clips.append((t, samples))
                dur = len(samples) / SR
                c["sentences"].append({"start": round(t, 3), "end": round(t + dur, 3), "cap": cap})
                captions.append((t, t + dur, cap))
                t += dur
            c["end"] = round(t, 3)
            c["start"] = round(c["start"], 3)
            sc["cues"].append(c)
            t += cue.get("hold", 0.0) + script["cue_gap"]
        t += script["scene_gap"]
        sc["end"] = round(t, 3)
        sc["start"] = round(sc["start"], 3)
        scenes.append(sc)
    total = t + script["tail"]
    scenes[-1]["end"] = round(total, 3)

    track = np.zeros(int(total * SR) + 1, dtype=np.float32)
    for start, samples in clips:
        i = int(round(start * SR))
        track[i:i + len(samples)] += samples
    peak = float(np.max(np.abs(track)))
    track *= 0.89 / peak            # about -1 dBFS
    sf.write(os.path.join(out, "narration.wav"), track, SR)

    with open(os.path.join(out, "timeline.js"), "w") as f:
        f.write("window.TIMELINE = ")
        json.dump({"total": round(total, 3), "scenes": scenes}, f, indent=1)
        f.write(";\n")
    with open(os.path.join(out, "captions.srt"), "w") as f:
        for n, (a, b, cap) in enumerate(captions, 1):
            f.write("%d\n%s --> %s\n%s\n\n" % (n, srt_time(a), srt_time(b + 0.3), cap))
    with open(os.path.join(out, "chapters.txt"), "w") as f:
        f.write(";FFMETADATA1\ntitle=pg_lion explained: Lion vs B-tree vs GIN\n")
        for sc in scenes:
            f.write("\n[CHAPTER]\nTIMEBASE=1/1000\nSTART=%d\nEND=%d\ntitle=%s\n"
                    % (sc["start"] * 1000, sc["end"] * 1000, sc["title"]))
    words = sum(len(s.split()) for sc in script["scenes"] for c in sc["cues"] for s in [c["say"]])
    speech = sum(len(s) / SR for _, s in clips)
    print("total %.1f s, %d words, %.0f words/min of speech, peak before normalising %.3f"
          % (total, words, words / speech * 60, peak))
    for sc in scenes:
        print("  %-9s %6.1f .. %6.1f  %s" % (sc["id"], sc["start"], sc["end"], sc["title"]))


if __name__ == "__main__":
    main()
