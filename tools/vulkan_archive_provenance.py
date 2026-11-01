#!/usr/bin/env python3
"""Verify archived Vulkan evidence using its last committed manifest revision.

Android GPU transcripts are tied to the source files identified by
the exact saved manifest. When the production HASHCAT backend gains
additional Vulkan functionality, older regression tests inspect
source blobs from the most recent commit containing that manifest.
A manifest may have been updated after its initial introduction.
The file on disk must match the Git blob from that exact revision;
missing revisions or source objects are explicit verification errors.
"""

from __future__ import annotations

import hashlib
from pathlib import Path
import subprocess


def source_sha256_at_archive_commit(
    root: Path,
    archive_manifest: Path,
    source_path: Path,
) -> str:
    """Hash source recorded alongside the last committed archive manifest.

    Locates the last Git commit that includes a content update to the
    manifest, confirms that the checked-out manifest bytes match the
    corresponding committed blob, and reads the requested source from
    that same commit. The original evidence writer's CRLF-to-LF source
    normalization is preserved. A missing or inconsistent Git object
    raises an error without substituting newer source code.
    """
    root = root.resolve(strict=True)
    manifest = archive_manifest.relative_to(root).as_posix()
    source = source_path.relative_to(root).as_posix()

    revision = subprocess.run(
        [
            "git", "log", "-1", "--format=%H", "--", manifest,
        ],
        cwd=root, check=True, capture_output=True,
    ).stdout.decode("ascii").strip()

    if len(revision) != 40:
        raise RuntimeError(
            f"The archived manifest has no committed Git revision: {manifest}"
        )

    saved_manifest = subprocess.run(
        ["git", "show", f"{revision}:{manifest}"],
        cwd=root, check=True, capture_output=True,
    ).stdout

    if archive_manifest.read_bytes() != saved_manifest:
        raise RuntimeError(
            f"The archive manifest differs from its committed Git revision: {manifest}"
        )

    content = subprocess.run(
        ["git", "show", f"{revision}:{source}"],
        cwd=root, check=True, capture_output=True,
    ).stdout

    return hashlib.sha256(
        content.replace(b"\r\n", b"\n")
    ).hexdigest()
