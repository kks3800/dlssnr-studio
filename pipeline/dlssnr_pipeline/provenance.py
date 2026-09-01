"""Machine-readable AI marking.

EU AI Act Art. 50(2) asks for outputs of a generative or manipulating system
to be "marked in a machine-readable format and detectable as artificially
generated or manipulated". This module writes that marking. It is on by
default; [provenance] in config.toml turns it off.

Three layers, weakest to strongest:
  1. container metadata (survives copying, lost on re-encode)
  2. a sidecar JSON manifest (never lost, trivially separated)
  3. C2PA Content Credentials (cryptographically signed, the real answer)

C2PA needs c2patool on PATH plus a signing certificate. Without those the
first two still apply and the manifest records that signing was unavailable,
so an unsigned output is never silently passed off as a signed one.
"""
from __future__ import annotations

import json
import shutil
import subprocess
from datetime import datetime, timezone
from pathlib import Path


def container_metadata(generator: str, declaration: str,
                       source_rights: str = "") -> dict:
    """Tags ffmpeg writes into the output container."""
    tags = {
        "comment": declaration,
        "description": declaration,
        "encoder": generator,
        # No registered key exists for this yet; these are the de-facto ones
        # tools look for, so write both rather than betting on one.
        "AIGenerated": "true",
        "DigitalSourceType": "trainedAlgorithmicMedia",
    }
    if source_rights:
        tags["copyright"] = source_rights
    return tags


def write_sidecar(out_path: Path, *, generator: str, declaration: str,
                  source: dict, settings: dict, signed: bool,
                  note: str = "") -> Path:
    manifest = {
        "schema": "dlssnr-provenance/1",
        "created": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "file": out_path.name,
        "ai": {
            "modified": True,
            "kind": "enhancement",
            "operations": ["neural denoise", "neural upscale"],
            "generator": generator,
            "declaration": declaration,
            "digital_source_type": "trainedAlgorithmicMedia",
            "synthetic_content": False,
            "deep_fake": False,
        },
        "source": source,
        "settings": settings,
        "c2pa": {"signed": signed, "note": note},
    }
    path = out_path.with_suffix(out_path.suffix + ".provenance.json")
    path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return path


def c2pa_available() -> bool:
    return shutil.which("c2patool") is not None


def sign_c2pa(out_path: Path, manifest: dict, on_log=print) -> tuple[bool, str]:
    """Attach signed Content Credentials. Returns (signed, note)."""
    tool = shutil.which("c2patool")
    if not tool:
        note = "c2patool not on PATH; container metadata and sidecar only"
        on_log(f"  c2pa: skipped -- {note}")
        return False, note

    manifest_path = out_path.with_suffix(".c2pa.json")
    manifest_path.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    signed_path = out_path.with_name(out_path.stem + ".signed" + out_path.suffix)

    result = subprocess.run(
        [tool, str(out_path), "-m", str(manifest_path), "-o", str(signed_path), "-f"],
        capture_output=True, text=True)
    if result.returncode != 0:
        note = (result.stderr or result.stdout or "c2patool failed").strip()[:400]
        on_log(f"  c2pa: failed -- {note}")
        return False, note

    signed_path.replace(out_path)
    manifest_path.unlink(missing_ok=True)
    on_log("  c2pa: signed")
    return True, "signed with c2patool"


def c2pa_manifest(generator: str, declaration: str) -> dict:
    return {
        "claim_generator": generator.replace(" ", "_"),
        "title": "AI-enhanced video",
        "assertions": [
            {
                "label": "c2pa.actions",
                "data": {"actions": [
                    {"action": "c2pa.edited",
                     "softwareAgent": generator,
                     "description": declaration},
                    {"action": "c2pa.filtered",
                     "softwareAgent": generator,
                     "digitalSourceType":
                         "http://cv.iptc.org/newscodes/digitalsourcetype/"
                         "trainedAlgorithmicMedia"},
                ]},
            },
        ],
    }


def disclosure_block(declaration: str, rights_line: str) -> str:
    """The paragraph appended to the video description."""
    lines = [
        "---",
        f"AI disclosure: {declaration}",
        "Marked under EU AI Act Art. 50 transparency obligations.",
    ]
    if rights_line:
        lines.append(rights_line)
    return "\n".join(lines)
