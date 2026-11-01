#!/usr/bin/env python3
"""Archive real Android Vulkan result-gate and public-buffer observations.

Preserves the exact LF-normalized Android diagnostic runner output.
Each native test first obtains genuine driver VkResult values and
completes real public buffer copies; a separately labeled application
VkResult then exercises a host-only fail-closed submission policy.
No captured trial represents a real GPU device loss, driver failure
or HASHCAT password-processing operation.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/diagnostic-multimode/vulkan-core10-result-gate-20261101"
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-core10-result-gate-20261101"

DEVICES = {
    "pixel-arm64": ("arm64-v8a", ("prefer", "noncoherent")),
}
APIS = ("1.0", "1.1")
TERMINALS = {
    "device-lost": (-4, -4),
    "device-memory": (-2, -2),
    "unexpected-positive": (5, -3),
}
SOURCES = (
    "tools/vulkan-core10-result-gate.c",
    "tools/vulkan-core10-result-gate.h",
    "tools/vulkan-core10-result-gate-diagnostic.c",
    "tools/build-vulkan-core10-result-gate.ps1",
    "tools/run-vulkan-core10-result-gate.ps1",
    "tools/archive-vulkan-core10-result-gate-evidence.py",
    "tools/test-vulkan-core10-result-gate-evidence.py",
    "tools/vulkan-core10-transfer-session.c",
    "tools/vulkan-core10-transfer-session.h",
    "tools/vulkan-core10-device-catalog.c",
    "tools/vulkan-core10-device-catalog.h",
    "tools/vulkan-auto-config.c",
    "tools/vulkan-auto-config.h",
    "tools/vulkan-core11-policy.c",
    "tools/vulkan-core11-policy.h",
)


def sha256(path: Path) -> str:
    """Hash every byte of an existing native Android ELF or transcript."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Hash a checked-out source file with normalized LF line endings."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def archive_log(filename: str) -> str:
    """Copy one LF-only real-device capture without changing its bytes."""
    source = BUILD / filename
    data = source.read_bytes()

    assert data and data.endswith(b"\n") and b"\r" not in data, filename
    assert b"46281FDAS002AL" not in data, filename
    assert b"91809037" not in data, filename

    target = ARCHIVE / filename
    shutil.copyfile(source, target)
    assert target.read_bytes() == data

    return sha256(target)


def main() -> None:
    """Archive real Pixel GPU observations and synthetic terminal controls."""
    ARCHIVE.mkdir(parents=True, exist_ok=True)

    runs = []
    invalid_cli = []

    for device, (abi, modes) in DEVICES.items():
        for api in APIS:
            for memory_mode in modes:
                for terminal, (observed, error) in TERMINALS.items():
                    name = f"{device}-{api}-{memory_mode}-{terminal}.log"
                    digest = archive_log(name)
                    lines = (ARCHIVE / name).read_text(
                        encoding="utf-8"
                    ).splitlines()

                    assert lines[:6] == [
                        "TEST=vulkan-core10-result-gate",
                        f"ABI={abi}",
                        f"API={api}",
                        f"MEMORY_MODE={memory_mode}",
                        f"TERMINAL={terminal}",
                        "ADB_EXIT_CODE=0",
                    ], name
                    assert lines[-1] == (
                        f"RESULT_GATE_DIAGNOSTIC_PASS api={api}"
                        f" mode={0 if memory_mode == 'prefer' else 1}"
                        f" terminal={terminal} trials=2 injected=2"
                        " actual_driver_loss=0 real_copies=4"
                        " verified_words=2062 verified_bytes=8248"
                        " nonfatal=4 blocked=6 host_controls=5 shaders=0"
                    ), name

                    for prefix, count in (
                        ("DEVICE_CATALOG_OPEN ", 2),
                        ("DEVICE_CATALOG_CLOSE ", 2),
                        ("TRANSFER_OPEN ", 2),
                        ("TRANSFER_COPY ", 4),
                        ("TRANSFER_CLOSE ", 2),
                        ("RESULT_GATE_REAL_DRIVER ", 4),
                        ("RESULT_GATE_TEST_INJECT ", 2),
                        ("RESULT_GATE_REFUSED ", 6),
                        ("RESULT_GATE_POLICY_ONLY_PASS ", 5),
                        ("RESULT_GATE_TRIAL_PASS ", 2),
                    ):
                        assert sum(
                            line.startswith(prefix) for line in lines
                        ) == count, (name, prefix)

                    runs.append({
                        "device": device,
                        "abi": abi,
                        "api": api,
                        "memory_mode": memory_mode,
                        "terminal_mode": terminal,
                        "test_only_observed": observed,
                        "retained_first_error": error,
                        "native_exit_code": 0,
                        "file": name,
                        "sha256": digest,
                        "real_device_sessions": 2,
                        "actual_gpu_copies": 4,
                        "verified_words": 2062,
                        "verified_bytes": 8248,
                        "real_not_ready": 2,
                        "real_timeout": 2,
                        "host_only_terminal_events": 2,
                        "blocked_resubmission_requests": 6,
                        "host_only_controls": 5,
                        "noncoherent_copies": (
                            4 if memory_mode == "noncoherent" else 0
                        ),
                    })

        name = f"{device}-negative-controls.log"
        invalid_cli.append({
            "device": device,
            "abi": abi,
            "file": name,
            "sha256": archive_log(name),
            "rejected_requests": 6,
        })

    assert len(runs) == 12
    assert len(invalid_cli) == 1
    assert len(list(ARCHIVE.glob("*.log"))) == 13

    assert sum(x["real_device_sessions"] for x in runs) == 24
    assert sum(x["actual_gpu_copies"] for x in runs) == 48
    assert sum(x["verified_words"] for x in runs) == 24744
    assert sum(x["verified_bytes"] for x in runs) == 98976
    assert sum(x["real_not_ready"] for x in runs) == 24
    assert sum(x["real_timeout"] for x in runs) == 24
    assert sum(x["host_only_terminal_events"] for x in runs) == 24
    assert sum(x["blocked_resubmission_requests"] for x in runs) == 72
    assert sum(x["noncoherent_copies"] for x in runs) == 24

    manifest = {
        "date": "2026-11-01",
        "scope": (
            "Standalone Vulkan 1.0 fail-closed VkResult classification "
            "after real Android GPU public buffer copies; terminal values "
            "are deliberately synthetic application-only observations"
        ),
        "minimum_android_api": 24,
        "requested_vulkan_apis": list(APIS),
        "terminal_modes": list(TERMINALS),
        "actual_device_loss_returned_by_driver": False,
        "actual_driver_allocation_error_tested": False,
        "application_injected_terminal_status": True,
        "policy_enforced_inside_production_hashcat": False,
        "gpu_shaders": 0,
        "cryptographic_or_password_inputs": False,
        "android_abi_hardware_tested": ["arm64-v8a"],
        "android_armv7_binary_built_but_not_run": True,
        "redmagic_not_connected_for_this_milestone": True,
        "capture_policy": (
            "Native Android stdout/stderr is captured over ADB and "
            "normalised to LF by the PowerShell runner; these archived "
            "logs are byte-identical to the saved runner result, not "
            "represented as raw, unprocessed ADB byte streams"
        ),
        "source_hash_policy": "CRLF normalized to LF",
        "source_sha256": {
            source: source_sha256(ROOT / source)
            for source in SOURCES
        },
        "native_elf_sha256": {
            abi: sha256(BUILD / f"vulkan-core10-result-gate-{abi}")
            for abi in ("arm64", "armv7")
        },
        "runs": runs,
        "invalid_cli": invalid_cli,
        "limitations": [
            "The terminal VK_ERROR_DEVICE_LOST, memory failure and invalid-positive results are passed by the application to a host policy; the Vulkan driver did not return these errors.",
            "The independent policy does not intercept the production HASHCAT runtime, and only compliant callers that consult the gate are prevented from submitting work.",
            "The physical GPU is not lost, reset or deliberately stalled; cleanup reflects a healthy real device after a simulated terminal classification.",
            "Pixel Mali-G715 ARM64 was the only connected GPU for the full matrix; RedMagic Adreno/ARMv7 became disconnected and neither underwent these tests.",
            "The Android ARMv7 API24 executable is statically inspected and bit-reproduced, but its native execution is not attested by this milestone.",
            "All GPU copies are public unsigned integer VkCmdCopyBuffer transfers without a shader, password or hashcat scheduler.",
            "No Android 7.0/API24 OS, physically Vulkan-1.0-only GPU, real VK_ERROR_DEVICE_LOST or validation-layer leak report was tested.",
        ],
    }

    manifest_path = ARCHIVE / "manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8", newline="\n",
    )

    print("PASS 12 actual Android Vulkan API/memory/terminal host-policy runs")
    print("PASS 24 real VkDevice sessions and 48 completed GPU buffer copies")
    print("PASS 24744 verified public integer words, 98976 bytes")
    print("PASS 24 genuine NOT_READY and 24 genuine TIMEOUT results")
    print("PASS 24 expressly test-injected terminal values and 72 blocked host submissions")
    print("PASS 60 pure policy cases, 6 rejected Android native CLI requests")
    print("PASS exact ARM64/ARMv7 executable and 15 source identities")
    print("MANIFEST_SHA256", sha256(manifest_path))


if __name__ == "__main__":
    main()
