"""Enhanced frames -> a delivery master, with the disclosure badge burned in."""
from __future__ import annotations

import subprocess
from pathlib import Path

from . import watermark


def _color_args() -> list[str]:
    # Tag bt709 explicitly. Untagged masters get guessed at by the transcoder
    # on the far end, and the guess is what makes uploads come back washed out.
    return ["-colorspace", "bt709", "-color_primaries", "bt709",
            "-color_trc", "bt709", "-color_range", "tv"]


def encode(ffmpeg: str, frames_dir: Path, pattern: str, start: int, fps: float,
           out_path: Path, *, height: int, audio: Path | None = None,
           wm: dict | None = None, codec: str = "h264", crf: int = 16,
           preset: str = "slow", audio_kbps: int = 384, faststart: bool = True,
           metadata: dict | None = None, work_dir: Path | None = None,
           on_log=print) -> Path:
    out_path.parent.mkdir(parents=True, exist_ok=True)

    cmd = [ffmpeg, "-v", "error", "-stats", "-y",
           "-framerate", f"{fps:g}", "-start_number", str(start),
           "-i", str(frames_dir / pattern)]

    filter_complex, video_map = None, "0:v"
    next_input = 1

    if wm and wm.get("enabled", True):
        diameter = watermark.badge_diameter(height, float(wm.get("size_pct", 5.0)))
        badge = watermark.build_badge(
            diameter,
            text=str(wm.get("text", "AI")),
            opacity=float(wm.get("opacity", 0.35)),
            out_path=(work_dir or out_path.parent) / "ai_badge.png")
        args, filter_complex = watermark.filter_chain(
            badge, height,
            position=str(wm.get("position", "bottom-right")),
            margin_pct=float(wm.get("margin_pct", 2.5)))
        cmd += args
        video_map, next_input = "[v]", next_input + 1
        on_log(f"  badge {diameter}px {wm.get('position')} "
               f"@ {float(wm.get('opacity', 0.35)):.0%}")

    audio_index = None
    if audio and audio.exists():
        cmd += ["-i", str(audio)]
        audio_index = next_input

    if filter_complex:
        cmd += ["-filter_complex", filter_complex]

    cmd += ["-map", video_map]
    if audio_index is not None:
        cmd += ["-map", f"{audio_index}:a", "-c:a", "aac",
                "-b:a", f"{audio_kbps}k"]

    if codec == "prores":
        cmd += ["-c:v", "prores_ks", "-profile:v", "3", "-pix_fmt", "yuv422p10le"]
    else:
        cmd += ["-c:v", "libx264", "-preset", preset, "-crf", str(crf),
                "-profile:v", "high", "-pix_fmt", "yuv420p"]
        if faststart:
            cmd += ["-movflags", "+faststart"]

    cmd += _color_args()
    for key, value in (metadata or {}).items():
        if value:
            cmd += ["-metadata", f"{key}={value}"]
    cmd.append(str(out_path))

    on_log(f"  encode -> {out_path.name} ({codec})")
    subprocess.run(cmd, check=True)
    if not out_path.exists():
        raise RuntimeError(f"encode produced nothing at {out_path}")
    return out_path
