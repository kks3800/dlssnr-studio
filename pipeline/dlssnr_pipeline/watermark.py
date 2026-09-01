"""EU AI Act Article 50 disclosure mark.

Two halves, and they are not interchangeable:

  * the visible badge built here -- a translucent ring with "AI" in it,
    which is what a human sees;
  * the machine-readable marking in provenance.py, which is what Art. 50(2)
    actually obliges ("marked in a machine-readable format and detectable as
    artificially generated or manipulated").

A burned-in circle on its own does not satisfy the machine-readable limb, so
the pipeline always writes both. The badge is rendered once per output height
and composited by a single ffmpeg overlay -- doing it per frame in Python
would cost more than the neural pass.
"""
from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

# Rendered at this multiple then downsampled, which is what keeps the ring edge
# and the glyph shoulders smooth at small diameters.
SUPERSAMPLE = 4

POSITIONS = {
    "bottom-right":  "x=W-w-{m}:y=H-h-{m}",
    "bottom-left":   "x={m}:y=H-h-{m}",
    "bottom-center": "x=(W-w)/2:y=H-h-{m}",
}

_FONT_CANDIDATES = (
    "segoeuib.ttf",      # Segoe UI Bold
    "arialbd.ttf",
    "DejaVuSans-Bold.ttf",
    "LiberationSans-Bold.ttf",
)


def _load_font(size: int) -> ImageFont.FreeTypeFont:
    for name in _FONT_CANDIDATES:
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            continue
    return ImageFont.load_default(size)


def _fit_font(text: str, box: float) -> ImageFont.FreeTypeFont:
    """Largest font whose rendered text fits `box` on both axes."""
    size = max(8, int(box))
    while size > 6:
        font = _load_font(size)
        left, top, right, bottom = font.getbbox(text)
        if (right - left) <= box and (bottom - top) <= box:
            return font
        size = int(size * 0.92)
    return _load_font(8)


def build_badge(diameter: int, *, text: str = "AI", opacity: float = 0.35,
                out_path: Path) -> Path:
    """Render the disclosure badge to a PNG with opacity baked into alpha."""
    diameter = max(16, int(diameter))
    size = diameter * SUPERSAMPLE
    ring = max(SUPERSAMPLE * 2, int(size * 0.075))

    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)

    # A dark disc under the white ring keeps the mark readable over bright
    # footage; without it the badge vanishes on a snow or sky shot.
    draw.ellipse([0, 0, size - 1, size - 1], fill=(0, 0, 0, 90))
    inset = ring / 2
    draw.ellipse([inset, inset, size - 1 - inset, size - 1 - inset],
                 outline=(255, 255, 255, 255), width=ring)

    # Text is fitted to the inner square of the ring, not the full circle.
    inner = (size - 2 * ring) * 0.72
    font = _fit_font(text, inner)
    left, top, right, bottom = font.getbbox(text)
    draw.text(((size - (right + left)) / 2, (size - (bottom + top)) / 2),
              text, font=font, fill=(255, 255, 255, 255))

    img = img.resize((diameter, diameter), Image.LANCZOS)

    if not 0.0 < opacity <= 1.0:
        opacity = 0.35
    alpha = img.getchannel("A").point(lambda a: int(a * opacity))
    img.putalpha(alpha)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    img.save(out_path)
    return out_path


def filter_chain(badge: Path, video_height: int, *, position: str,
                 margin_pct: float) -> tuple[list[str], str]:
    """ffmpeg input args plus the filter_complex string for the overlay."""
    if position not in POSITIONS:
        raise ValueError(
            f"position {position!r} not one of {', '.join(POSITIONS)}")
    margin = max(4, int(video_height * margin_pct / 100.0))
    placement = POSITIONS[position].format(m=margin)
    # [0:v] is the frame source, [1:v] the badge; format=auto keeps the
    # overlay from silently forcing an 8-bit pipeline on 10-bit sources.
    chain = f"[0:v][1:v]overlay={placement}:format=auto[v]"
    return ["-i", str(badge)], chain


def badge_diameter(video_height: int, size_pct: float) -> int:
    return max(24, int(video_height * size_pct / 100.0))
