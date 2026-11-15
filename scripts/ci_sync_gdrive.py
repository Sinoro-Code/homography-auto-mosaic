#!/usr/bin/env python3
"""
GitHub Actions -> Google Drive Permanent Sync Utility (Service Account / Drive API v3)

Uses a non-expiring Google Cloud Service Account JSON key (`GDRIVE_SERVICE_ACCOUNT_JSON`)
to overwrite packaged build artifacts (e.g. `自碼系統.apk`) in-place inside the shared
Google Drive folder (`自碼系統`, default ID `1O96AN1YzkmXz1BgRmHBioR3FS4CRTmBr`).

Why in-place PATCH update matters on personal Google Drive:
  Google Cloud Service Accounts have 0-byte personal storage quota, so `POST /files`
  (creating a new SA-owned file in personal My Drive) fails with `storageQuotaExceeded`.
  Updating an existing user-owned file via `PATCH /upload/drive/v3/files/{fileId}` keeps
  ownership on the user's Google account, consumes zero SA quota, preserves the exact
  sharing link & file ID forever, and never expires!
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import sys
from pathlib import Path
from typing import Dict, List, Optional

from google.auth.transport.requests import AuthorizedSession
from google.oauth2 import service_account

DEFAULT_GDRIVE_FOLDER_ID = "1O96AN1YzkmXz1BgRmHBioR3FS4CRTmBr"
DRIVE_SCOPES = ["https://www.googleapis.com/auth/drive"]
DRIVE_FILES_API = "https://www.googleapis.com/drive/v3/files"
DRIVE_UPLOAD_API = "https://www.googleapis.com/upload/drive/v3/files"

MIME_BY_SUFFIX = {
    ".apk": "application/vnd.android.package-archive",
    ".aab": "application/octet-stream",
}


def parse_service_account_json(raw_secret: str) -> dict:
    cleaned = raw_secret.strip()
    if not cleaned:
        raise ValueError("GDRIVE_SERVICE_ACCOUNT_JSON is empty.")

    if cleaned.startswith("{"):
        return json.loads(cleaned)

    # Also support Base64-encoded JSON secrets seamlessly
    decoded = base64.b64decode(cleaned).decode("utf-8").strip()
    return json.loads(decoded)


def sha256_hex(file_path: Path) -> str:
    h = hashlib.sha256()
    with file_path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def find_existing_file_in_folder(
    session: AuthorizedSession, folder_id: str, filename: str
) -> Optional[str]:
    escaped_name = filename.replace("'", "\\'")
    query = f"'{folder_id}' in parents and name = '{escaped_name}' and trashed = false"
    resp = session.get(
        DRIVE_FILES_API,
        params={
            "q": query,
            "fields": "files(id,name,size,owners(emailAddress))",
            "pageSize": 10,
            "supportsAllDrives": "true",
            "includeItemsFromAllDrives": "true",
        },
        timeout=30,
    )
    if resp.status_code != 200:
        raise RuntimeError(
            f"Drive list failed ({resp.status_code}): {resp.text}"
        )
    files = resp.json().get("files") or []
    if not files:
        return None
    return str(files[0]["id"])


def resumable_overwrite_existing_file(
    session: AuthorizedSession, file_id: str, local_path: Path, mime_type: str
) -> str:
    file_size = local_path.stat().st_size
    init_resp = session.patch(
        f"{DRIVE_UPLOAD_API}/{file_id}",
        params={
            "uploadType": "resumable",
            "supportsAllDrives": "true",
        },
        headers={
            "Content-Type": "application/json; charset=UTF-8",
            "X-Upload-Content-Type": mime_type,
            "X-Upload-Content-Length": str(file_size),
        },
        data=json.dumps({"name": local_path.name}),
        timeout=30,
    )
    if init_resp.status_code not in (200, 201):
        raise RuntimeError(
            f"Failed to initiate resumable update for file {file_id} "
            f"({init_resp.status_code}): {init_resp.text}"
        )

    upload_url = init_resp.headers.get("Location")
    if not upload_url:
        raise RuntimeError("Missing resumable upload Location header from Google Drive.")

    with local_path.open("rb") as f:
        put_resp = session.put(
            upload_url,
            headers={
                "Content-Type": mime_type,
                "Content-Length": str(file_size),
            },
            data=f,
            timeout=300,
        )

    if put_resp.status_code not in (200, 201):
        raise RuntimeError(
            f"Resumable upload PUT failed ({put_resp.status_code}): {put_resp.text}"
        )
    return str(put_resp.json().get("id", file_id))


def resumable_create_new_file(
    session: AuthorizedSession, folder_id: str, local_path: Path, mime_type: str
) -> str:
    file_size = local_path.stat().st_size
    init_resp = session.post(
        DRIVE_UPLOAD_API,
        params={
            "uploadType": "resumable",
            "supportsAllDrives": "true",
        },
        headers={
            "Content-Type": "application/json; charset=UTF-8",
            "X-Upload-Content-Type": mime_type,
            "X-Upload-Content-Length": str(file_size),
        },
        data=json.dumps({"name": local_path.name, "parents": [folder_id]}),
        timeout=30,
    )
    if init_resp.status_code not in (200, 201):
        if "storageQuotaExceeded" in init_resp.text:
            raise RuntimeError(
                f"Google 個人雲端硬碟限制服務帳戶建立新檔（storageQuotaExceeded）。"
                f"請先用您的個人 Google 帳號在「自碼系統」資料夾 (ID: {folder_id}) 內放置一個同名檔案 "
                f"「{local_path.name}」，之後 GitHub Actions 即可永久免配額原地覆寫更新！"
            )
        raise RuntimeError(
            f"Failed to initiate resumable create ({init_resp.status_code}): {init_resp.text}"
        )

    upload_url = init_resp.headers.get("Location")
    if not upload_url:
        raise RuntimeError("Missing resumable create Location header from Google Drive.")

    with local_path.open("rb") as f:
        put_resp = session.put(
            upload_url,
            headers={
                "Content-Type": mime_type,
                "Content-Length": str(file_size),
            },
            data=f,
            timeout=300,
        )

    if put_resp.status_code not in (200, 201):
        if "storageQuotaExceeded" in put_resp.text:
            raise RuntimeError(
                f"Google 個人雲端硬碟限制服務帳戶建立新檔（storageQuotaExceeded）。"
                f"請確認「自碼系統」資料夾 (ID: {folder_id}) 內已有您的個人帳號所擁有的「{local_path.name}」並已開啟編輯者共用給服務帳戶。"
            )
        raise RuntimeError(
            f"Resumable create PUT failed ({put_resp.status_code}): {put_resp.text}"
        )
    return str(put_resp.json()["id"])


def append_step_summary(lines: List[str]) -> None:
    summary_path = os.environ.get("GITHUB_STEP_SUMMARY")
    if not summary_path:
        return
    with Path(summary_path).open("a", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Overwrite packaged APK/AAB artifacts into Google Drive via Service Account."
    )
    parser.add_argument(
        "--artifact",
        action="append",
        required=True,
        help="Path to packaged artifact file (e.g., app/build/outputs/apk/release/自碼系統.apk)",
    )
    parser.add_argument(
        "--folder-id",
        default=os.environ.get("GDRIVE_FOLDER_ID", "").strip() or DEFAULT_GDRIVE_FOLDER_ID,
        help="Google Drive folder ID shared with the Service Account",
    )
    args = parser.parse_args()

    raw_sa = os.environ.get("GDRIVE_SERVICE_ACCOUNT_JSON", "")
    sa_info = parse_service_account_json(raw_sa)
    sa_email = sa_info.get("client_email", "unknown")

    creds = service_account.Credentials.from_service_account_info(
        sa_info, scopes=DRIVE_SCOPES
    )
    session = AuthorizedSession(creds)

    folder_id = args.folder_id
    summary_rows: List[str] = [
        "### Google 雲端硬碟自動同步結果",
        f"- **服務帳戶**: `{sa_email}`",
        f"- **目標資料夾 (`自碼系統`)**: https://drive.google.com/drive/folders/{folder_id}",
        "",
        "| 檔案名稱 | 大小 | SHA-256 (前 12 碼) | 同步模式 | Google Drive 連結 |",
        "| :--- | :--- | :--- | :--- | :--- |",
    ]

    for art_str in args.artifact:
        art_path = Path(art_str).resolve()
        if not art_path.is_file():
            raise FileNotFoundError(f"Packaged artifact not found: {art_path}")

        mime = MIME_BY_SUFFIX.get(art_path.suffix.lower(), "application/octet-stream")
        existing_id = find_existing_file_in_folder(session, folder_id, art_path.name)

        if existing_id:
            file_id = resumable_overwrite_existing_file(
                session, existing_id, art_path, mime
            )
            mode = "原地覆寫 (`PATCH`)"
        else:
            file_id = resumable_create_new_file(
                session, folder_id, art_path, mime
            )
            mode = "新建檔案 (`POST`)"

        size_mb = art_path.stat().st_size / (1024.0 * 1024.0)
        digest = sha256_hex(art_path)[:12]
        view_url = f"https://drive.google.com/file/d/{file_id}/view"
        print(
            f"[CI-GDrive] {mode} {art_path.name} ({size_mb:.2f} MB, sha256={digest}) -> {view_url}"
        )
        summary_rows.append(
            f"| `{art_path.name}` | `{size_mb:.2f} MB` | `{digest}` | {mode} | [開啟檔案]({view_url}) |"
        )

    append_step_summary(summary_rows)
    return 0


if __name__ == "__main__":
    sys.exit(main())
