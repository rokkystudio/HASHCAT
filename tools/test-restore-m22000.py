#!/usr/bin/env python3
"""Offline safety rehearsal for restore-m22000-after-test.sh (no ADB)."""

import argparse
import os
from pathlib import Path
import subprocess
import tempfile

MODULE_SIZE = 1061768
SOURCE_SIZE = 11524
MODULE_BACKUP = b"M" * MODULE_SIZE
SOURCE_BACKUP = b"S" * SOURCE_SIZE
EXPERIMENTAL = b"DO-NOT-KEEP: experimental app contents"


def msys_path(path):
    name = Path(path).resolve().as_posix()
    if len(name) > 2 and name[1] == ":":
        return "/" + name[0].lower() + name[2:]
    return name


def scenario(bash, source, root, kind):
    with tempfile.TemporaryDirectory(prefix="m22000-restore-", dir=root) as temp:
        case = Path(temp)
        app = case / "app"
        cache = app / "cache"
        runtime = app / "files/hashcat/runtime"
        module = runtime / "modules/module_22000.so"
        cl = runtime / "OpenCL/m22000-pure.cl"
        module_backup = cache / "module_22000.safe.bak"
        cl_backup = cache / "m22000-pure.safe.bak"
        for directory in (cache, module.parent, cl.parent):
            directory.mkdir(parents=True, exist_ok=True)
        module.write_bytes(EXPERIMENTAL)
        cl.write_bytes(EXPERIMENTAL)
        module_backup.write_bytes(MODULE_BACKUP)
        cl_backup.write_bytes(SOURCE_BACKUP)
        (runtime / "hc-safe-22000").write_bytes(b"test-executable")

        if kind == "no-module":
            module_backup.unlink()
        elif kind == "no-source":
            cl_backup.unlink()
        elif kind == "wrong-size":
            module_backup.write_bytes(b"unexpected backup")

        fixture = source.replace(
            "APP=/data/user/0/fuck.system.hashcat", "APP=" + msys_path(app), 1
        )
        assert fixture != source, "could not patch test-only APP path"
        script = case / "restore.sh"
        script.write_bytes(fixture.encode("utf-8"))

        bin_dir = case / "mockbin"
        bin_dir.mkdir()
        (bin_dir / "pidof").write_bytes(
            b'#!/bin/sh\n[ "${MOCK_ACTIVE:-0}" = "1" ]\n'
        )
        (bin_dir / "cp").write_bytes(
            b'#!/bin/sh\nif [ "${MOCK_PARTIAL:-0}" = "1" ]; then\n'
            b'  head -c 3 "$1" > "$2"\nelse\n  /usr/bin/cp "$@"\nfi\n'
        )
        for stub in (bin_dir / "pidof", bin_dir / "cp"):
            stub.chmod(0o755)
        env = os.environ.copy()
        env["MOCK_ACTIVE"] = "1" if kind == "running" else "0"
        env["MOCK_PARTIAL"] = "1" if kind == "partial-copy" else "0"
        cmd = f'export PATH="{msys_path(bin_dir)}:/usr/bin:/bin:$PATH"; /usr/bin/sh "{msys_path(script)}"'
        result = subprocess.run(
            [bash, "-c", cmd], env=env, capture_output=True, text=True, timeout=25
        )
        expected = {
            "success": (0, "RESTORE_VERIFIED:"),
            "running": (20, "RESTORE_REFUSED:"),
            "no-module": (21, "RESTORE_REFUSED:"),
            "no-source": (22, "RESTORE_REFUSED:"),
            "wrong-size": (25, "RESTORE_REFUSED:"),
            "partial-copy": (26, "RESTORE_FAILED:"),
        }
        code, text = expected[kind]
        assert result.returncode == code and text in result.stdout, (
            f"{kind}: got exit {result.returncode}, stdout={result.stdout!r}, "
            f"stderr={result.stderr!r}"
        )
        assert not (module.parent / "module_22000.so.restore-tmp").exists()
        assert not (cl.parent / "m22000-pure.cl.restore-tmp").exists()
        if kind == "success":
            assert module.read_bytes() == MODULE_BACKUP
            assert cl.read_bytes() == SOURCE_BACKUP
            assert not module_backup.exists() and not cl_backup.exists()
            assert not (runtime / "hc-safe-22000").exists()
        else:
            assert module.read_bytes() == EXPERIMENTAL
            assert cl.read_bytes() == EXPERIMENTAL
            assert (runtime / "hc-safe-22000").exists()
            if kind not in ("no-module", "no-source"):
                assert module_backup.exists() and cl_backup.exists()
        print(f"PASS {kind}: exit={result.returncode}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bash", default="bash")
    args = parser.parse_args()
    source = (Path(__file__).resolve().parent / "restore-m22000-after-test.sh").read_text(encoding="utf-8")
    root = Path(__file__).resolve().parents[1] / "build"
    root.mkdir(exist_ok=True)
    for kind in ("running", "no-module", "no-source", "wrong-size", "partial-copy", "success"):
        scenario(args.bash, source, root, kind)
    print("ALL RESTORE REHEARSALS PASSED (offline, no ADB)")


if __name__ == "__main__":
    main()
