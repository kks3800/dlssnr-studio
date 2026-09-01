#!/usr/bin/env python
"""dlssnr pipeline -- source manifest to finished, marked, uploadable master.

    python run.py                      process everything in the manifest
    python run.py --only sky_capture   just one item
    python run.py --dry-run            print the plan, touch nothing
    python run.py --badge-preview      render the AI badge and exit
    python run.py --upload             actually push to YouTube
"""
from __future__ import annotations

import argparse
import shutil
import sys
import traceback
from pathlib import Path

from dlssnr_pipeline import (config, encode, enhance, extract, manifest,
                             probe, provenance, watermark)


def log(msg: str = "") -> None:
    print(msg, flush=True)


def process(src: manifest.Source, cfg: config.Config, ffmpeg: str, ffprobe: str,
            *, do_upload: bool, keep_work: bool) -> Path:
    log(f"\n=== {src.key}: {src.title}")
    log(f"  rights: {src.rights}")

    info = probe.inspect(ffprobe, src.path)
    log(f"  source: {src.path.name} -- {info.label}")

    work = cfg.work_root / src.key
    raw_dir, enhanced_dir = work / "raw", work / "enhanced"
    work.mkdir(parents=True, exist_ok=True)

    # ---- extract ----------------------------------------------------------
    _, fps = extract.extract(
        ffmpeg, src.path, raw_dir, info,
        fmt=str(cfg.get("extract", "format", "png16")),
        fps=float(cfg.get("extract", "fps", 0) or 0),
        on_log=log)
    audio = extract.extract_audio(ffmpeg, src.path, work, on_log=log) \
        if info.has_audio else None

    # ---- neural pass ------------------------------------------------------
    opts = dict(cfg.section("enhance"))
    enhance.run(cfg.exe, raw_dir, enhanced_dir, opts, on_log=log)
    pattern, start = enhance.output_pattern(enhanced_dir)

    # Output height drives badge size, and upscaling may have changed it.
    first = sorted(enhanced_dir.glob("*.png"))[0]
    from PIL import Image
    with Image.open(first) as probe_img:
        out_h = probe_img.height
    if out_h != info.height:
        log(f"  upscaled {info.height}p -> {out_h}p")

    # ---- disclosure + encode ---------------------------------------------
    generator = str(cfg.get("provenance", "generator", "dlssnr-studio"))
    declaration = str(cfg.get("provenance", "declaration", "AI-enhanced."))
    tags = provenance.container_metadata(generator, declaration, src.rights_line())
    tags["title"] = src.title

    out_path = cfg.out_root / f"{src.key}.mp4"
    encode.encode(
        ffmpeg, enhanced_dir, pattern, start, fps, out_path,
        height=out_h, audio=audio,
        wm=dict(cfg.section("watermark")),
        codec=str(cfg.get("encode", "codec", "h264")),
        crf=int(cfg.get("encode", "crf", 16)),
        preset=str(cfg.get("encode", "preset", "slow")),
        audio_kbps=int(cfg.get("encode", "audio_kbps", 384)),
        faststart=bool(cfg.get("encode", "faststart", True)),
        metadata=tags, work_dir=work, on_log=log)

    # ---- provenance -------------------------------------------------------
    signed, note = False, "disabled"
    if cfg.get("provenance", "enabled", True):
        if cfg.get("provenance", "c2pa", False):
            signed, note = provenance.sign_c2pa(
                out_path, provenance.c2pa_manifest(generator, declaration), on_log=log)
        else:
            note = "c2pa not requested in config"
        sidecar = provenance.write_sidecar(
            out_path, generator=generator, declaration=declaration,
            source={"key": src.key, "title": src.title, "rights": src.rights,
                    "attribution": src.attribution, "url": src.source_url,
                    "file": src.path.name},
            settings=opts, signed=signed, note=note)
        log(f"  provenance -> {sidecar.name}")

    log(f"  master: {out_path}  ({out_path.stat().st_size / 1e6:.1f} MB)")

    # ---- upload -----------------------------------------------------------
    if do_upload:
        from dlssnr_pipeline import upload as uploader
        description = "\n\n".join(filter(None, [
            src.notes,
            provenance.disclosure_block(declaration, src.rights_line()),
        ]))
        uploader.upload(
            out_path, title=src.title, description=description,
            tags=list(src.tags),
            client_secrets=cfg.root / str(cfg.get("upload", "client_secrets",
                                                  "client_secrets.json")),
            token_store=cfg.root / str(cfg.get("upload", "token_store", "token.json")),
            privacy=str(cfg.get("upload", "privacy", "private")),
            category_id=str(cfg.get("upload", "category_id", "20")),
            made_for_kids=bool(cfg.get("upload", "made_for_kids", False)),
            discloses_altered_content=bool(
                cfg.get("upload", "disclose_altered", True)),
            on_log=log)

    if not keep_work:
        shutil.rmtree(work, ignore_errors=True)
    return out_path


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", type=Path)
    ap.add_argument("--only", action="append", metavar="KEY")
    ap.add_argument("--upload", action="store_true",
                    help="push to YouTube (otherwise stops at the master)")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--keep-work", action="store_true",
                    help="keep extracted/enhanced frames for inspection")
    ap.add_argument("--badge-preview", action="store_true",
                    help="render the AI badge over three grounds and exit")
    args = ap.parse_args(argv)

    try:
        cfg = config.load(args.config)
    except config.ConfigError as exc:
        log(f"error: {exc}")
        return 2

    if args.badge_preview:
        wm = cfg.section("watermark")
        out = cfg.work_root / "badge_preview.png"
        badge_path = watermark.build_badge(
            240, text=str(wm.get("text", "AI")),
            opacity=float(wm.get("opacity", 0.35)),
            out_path=cfg.work_root / "badge.png")
        from PIL import Image
        badge = Image.open(badge_path)
        canvas = Image.new("RGB", (900, 300))
        for i, colour in enumerate([(18, 18, 20), (128, 128, 128), (235, 238, 245)]):
            tile = Image.new("RGB", (300, 300), colour)
            tile.paste(badge, (30, 30), badge)
            canvas.paste(tile, (i * 300, 0))
        canvas.save(out)
        log(f"badge preview -> {out}")
        return 0

    try:
        ffmpeg, ffprobe = config.require_ffmpeg()
        sources = manifest.load(cfg.manifest_path)
    except (config.ConfigError, manifest.ManifestError) as exc:
        log(f"error: {exc}")
        return 2

    if args.only:
        wanted = set(args.only)
        sources = [s for s in sources if s.key in wanted]
        missing = wanted - {s.key for s in sources}
        if missing:
            log(f"error: no manifest entry named {', '.join(sorted(missing))}")
            return 2
    if not sources:
        log("nothing to do")
        return 0

    upload_on = args.upload or bool(cfg.get("upload", "enabled", False))

    log(f"{len(sources)} item(s); upload {'ON' if upload_on else 'off'}")
    if args.dry_run:
        for s in sources:
            log(f"  {s.key:<20} {s.rights:<14} {s.path}")
        return 0

    failures = 0
    for src in sources:
        try:
            process(src, cfg, ffmpeg, ffprobe,
                    do_upload=upload_on, keep_work=args.keep_work)
        except Exception as exc:  # keep the batch going
            failures += 1
            log(f"  FAILED {src.key}: {exc}")
            if not isinstance(exc, (RuntimeError, OSError)):
                traceback.print_exc()

    log(f"\ndone: {len(sources) - failures} ok, {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
