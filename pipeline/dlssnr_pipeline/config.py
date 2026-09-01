"""Configuration loading and path resolution."""
from __future__ import annotations

import shutil
import tomllib
from dataclasses import dataclass, field
from pathlib import Path


class ConfigError(RuntimeError):
    pass


@dataclass
class Config:
    root: Path                      # pipeline/ directory
    repo: Path                      # dlssnr-image/ directory
    data: dict = field(default_factory=dict)

    def section(self, name: str) -> dict:
        return self.data.get(name, {}) or {}

    def get(self, section: str, key: str, default=None):
        value = self.section(section).get(key, default)
        # Blank strings in the toml mean "unset", not "empty string".
        if isinstance(value, str) and not value.strip():
            return default
        return value

    # ---- resolved paths ---------------------------------------------------
    @property
    def exe(self) -> Path:
        configured = self.get("paths", "exe")
        if configured:
            path = Path(configured)
            if not path.is_absolute():
                path = self.root / path
        else:
            path = self.repo / "build" / "Release" / "dlssnr-image.exe"
        if not path.exists():
            raise ConfigError(
                f"dlssnr-image.exe not found at {path}\n"
                f"Build it first:  cmake --build build --config Release"
            )
        return path

    def _dir(self, key: str, fallback: str) -> Path:
        path = Path(self.get("paths", key, fallback))
        if not path.is_absolute():
            path = self.root / path
        path.mkdir(parents=True, exist_ok=True)
        return path

    @property
    def work_root(self) -> Path:
        return self._dir("work_root", "work")

    @property
    def out_root(self) -> Path:
        return self._dir("out_root", "out")

    @property
    def manifest_path(self) -> Path:
        path = Path(self.get("paths", "manifest", "sources.toml"))
        return path if path.is_absolute() else self.root / path


def load(path: Path | None = None) -> Config:
    root = Path(__file__).resolve().parent.parent
    cfg_path = path or (root / "config.toml")
    if not cfg_path.exists():
        raise ConfigError(f"config not found: {cfg_path}")
    with cfg_path.open("rb") as handle:
        data = tomllib.load(handle)
    return Config(root=root, repo=root.parent, data=data)


def require_ffmpeg() -> tuple[str, str]:
    ffmpeg, ffprobe = shutil.which("ffmpeg"), shutil.which("ffprobe")
    if not ffmpeg or not ffprobe:
        raise ConfigError("ffmpeg and ffprobe must be on PATH")
    return ffmpeg, ffprobe
