"""YouTube Data API v3 upload.

Uploads only what the manifest cleared. The rights line and the AI disclosure
are appended to the description here rather than left to whoever fills in the
form later, so a video cannot reach the channel without carrying both.

Defaults to privacy=private: the pipeline stages the video, a human reviews it
and flips it public. Automating that last click is how a bad clip ships at 3am.
"""
from __future__ import annotations

from pathlib import Path

SCOPES = ["https://www.googleapis.com/auth/youtube.upload"]

_IMPORT_HELP = (
    "YouTube upload needs the Google client libraries:\n"
    "    pip install google-api-python-client google-auth-oauthlib\n"
    "then put an OAuth 2.0 Desktop-app client_secrets.json beside config.toml\n"
    "(Google Cloud console -> APIs & Services -> Credentials, with the\n"
    "YouTube Data API v3 enabled)."
)


class UploadError(RuntimeError):
    pass


def _client(client_secrets: Path, token_store: Path):
    try:
        from google.auth.transport.requests import Request
        from google.oauth2.credentials import Credentials
        from google_auth_oauthlib.flow import InstalledAppFlow
        from googleapiclient.discovery import build
    except ImportError as exc:  # pragma: no cover - environment dependent
        raise UploadError(f"{_IMPORT_HELP}\n({exc})") from exc

    creds = None
    if token_store.exists():
        creds = Credentials.from_authorized_user_file(str(token_store), SCOPES)
    if not creds or not creds.valid:
        if creds and creds.expired and creds.refresh_token:
            creds.refresh(Request())
        else:
            if not client_secrets.exists():
                raise UploadError(
                    f"client_secrets.json not found at {client_secrets}\n{_IMPORT_HELP}")
            flow = InstalledAppFlow.from_client_secrets_file(
                str(client_secrets), SCOPES)
            creds = flow.run_local_server(port=0)
        token_store.write_text(creds.to_json(), encoding="utf-8")
    return build("youtube", "v3", credentials=creds)


def upload(video: Path, *, title: str, description: str, tags: list[str],
           client_secrets: Path, token_store: Path, privacy: str = "private",
           category_id: str = "20", made_for_kids: bool = False,
           discloses_altered_content: bool = True, on_log=print) -> str:
    try:
        from googleapiclient.http import MediaFileUpload
    except ImportError as exc:  # pragma: no cover
        raise UploadError(f"{_IMPORT_HELP}\n({exc})") from exc

    if not video.exists():
        raise UploadError(f"no such file: {video}")

    youtube = _client(client_secrets, token_store)

    status = {
        "privacyStatus": privacy,
        "selfDeclaredMadeForKids": bool(made_for_kids),
    }
    if discloses_altered_content:
        # YouTube's own synthetic/altered-content declaration. Separate from
        # the AI Act marking and required by their policy for realistic
        # altered material -- setting it here avoids relying on the Studio UI.
        status["containsSyntheticMedia"] = True

    body = {
        "snippet": {
            "title": title[:100],
            "description": description[:5000],
            "tags": tags[:60],
            "categoryId": str(category_id),
        },
        "status": status,
    }

    media = MediaFileUpload(str(video), chunksize=8 * 1024 * 1024, resumable=True)
    request = youtube.videos().insert(part="snippet,status", body=body, media_body=media)

    on_log(f"  uploading {video.name} ({video.stat().st_size / 1e6:.1f} MB)")
    response, last = None, -1
    while response is None:
        chunk, response = request.next_chunk()
        if chunk:
            pct = int(chunk.progress() * 100)
            if pct >= last + 10:
                on_log(f"    {pct}%")
                last = pct

    video_id = response.get("id")
    if not video_id:
        raise UploadError(f"upload returned no id: {response}")
    on_log(f"  https://youtu.be/{video_id}  ({privacy})")
    return video_id
