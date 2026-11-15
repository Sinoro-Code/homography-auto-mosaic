#!/usr/bin/env python3
"""
Dual-Track Project Sync Utility:
  1. Source Code -> Pushed exclusively to GitHub (`origin/main`).
  2. Packaged Artifacts (`自碼系統.apk`, `*.aab`, etc.) -> Synced exclusively into
     a dedicated Google Drive folder (`自碼系統` by default), updating existing
     artifacts in-place (`gdrive mutate update`) and migrating any loose root
     `自碼系統.apk` into the folder automatically.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Dict, List, Optional

DEFAULT_DRIVE_FOLDER_NAME = "自碼系統"
DEFAULT_APK_NAME = "自碼系統.apk"
FOLDER_MIME = "application/vnd.google-apps.folder"
ID_REGEX = re.compile(r"\(ID:\s*([A-Za-z0-9_-]+)\)")

ARTIFACT_MIME_MAP = {
    ".apk": "application/vnd.android.package-archive",
    ".aab": "application/octet-stream",
    ".idsig": "application/octet-stream",
}


def detect_artifact_mime(path: Path) -> str:
    return ARTIFACT_MIME_MAP.get(path.suffix.lower(), "application/octet-stream")


def run_cmd(cmd: List[str], cwd: Path, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        cwd=str(cwd),
        text=True,
        capture_output=True,
        check=check,
    )


def extract_id_from_output(stdout: str) -> str:
    match = ID_REGEX.search(stdout)
    if not match:
        raise RuntimeError(f"Unable to parse Drive ID from output: {stdout.strip()}")
    return match.group(1)


def is_folder_item(item: dict) -> bool:
    return (
        item.get("mimeType") == FOLDER_MIME
        or str(item.get("type", "")).lower() == "folder"
    )


def resolve_or_create_drive_folder(
    folder_name: str, explicit_folder_id: Optional[str], cwd: Path
) -> str:
    if explicit_folder_id:
        return explicit_folder_id

    res = run_cmd(
        ["gdrive", "readonly", "search", "--name-exact", folder_name, "--type", "folder", "--json"],
        cwd=cwd,
    )
    items = json.loads(res.stdout.strip() or "[]") or []
    for item in items:
        if item.get("name") == folder_name and is_folder_item(item):
            return str(item["id"])

    mk = run_cmd(["gdrive", "mutate", "mkdir", folder_name], cwd=cwd)
    return extract_id_from_output(mk.stdout)


def list_folder_files(folder_id: str, cwd: Path) -> Dict[str, str]:
    res = run_cmd(["gdrive", "readonly", "ls", folder_id, "--json"], cwd=cwd)
    items = json.loads(res.stdout.strip() or "[]") or []
    out: Dict[str, str] = {}
    for item in items:
        if not is_folder_item(item) and item.get("name") and item.get("id"):
            out[str(item["name"])] = str(item["id"])
    return out


def migrate_loose_root_artifact_if_present(
    artifact_name: str, target_folder_id: str, existing_in_folder: Dict[str, str], cwd: Path
) -> None:
    res = run_cmd(
        ["gdrive", "readonly", "search", "--name-exact", artifact_name, "--parent-id", "root", "--json"],
        cwd=cwd,
    )
    items = json.loads(res.stdout.strip() or "[]") or []
    for item in items:
        if is_folder_item(item) or item.get("name") != artifact_name:
            continue
        loose_id = str(item["id"])
        if loose_id == target_folder_id:
            continue
        if artifact_name not in existing_in_folder:
            run_cmd(["gdrive", "mutate", "mv", loose_id, target_folder_id], cwd=cwd)
            existing_in_folder[artifact_name] = loose_id
        else:
            run_cmd(["gdrive", "mutate", "trash", loose_id], cwd=cwd)


def collect_default_artifacts(repo_root: Path) -> List[Path]:
    artifacts: List[Path] = []
    release_apk = repo_root / "app" / "build" / "outputs" / "apk" / "release" / DEFAULT_APK_NAME
    if release_apk.is_file():
        artifacts.append(release_apk)

    bundle_dir = repo_root / "app" / "build" / "outputs" / "bundle" / "release"
    if bundle_dir.is_dir():
        for aab in sorted(bundle_dir.glob("*.aab")):
            artifacts.append(aab)

    return artifacts


def sync_artifacts_to_gdrive_folder(
    repo_root: Path,
    artifacts: List[Path],
    folder_name: str,
    explicit_folder_id: Optional[str] = None,
) -> Tuple_Result:
    folder_id = resolve_or_create_drive_folder(folder_name, explicit_folder_id, repo_root)
    existing_files = list_folder_files(folder_id, repo_root)

    synced_ids: Dict[str, str] = {}
    for artifact in artifacts:
        if not artifact.is_file():
            continue
        migrate_loose_root_artifact_if_present(
            artifact.name, folder_id, existing_files, repo_root
        )
        mime = detect_artifact_mime(artifact)
        if artifact.name in existing_files:
            file_id = existing_files[artifact.name]
            run_cmd(
                ["gdrive", "mutate", "update", file_id, str(artifact), "--mime-type", mime],
                cwd=repo_root,
            )
            synced_ids[artifact.name] = file_id
        else:
            up = run_cmd(
                [
                    "gdrive",
                    "mutate",
                    "upload",
                    str(artifact),
                    "--parent",
                    folder_id,
                    "--mime-type",
                    mime,
                ],
                cwd=repo_root,
            )
            synced_ids[artifact.name] = extract_id_from_output(up.stdout)

    return folder_id, synced_ids


Tuple_Result = tuple[str, Dict[str, str]]


def install_pre_push_hook(repo_root: Path, folder_name: str, folder_id: str) -> Path:
    hooks_dir = repo_root / ".git" / "hooks"
    hooks_dir.mkdir(parents=True, exist_ok=True)
    hook_path = hooks_dir / "pre-push"
    id_flag = f' --drive-folder-id "{folder_id}"' if folder_id else ""
    hook_script = f"""#!/usr/bin/env bash
# Auto-generated by scripts/sync_github_and_gdrive.py
# Source code pushes to GitHub; packaged artifacts (APK/AAB) sync into the Google Drive folder `{folder_name}`.
set -euo pipefail

if [ "${{FAM_SKIP_GDRIVE_HOOK:-0}}" = "1" ]; then
    exit 0
fi

REPO_ROOT="$(git rev-parse --show-toplevel)"
APK_FILE="$REPO_ROOT/app/build/outputs/apk/release/{DEFAULT_APK_NAME}"

if [ -f "$APK_FILE" ] && (command -v gdrive >/dev/null 2>&1 || [ -n "${{GDRIVE_RCLONE_DEST:-}}" ]); then
    echo "[pre-push] Syncing packaged artifacts to Google Drive folder '{folder_name}'..."
    python3 "$REPO_ROOT/scripts/sync_github_and_gdrive.py" --skip-git-push --drive-folder-name "{folder_name}"{id_flag}
fi
"""
    hook_path.write_text(hook_script, encoding="utf-8")
    hook_path.chmod(0o755)
    return hook_path


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Push source code to GitHub and sync packaged artifacts (APK/AAB) into a Google Drive folder."
    )
    parser.add_argument("--remote", default="origin", help="Git remote name (default: origin)")
    parser.add_argument("--branch", default="", help="Git branch to push (default: current HEAD)")
    parser.add_argument(
        "--artifact",
        action="append",
        default=[],
        help="Explicit packaged artifact path(s) to sync (defaults to release 自碼系統.apk / *.aab)",
    )
    parser.add_argument(
        "--drive-folder-name",
        default=os.environ.get("FAM_GDRIVE_FOLDER_NAME", DEFAULT_DRIVE_FOLDER_NAME),
        help=f"Target Google Drive folder name for packaged artifacts (default: {DEFAULT_DRIVE_FOLDER_NAME})",
    )
    parser.add_argument(
        "--drive-folder-id",
        default=os.environ.get("FAM_GDRIVE_FOLDER_ID", ""),
        help="Optional explicit Google Drive folder ID",
    )
    parser.add_argument(
        "--skip-git-push",
        action="store_true",
        help="Only sync packaged artifacts to the Google Drive folder without running git push",
    )
    parser.add_argument(
        "--install-hook",
        action="store_true",
        help="Install .git/hooks/pre-push for automatic packaged-artifact sync",
    )
    args = parser.parse_args()

    repo_root = Path(
        run_cmd(["git", "rev-parse", "--show-toplevel"], cwd=Path.cwd()).stdout.strip()
    )

    if args.install_hook:
        hook_file = install_pre_push_hook(
            repo_root, args.drive_folder_name, args.drive_folder_id
        )
        print(f"[Hook] Installed dual-track pre-push hook at: {hook_file}")

    if not args.skip_git_push:
        branch = args.branch or run_cmd(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=repo_root
        ).stdout.strip()
        print(f"[GitHub] Pushing source code branch '{branch}' to '{args.remote}'...")
        env = os.environ.copy()
        env["FAM_SKIP_GDRIVE_HOOK"] = "1"
        subprocess.run(
            ["git", "push", "-u", args.remote, branch],
            cwd=str(repo_root),
            env=env,
            check=True,
        )
        print("[GitHub] Source code push complete.")

    artifacts = (
        [Path(p).resolve() for p in args.artifact]
        if args.artifact
        else collect_default_artifacts(repo_root)
    )
    if not artifacts:
        print(
            "[GDrive] No packaged artifacts found under app/build/outputs/; build `:app:assembleRelease` first.",
            file=sys.stderr,
        )
        return 1

    if shutil.which("gdrive"):
        folder_id, synced = sync_artifacts_to_gdrive_folder(
            repo_root=repo_root,
            artifacts=artifacts,
            folder_name=args.drive_folder_name,
            explicit_folder_id=args.drive_folder_id or None,
        )
        folder_url = f"https://drive.google.com/drive/folders/{folder_id}"
        print(f"[GDrive] Synced {len(synced)} packaged artifact(s) into folder '{args.drive_folder_name}' -> {folder_url}")
        for name, fid in synced.items():
            print(f"  - {name}: https://drive.google.com/file/d/{fid}/view")
        return 0

    rclone_dest = os.environ.get("GDRIVE_RCLONE_DEST", "")
    if rclone_dest and shutil.which("rclone"):
        target_dir = f"{rclone_dest.rstrip('/')}/{args.drive_folder_name}"
        for art in artifacts:
            subprocess.run(
                ["rclone", "copyto", str(art), f"{target_dir}/{art.name}"],
                check=True,
            )
        print(f"[GDrive/rclone] Synced {len(artifacts)} artifact(s) to {target_dir}")
        return 0

    print(
        "ERROR: Neither 'gdrive' CLI nor 'GDRIVE_RCLONE_DEST' + 'rclone' was found.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
