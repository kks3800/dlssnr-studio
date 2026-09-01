"""Drive dlssnr-image in sequence mode.

Sequence mode is the whole point. NR carries temporal history from frame to
frame and resets only on the first, which is the configuration the network was
trained for -- running each frame as an independent still produces a result
that is individually clean and collectively unwatchable, because the denoiser
settles on a slightly different answer every frame and the sequence crawls.

Captured video has no engine passes, so depth and motion have to be inferred:
--estimate-depth runs DepthAnything, --estimate-motion runs RAFT optical flow.
Both are strictly worse than a real G-buffer and strictly better than nothing.
"""
from __future__ import annotations

import subprocess
from pathlib import Path


def build_command(exe: Path, frames: Path, out_dir: Path, opts: dict) -> list[str]:
    cmd = [str(exe), "--sequence", str(frames), "--out-dir", str(out_dir)]

    if opts.get("estimate_motion", True):
        cmd.append("--estimate-motion")
        if opts.get("motion_res"):
            cmd += ["--motion-res", str(int(opts["motion_res"]))]
        if opts.get("motion_scale"):
            cmd += ["--motion-scale", str(float(opts["motion_scale"]))]
    if opts.get("estimate_depth", True):
        cmd.append("--estimate-depth")

    if opts.get("iterations"):
        cmd += ["--iterations", str(int(opts["iterations"]))]

    # Anything left unset stays at the runtime's own default rather than being
    # pinned to a value we invented.
    for flag, key, cast in (
        ("--intensity", "intensity", float),
        ("--skin", "skin", float),
        ("--local-structure", "local_structure", float),
        ("--local-tone", "local_tone", float),
        ("--global-tone", "global_tone", float),
    ):
        value = opts.get(key)
        if value not in (None, ""):
            cmd += [flag, str(cast(value))]
    for flag, key in (("--style", "style"), ("--preset", "preset"),
                      ("--auto-mask", "auto_mask")):
        value = opts.get(key)
        if value not in (None, ""):
            cmd += [flag, str(int(value))]

    width, height = int(opts.get("width") or 0), int(opts.get("height") or 0)
    if width:
        cmd += ["--width", str(width)]
    if height:
        cmd += ["--height", str(height)]

    return cmd


def run(exe: Path, frames: Path, out_dir: Path, opts: dict,
        on_log=print) -> tuple[Path, int]:
    out_dir.mkdir(parents=True, exist_ok=True)
    for stale in out_dir.glob("*.png"):
        stale.unlink()

    cmd = build_command(exe, frames, out_dir, opts)
    on_log("  " + " ".join(cmd))

    # The neural pass is the long pole; stream its progress rather than
    # buffering, so a 5000-frame job is not a silent hour.
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True,
                            errors="replace", bufsize=1)
    tail: list[str] = []
    for line in proc.stdout:
        line = line.rstrip()
        if line:
            tail.append(line)
            del tail[:-40]
            on_log(f"    {line}")
    code = proc.wait()
    if code != 0:
        raise RuntimeError(
            "dlssnr-image failed (exit {}):\n{}".format(code, "\n".join(tail[-15:])))

    produced = sorted(out_dir.glob("*.png"))
    if not produced:
        raise RuntimeError(f"sequence mode wrote no frames into {out_dir}")
    on_log(f"  enhanced {len(produced)} frames")
    return out_dir, len(produced)


def output_pattern(out_dir: Path) -> tuple[str, int]:
    """Infer the printf pattern and first index of whatever it wrote."""
    files = sorted(out_dir.glob("*.png"))
    if not files:
        raise RuntimeError(f"no frames in {out_dir}")
    stem = files[0].stem
    digits, i = "", len(stem) - 1
    while i >= 0 and stem[i].isdigit():
        digits = stem[i] + digits
        i -= 1
    if not digits:
        raise RuntimeError(f"cannot infer numbering from {files[0].name}")
    prefix = stem[:i + 1]
    return f"{prefix}%0{len(digits)}d.png", int(digits)
