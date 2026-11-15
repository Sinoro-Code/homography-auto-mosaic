#!/usr/bin/env python3
"""
Dual-Sync Utility: Synchronizes the Git repository to GitHub AND mirrors all
tracked source files + a clean HEAD zip archive to Google Drive idempotently.

Supports:
  1. Official `gdrive` CLI (`gdrive readonly` / `gdrive mutate` update-in-place)
  2. `rclone` fallback (`GDRIVE_RCLONE_DEST`, e.g., `gdrive:homography-auto-mosaic`)
  3. Optional Git `pre-push` hook installation (`--install-hook`) so every
     `git push` automatically mirrors the repository to Google Drive.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Dict, List, Optional, Tuple

FOLDER_MIME = "application/vnd.google-apps.folder"
ID_REGEX = re.compile(r"\(ID:\s*([A-Za-z0-9_-]+)\)")

TEXT_PLAIN_SUFFIXES = {
    ".kt",
    ".kts",
    ".cpp",
    ".hpp",
    ".c",
    ".h",
    ".cmake",
    ".pro",
    ".toml",
    ".properties",
    ".sh",
    ".py",
    ".txt",
    ".gitignore",
}


def detect_mime_type(path: Path) -> str:
    if path.name == ".gitignore" or path.name == "CMakeLists.txt":
        return "text/plain"
    ext = path.suffix.lower()
    if ext == ".md":
        return "text/markdown"
    if ext == ".xml":
        return "application/xml"
    if ext == ".json":
        return "application/json"
    if ext == ".zip":
        return "application/zip"
    if ext in TEXT_PLAIN_SUFFIXES:
        return "text/plain"
    return "application/octet-stream"


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


def list_drive_children(folder_id: str, cwd: Path) -> Dict[str, dict]:
    res = run_cmd(["gdrive", "readonly", "ls", folder_id, "--json"], cwd=cwd)
    raw = res.stdout.strip()
    items = json.loads(raw) if raw else []
    if not items:
        return {}
    by_name: Dict[str, dict] = {}
    for item in items:
        name = item.get("name")
        if name and name not in by_name:
            by_name[name] = item
    return by_name


def resolve_or_create_root_drive_folder(
    folder_name: str, explicit_id: Optional[str], cwd: Path
) -> str:
    if explicit_id:
        return explicit_id

    res = run_cmd(
        ["gdrive", "readonly", "search", "--name-contains", folder_name, "--json"],
        cwd=cwd,
    )
    items = json.loads(res.stdout.strip() or "[]") or []
    for item in items:
        if item.get("name") == folder_name and is_folder_item(item):
            return str(item["id"])

    mk = run_cmd(["gdrive", "mutate", "mkdir", folder_name], cwd=cwd)
    return extract_id_from_output(mk.stdout)


def sync_via_gdrive_cli(
    repo_root: Path,
    tracked_files: List[str],
    drive_folder_name: str,
    explicit_folder_id: Optional[str],
    workers: int,
) -> Tuple[str, int, int]:
    root_id = resolve_or_create_root_drive_folder(
        drive_folder_name, explicit_folder_id, repo_root
    )
    print(f"[GDrive] Target root folder: {drive_folder_name} (ID: {root_id})")

    # 1. Collect all unique relative directories in parent-first order
    rel_dirs = sorted(
        {
            str(Path(rel).parent)
            for rel in tracked_files
            if str(Path(rel).parent) not in ("", ".")
        },
        key=lambda p: (len(Path(p).parts), p),
    )

    dir_id_map: Dict[str, str] = {"": root_id, ".": root_id}
    children_cache: Dict[str, Dict[str, dict]] = {
        root_id: list_drive_children(root_id, repo_root)
    }

    for rel_dir in rel_dirs:
        parts = Path(rel_dir).parts
        parent_rel = str(Path(*parts[:-1])) if len(parts) > 1 else ""
        leaf_name = parts[-1]
        parent_id = dir_id_map[parent_rel]

        if parent_id not in children_cache:
            children_cache[parent_id] = list_drive_children(parent_id, repo_root)

        existing = children_cache[parent_id].get(leaf_name)
        if existing and is_folder_item(existing):
            sub_id = str(existing["id"])
        else:
            mk = run_cmd(
                ["gdrive", "mutate", "mkdir", leaf_name, "--parent", parent_id],
                cwd=repo_root,
            )
            sub_id = extract_id_from_output(mk.stdout)
            children_cache[sub_id] = {}

        dir_id_map[rel_dir] = sub_id
        if sub_id not in children_cache:
            children_cache[sub_id] = list_drive_children(sub_id, repo_root)

    # 2. Build upload/update task list for all tracked files + clean HEAD zip snapshot
    upload_tasks: List[Tuple[Path, str, Optional[str], str]] = []
    for rel in tracked_files:
        local_file = repo_root / rel
        if not local_file.is_file():
            continue
        parent_rel = str(Path(rel).parent)
        if parent_rel == ".":
            parent_rel = ""
        parent_id = dir_id_map[parent_rel]
        existing_entry = children_cache[parent_id].get(local_file.name)
        existing_file_id = (
            str(existing_entry["id"])
            if (existing_entry and not is_folder_item(existing_entry))
            else None
        )
        upload_tasks.append(
            (local_file, parent_id, existing_file_id, detect_mime_type(local_file))
        )

    with tempfile.TemporaryDirectory() as tmpdir:
        zip_name = f"{drive_folder_name}-source.zip"
        zip_path = Path(tmpdir) / zip_name
        run_cmd(
            [
                "git",
                "archive",
                "--format=zip",
                f"--prefix={drive_folder_name}/",
                "HEAD",
                "-o",
                str(zip_path),
            ],
            cwd=repo_root,
        )
        existing_zip = children_cache[root_id].get(zip_name)
        existing_zip_id = (
            str(existing_zip["id"])
            if (existing_zip and not is_folder_item(existing_zip))
            else None
        )
        upload_tasks.append((zip_path, root_id, existing_zip_id, "application/zip"))

        created_count = 0
        updated_count = 0

        def _sync_one(
            task: Tuple[Path, str, Optional[str], str]
        ) -> Tuple[str, bool]:
            src_path, parent_id, existing_id, mime = task
            if existing_id:
                run_cmd(
                    [
                        "gdrive",
                        "mutate",
                        "update",
                        existing_id,
                        str(src_path),
                        "--mime-type",
                        mime,
                    ],
                    cwd=repo_root,
                )
                return (src_path.name, False)
            else:
                run_cmd(
                    [
                        "gdrive",
                        "mutate",
                        "upload",
                        str(src_path),
                        "--parent",
                        parent_id,
                        "--mime-type",
                        mime,
                    ],
                    cwd=repo_root,
                )
                return (src_path.name, True)

        with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
            futures = [pool.submit(_sync_one, t) for t in upload_tasks]
            for fut in concurrent.futures.as_completed(futures):
                _, is_created = fut.result()
                if is_created:
                    created_count += 1
                else:
                    updated_count += 1

    return (root_id, created_count, updated_count)


def install_pre_push_hook(repo_root: Path, drive_folder_id: Optional[str]) -> Path:
    hooks_dir = repo_root / ".git" / "hooks"
    hooks_dir.mkdir(parents=True, exist_ok=True)
    hook_path = hooks_dir / "pre-push"
    folder_arg = f' --drive-folder-id "{drive_folder_id}"' if drive_folder_id else ""
    hook_script = f"""#!/usr/bin/env bash
# Auto-generated by scripts/sync_github_and_gdrive.py
# Automatically mirrors tracked repository files to Google Drive whenever `git push` runs.
set -euo pipefail

if [ "${{FAM_SKIP_GDRIVE_HOOK:-0}}" = "1" ]; then
    exit 0
fi

REPO_ROOT="$(git rev-parse --show-toplevel)"
if command -v gdrive >/dev/null 2>&1 || [ -n "${{GDRIVE_RCLONE_DEST:-}}" ]; then
    echo "[pre-push] Mirroring repository to Google Drive..."
    python3 "$REPO_ROOT/scripts/sync_github_and_gdrive.py" --skip-git-push{folder_arg}
else
    echo "[pre-push] Skipping Google Drive mirror (neither 'gdrive' CLI nor GDRIVE_RCLONE_DEST configured)."
fi
"""
    hook_path.write_text(hook_script, encoding="utf-8")
    hook_path.chmod(0o755)
    return hook_path


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Synchronize repository to GitHub and Google Drive simultaneously."
    )
    parser.add_argument("--remote", default="origin", help="Git remote name (default: origin)")
    parser.add_argument("--branch", default="", help="Git branch to push (default: current HEAD)")
    parser.add_argument(
        "--drive-folder-name",
        default="homography-auto-mosaic",
        help="Target folder name in Google Drive",
    )
    parser.add_argument(
        "--drive-folder-id",
        default=os.environ.get("FAM_GDRIVE_FOLDER_ID", ""),
        help="Explicit Google Drive folder ID (optional)",
    )
    parser.add_argument(
        "--skip-git-push",
        action="store_true",
        help="Only mirror tracked files to Google Drive without running git push",
    )
    parser.add_argument(
        "--install-hook",
        action="store_true",
        help="Install .git/hooks/pre-push so `git push` automatically syncs Google Drive",
    )
    parser.add_argument(
        "--workers",
        type=int,
        default=6,
        help="Concurrent Drive upload workers (default: 6)",
    )
    args = parser.parse_args()

    repo_root = Path(
        run_cmd(["git", "rev-parse", "--show-toplevel"], cwd=Path.cwd()).stdout.strip()
    )

    if args.install_hook:
        hook_file = install_pre_push_hook(
            repo_root, args.drive_folder_id or None
        )
        print(f"[Hook] Installed automatic dual-sync Git hook at: {hook_file}")

    if not args.skip_git_push:
        branch = args.branch or run_cmd(
            ["git", "rev-parse", "--abbrev-ref", "HEAD"], cwd=repo_root
        ).stdout.strip()
        print(f"[GitHub] Pushing branch '{branch}' to remote '{args.remote}'...")
        env = os.environ.copy()
        env["FAM_SKIP_GDRIVE_HOOK"] = "1"
        subprocess.run(
            ["git", "push", "-u", args.remote, branch],
            cwd=str(repo_root),
            env=env,
            check=True,
        )
        print("[GitHub] Push complete.")

    tracked = [
        line.strip()
        for line in run_cmd(["git", "ls-files"], cwd=repo_root).stdout.splitlines()
        if line.strip()
    ]

    if shutil.which("gdrive"):
        folder_id, created, updated = sync_via_gdrive_cli(
            repo_root=repo_root,
            tracked_files=tracked,
            drive_folder_name=args.drive_folder_name,
            explicit_folder_id=args.drive_folder_id or None,
            workers=max(1, args.workers),
        )
        drive_url = f"https://drive.google.com/drive/folders/{folder_id}"
        print(
            f"[GDrive] Sync complete -> {drive_url} "
            f"(created={created}, updated={updated}, total={created + updated})"
        )
        return 0

    rclone_dest = os.environ.get("GDRIVE_RCLONE_DEST", "")
    if rclone_dest and shutil.which("rclone"):
        print(f"[GDrive/rclone] Syncing tracked repository to {rclone_dest}...")
        with tempfile.TemporaryDirectory() as stage_dir:
            stage_path = Path(stage_dir)
            for rel in tracked:
                src = repo_root / rel
                if src.is_file():
                    dst = stage_path / rel
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(src, dst)
            subprocess.run(["rclone", "sync", str(stage_path), rclone_dest], check=True)
        print("[GDrive/rclone] Sync complete.")
        return 0

    print(
        "ERROR: Neither 'gdrive' CLI nor 'GDRIVE_RCLONE_DEST' + 'rclone' was found.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
