#!/usr/bin/env python3
"""Render scenes.html frame by frame and mux it with the narration.

  render.py BUILD_DIR stills T1 T2 ...     PNGs of single instants, for checking a layout
  render.py BUILD_DIR video [--jobs N]     BUILD_DIR/pg_lion_explained.mp4
  render.py BUILD_DIR mux                  the same, from the parts already rendered (after a caption
                                           or chapter change that leaves the timeline alone)

BUILD_DIR must hold timeline.js and narration.wav from build_audio.py.  The page and its fonts
are copied next to them, and Chromium screenshots the page once per frame, with renderAt(t)
posing it for that frame's time; ffmpeg (from imageio-ffmpeg) encodes the frames.
"""
import os
import shutil
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor

import imageio_ffmpeg
from playwright.sync_api import sync_playwright

HERE = os.path.dirname(os.path.abspath(__file__))
FPS = 30
FFMPEG = imageio_ffmpeg.get_ffmpeg_exe()


def stage(build):
    shutil.copy(os.path.join(HERE, "scenes.html"), os.path.join(build, "scenes.html"))
    shutil.copytree(os.path.join(HERE, "fonts"), os.path.join(build, "fonts"), dirs_exist_ok=True)
    return "file://" + os.path.abspath(os.path.join(build, "scenes.html"))


def open_page(p, url):
    # CHROMIUM names a browser binary when Playwright's own download is not the one installed
    browser = p.chromium.launch(executable_path=os.environ.get("CHROMIUM") or None,
                                args=["--force-color-profile=srgb", "--font-render-hinting=none"])
    page = browser.new_page(viewport={"width": 1920, "height": 1080}, device_scale_factor=1)
    page.goto(url)
    page.evaluate("document.fonts.ready")
    page.wait_for_timeout(300)
    return browser, page


def stills(build, times):
    url = stage(build)
    with sync_playwright() as p:
        browser, page = open_page(p, url)
        for t in times:
            page.evaluate("t => renderAt(t)", float(t))
            out = os.path.join(build, "still_%07.2f.png" % float(t))
            page.locator("#stage").screenshot(path=out)
            print(out)
        browser.close()


def render_range(args):
    build, url, first, last, out = args
    enc = subprocess.Popen([FFMPEG, "-y", "-loglevel", "error", "-f", "image2pipe", "-framerate", str(FPS),
                            "-c:v", "png", "-i", "-", "-c:v", "libx264", "-preset", "medium", "-crf", "20",
                            "-tune", "animation", "-pix_fmt", "yuv420p", "-g", str(FPS * 4), out],
                           stdin=subprocess.PIPE)
    with sync_playwright() as p:
        browser, page = open_page(p, url)
        cdp = page.context.new_cdp_session(page)
        import base64
        for f in range(first, last):
            page.evaluate("t => renderAt(t)", f / FPS)
            shot = cdp.send("Page.captureScreenshot", {"format": "png", "optimizeForSpeed": True,
                                                        "clip": {"x": 0, "y": 0, "width": 1920, "height": 1080, "scale": 1}})
            enc.stdin.write(base64.b64decode(shot["data"]))
            if (f - first) % 600 == 0:
                print("  frames %d..%d: at %d" % (first, last, f), flush=True)
        browser.close()
    enc.stdin.close()
    if enc.wait() != 0:
        raise RuntimeError("ffmpeg failed on " + out)
    return out


def video(build, jobs):
    url = stage(build)
    with sync_playwright() as p:
        browser, page = open_page(p, url)
        total = page.evaluate("window.TOTAL")
        browser.close()
    frames = int(total * FPS)
    bounds = [frames * k // jobs for k in range(jobs + 1)]
    parts = [(build, url, bounds[k], bounds[k + 1], os.path.join(build, "part%02d.mp4" % k)) for k in range(jobs)]
    print("%d frames (%.1f s) in %d parts" % (frames, total, jobs), flush=True)
    with ProcessPoolExecutor(jobs) as ex:
        outs = list(ex.map(render_range, parts))
    with open(os.path.join(build, "parts.txt"), "w") as f:
        for o in outs:
            f.write("file '%s'\n" % os.path.abspath(o))
    mux(build)


def mux(build):
    """Join the rendered parts with the narration, captions and chapters."""
    out = os.path.join(build, "pg_lion_explained.mp4")
    subprocess.check_call([FFMPEG, "-y", "-loglevel", "error",
                           "-f", "concat", "-safe", "0", "-i", os.path.join(build, "parts.txt"),
                           "-i", os.path.join(build, "narration.wav"),
                           "-i", os.path.join(build, "captions.srt"),
                           "-i", os.path.join(build, "chapters.txt"),
                           "-map", "0:v", "-map", "1:a", "-map", "2:s", "-map_metadata", "3", "-map_chapters", "3",
                           "-c:v", "copy", "-c:a", "aac", "-b:a", "160k", "-ar", "48000",
                           "-c:s", "mov_text", "-metadata:s:s:0", "language=eng",
                           "-movflags", "+faststart", "-shortest", out])
    print(out)


if __name__ == "__main__":
    build, mode = sys.argv[1], sys.argv[2]
    if mode == "stills":
        stills(build, sys.argv[3:])
    elif mode == "mux":
        mux(build)
    else:
        jobs = int(sys.argv[sys.argv.index("--jobs") + 1]) if "--jobs" in sys.argv else 4
        video(build, jobs)
