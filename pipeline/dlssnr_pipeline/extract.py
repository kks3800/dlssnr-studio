"""Video -> numbered frame sequence.

NR reads 16-bit PNG happily and the extra range matters: the denoiser works on
values that an 8-bit intermediate would have already crushed, particularly in
the near-blacks where compression noise lives.
"""
from __future__ import annotations

import subprocess
from pathlib import Path

from .probe import MediaInfo

PATTERN = "frame.%06d.png"


def extract(ffmpeg: str, src: Path, dest: Path, info: MediaInfo, *,
            fmt: str = "png16", fps: float = 0.0,
            on_log=print) -> tuple[Path, int]:
    """Decode `src` into `dest` as frame.NNNNNN.png. Returns (dir, fps used)."""
    dest.mkdir(parents=True, exist_ok=True)
    for stale in dest.glob("frame.*.png"):
        stale.unlink()

    rate = fps or info.fps
    if rate <= 0:
        raise RuntimeError(f"could not determine a frame rate for {src.name}")

    pix_fmt = "rgb48be" if fmt == "png16" else "rgb24"
    cmd = [ffmpeg, "-v", "error", "-stats", "-y", "-i", str(src)]
    if fps:
        cmd += ["-vf", f"fps={fps}"]
    cmd += ["-pix_fmt", pix_fmt, str(dest / PATTERN)]

    on_log(f"  extract -> {dest} ({pix_fmt} @ {rate:g}fps)")
    subprocess.run(cmd, check=True)

    count = len(list(dest.glob("frame.*.png")))
    if count == 0:
        raise RuntimeError(f"no frames extracted from {src}")
    on_log(f"  extracted {count} frames")
    return dest, rate


def extract_audio(ffmpeg: str, src: Path, dest: Path, on_log=print) -> Path | None:
    """Pull the audio aside untouched so the neural pass never touches it."""
    out = dest / "audio.m4a"
    result = subprocess.run(
        [ffmpeg, "-v", "error", "-y", "-i", str(src), "-vn",
         "-c:a", "copy", str(out)],
        capture_output=True, text=True)
    if result.returncode != 0 or not out.exists():
        # Copy fails when the source codec cannot live in m4a; re-encode once.
        result = subprocess.run(
            [ffmpeg, "-v", "error", "-y", "-i", str(src), "-vn",
             "-c:a", "aac", "-b:a", "384k", str(out)],
            capture_output=True, text=True)
        if result.returncode != 0 or not out.exists():
            on_log("  audio: none carried over")
            return None
    on_log(f"  audio -> {out.name}")
    return out
