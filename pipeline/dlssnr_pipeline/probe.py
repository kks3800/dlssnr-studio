"""ffprobe wrappers."""
from __future__ import annotations

import json
import subprocess
from dataclasses import dataclass
from fractions import Fraction
from pathlib import Path


@dataclass
class MediaInfo:
    width: int
    height: int
    fps: float
    duration: float
    frames: int
    has_audio: bool
    pix_fmt: str

    @property
    def label(self) -> str:
        audio = ", audio" if self.has_audio else ", no audio"
        return (f"{self.width}x{self.height} @ {self.fps:g}fps, "
                f"{self.duration:.1f}s, ~{self.frames} frames{audio}")


def inspect(ffprobe: str, path: Path) -> MediaInfo:
    out = subprocess.run(
        [ffprobe, "-v", "error", "-print_format", "json",
         "-show_streams", "-show_format", str(path)],
        capture_output=True, text=True, check=True).stdout
    data = json.loads(out)
    streams = data.get("streams", [])
    video = next((s for s in streams if s.get("codec_type") == "video"), None)
    if video is None:
        raise RuntimeError(f"no video stream in {path}")

    # avg_frame_rate is the safer of the two for variable-rate captures.
    rate = video.get("avg_frame_rate") or video.get("r_frame_rate") or "0/1"
    try:
        fps = float(Fraction(rate))
    except (ZeroDivisionError, ValueError):
        fps = 0.0

    duration = float(data.get("format", {}).get("duration") or 0.0)
    frames = int(video.get("nb_frames") or 0) or int(round(duration * fps))

    return MediaInfo(
        width=int(video["width"]),
        height=int(video["height"]),
        fps=fps,
        duration=duration,
        frames=frames,
        has_audio=any(s.get("codec_type") == "audio" for s in streams),
        pix_fmt=str(video.get("pix_fmt", "")),
    )
