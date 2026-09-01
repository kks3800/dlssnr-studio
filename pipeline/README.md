# dlssnr pipeline

Batch driver for `dlssnr-image`: source clip → frames → NR sequence pass →
visible AI badge → encode → machine-readable marking → optional upload.

```
python run.py --dry-run          # show the plan
python run.py --badge-preview    # eyeball the AI badge
python run.py --keep-work        # process, keep intermediates
python run.py --upload           # process and push to YouTube
python run.py --only test_clip   # one manifest item
```

## Layout

| file | does |
|---|---|
| `config.toml` | all settings |
| `sources.toml` | what to process, and on what rights basis (`sources.example.toml` is a template) |
| `run.py` | orchestrator |
| `dlssnr_pipeline/extract.py` | video → PNG16 sequence, audio set aside |
| `dlssnr_pipeline/enhance.py` | drives `dlssnr-image --sequence` |
| `dlssnr_pipeline/watermark.py` | the visible AI badge |
| `dlssnr_pipeline/encode.py` | frames + badge + audio → master |
| `dlssnr_pipeline/provenance.py` | machine-readable AI marking |
| `dlssnr_pipeline/upload.py` | YouTube Data API v3 |

## Why sequence mode

`dlssnr-image --sequence` carries NR's temporal history across frames and
resets only on the first. Running frames as independent stills gives a result
that is clean frame-by-frame and crawls in motion, because the network settles
somewhere slightly different each time.

Captured video has no G-buffer, so the pipeline turns on `--estimate-motion`
(RAFT optical flow). Inferred flow is worse than a real velocity pass — if
footage comes from Movie Render Queue with a Velocity AOV, point `--sequence`
at the render root and it is picked up automatically. But it is far better
than nothing: measured over 48 frames, running with motion versus without
changes ~99% of pixels.

**Depth is off, on purpose.** Supplying it changes nothing on this snippet —
estimated, constant `0.05`, constant `0.95` and inverted all produce
bit-identical output — while Depth Anything costs about 1.1 s a frame to
compute. Turn it back on only if a future snippet starts honouring depth, and
verify with a diff before paying for it.

Per-frame costs are in the main README. Optical flow runs on the GPU through
DirectML; on a machine without a usable DX12 device it falls back to the CPU
provider and becomes the bottleneck.

## The AI mark

Two layers:

**Visible** — a translucent ring with `AI`, sized as a percentage of output
height so it looks the same at 1080p and 4K. Dark disc under a white ring so
it survives both a night scene and a snow scene. Rendered once at 4×
supersample, composited by a single ffmpeg `overlay`. Configure position,
size, opacity and text in `[watermark]`, or disable it there.

**Machine-readable** — container tags plus a `.provenance.json` sidecar
carrying the generator, a declaration string and the declared rights basis.
With `c2pa = true` and `c2patool` on PATH with a signing certificate it also
signs C2PA Content Credentials. Configure in `[provenance]`.

Known limitation: MP4's metadata atom set is restricted, so the custom
`AIGenerated` / `DigitalSourceType` keys are dropped by the container —
`comment`, `description` and `copyright` survive. The sidecar carries the full
set regardless. C2PA is the only layer that survives re-encoding.

`upload.py` sets YouTube's own altered-content flag on the video so the
declaration is made through the API rather than a checkbox in Studio.

## The rights gate

Every `[[source]]` declares `rights`, and only these are accepted:

`own-capture` · `licensed` · `dev-supplied` · `public-domain` · `cc`

Anything else is refused before a frame is decoded. The declared basis is
copied into the container `copyright` tag, the sidecar and the video
description, so it travels with the file.

## Setup

Needs `ffmpeg`/`ffprobe` on PATH, Pillow, and a built `dlssnr-image.exe`.
For upload:

```
pip install google-api-python-client google-auth-oauthlib
```

then an OAuth 2.0 **Desktop app** `client_secrets.json` beside `config.toml`
(Google Cloud console → Credentials, YouTube Data API v3 enabled). The first
`--upload` opens a browser once and caches `token.json`. Neither file is
committed.

Uploads default to `privacy = "private"`: stage the video, review it, then
flip it public yourself. A default API project allows roughly six uploads a
day before its quota runs out.
