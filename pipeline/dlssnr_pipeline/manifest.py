"""Source manifest.

Every clip that enters the pipeline declares where it came from and on what
basis it may be republished. This is not paperwork for its own sake: the rights
string is copied into the output metadata and the video description, so
provenance travels with the file instead of living in someone's memory.
"""
from __future__ import annotations

import tomllib
from dataclasses import dataclass
from pathlib import Path

# Bases that permit republication. Anything outside this set is refused.
ALLOWED_RIGHTS = {
    "own-capture":   "Footage captured by the uploader themselves.",
    "licensed":      "Written licence or press-kit terms permitting redistribution.",
    "dev-supplied":  "Supplied by the developer/publisher for this purpose.",
    "public-domain": "Out of copyright.",
    "cc":            "Creative Commons; attribution recorded with the item.",
}


class ManifestError(RuntimeError):
    pass


@dataclass
class Source:
    key: str
    path: Path
    title: str
    rights: str
    attribution: str = ""
    source_url: str = ""
    notes: str = ""
    tags: tuple[str, ...] = ()

    def rights_line(self) -> str:
        parts = [f"Source: {ALLOWED_RIGHTS[self.rights].rstrip('.')}"]
        if self.attribution:
            parts.append(f"Credit: {self.attribution}")
        if self.source_url:
            parts.append(self.source_url)
        return " | ".join(parts)


def _validate(key: str, raw: dict, base: Path) -> Source:
    missing = [f for f in ("path", "title", "rights") if not raw.get(f)]
    if missing:
        raise ManifestError(f"[{key}] missing required field(s): {', '.join(missing)}")

    rights = str(raw["rights"]).strip().lower()
    if rights not in ALLOWED_RIGHTS:
        allowed = "\n  ".join(f"{k:<14} {v}" for k, v in ALLOWED_RIGHTS.items())
        raise ManifestError(
            f"[{key}] rights = {rights!r} is not an accepted basis.\n"
            f"Use one of:\n  {allowed}\n"
            f"If none of these apply, the clip cannot be republished from here."
        )
    if rights == "cc" and not raw.get("attribution"):
        raise ManifestError(f"[{key}] rights = 'cc' requires an attribution field")

    path = Path(raw["path"])
    if not path.is_absolute():
        path = (base / path).resolve()
    if not path.exists():
        raise ManifestError(f"[{key}] file not found: {path}")

    return Source(
        key=key,
        path=path,
        title=str(raw["title"]),
        rights=rights,
        attribution=str(raw.get("attribution", "")),
        source_url=str(raw.get("source_url", "")),
        notes=str(raw.get("notes", "")),
        tags=tuple(raw.get("tags", ())),
    )


def load(path: Path) -> list[Source]:
    if not path.exists():
        raise ManifestError(
            f"no source manifest at {path}\n"
            f"Create one -- see sources.example.toml for the shape."
        )
    with path.open("rb") as handle:
        data = tomllib.load(handle)
    items = data.get("source")
    if not items:
        raise ManifestError(f"{path} declares no [[source]] entries")
    base = path.parent
    return [_validate(raw.get("key", f"source{i}"), raw, base)
            for i, raw in enumerate(items, 1)]
