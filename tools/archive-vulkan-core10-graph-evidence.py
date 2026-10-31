#!/usr/bin/env python3
"""Archive original GPU and native-C Vulkan 1.0 dynamic graph evidence.

Copies the exact LF-only observed records from three Android ABI targets
without modifying the device output bytes. Records exact source and build
hashes and the historical real process-specific Vulkan heap surveys.

Does not call ADB, Vulkan APIs, web services, hashcat or cryptographic code.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/diagnostic-multimode/vulkan-dynamic-graph-20261031"
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-dynamic-graph-20261031"

DEVICES = {
    "redmagic-arm64": "arm64-v8a",
    "redmagic-armv7": "armeabi-v7a",
    "pixel-arm64": "arm64-v8a",
}
MODES = ("default10", "explicit10", "required11", "noncoherent10")
OPTIMIZATIONS = ("O0", "O")

SOURCES = (
    "tools/vulkan-core10-graph-memory.h",
    "tools/vulkan-core10-graph-memory.c",
    "tools/vulkan-core10-graph.h",
    "tools/vulkan-core10-graph.c",
    "tools/vulkan-core10-graph-diagnostic.c",
    "tools/test-vulkan-core10-graph-memory-host.c",
    "tools/build-vulkan-core10-graph.ps1",
    "tools/run-vulkan-core10-graph.ps1",
    "tools/archive-vulkan-core10-graph-evidence.py",
    "tools/test-vulkan-core10-graph-evidence.py",
    "tools/vulkan-auto-config.c",
    "tools/vulkan-auto-config.h",
    "tools/vulkan-core11-policy.c",
    "tools/vulkan-core11-policy.h",
    "tools/vulkan-memory-heap-policy.h",
    "tools/vulkan-pipeline-stage-a.comp",
    "tools/vulkan-pipeline-stage-b.comp",
    "tools/vulkan-pipeline-stage-c.comp",
)

ARTIFACTS = (
    "vulkan-dynamic-graph-arm64",
    "vulkan-dynamic-graph-armv7",
    "vulkan-graph-host-O0.exe",
    "vulkan-graph-host-O2.exe",
    *(
        f"stage-{stage}-{mode}-vk1.0.spv"
        for mode in OPTIMIZATIONS
        for stage in ("a", "b", "c")
    ),
)


def sha256(path: Path) -> str:
    """Calculate the exact SHA-256 digest of one local file."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Hash canonical source bytes independent of Windows CRLF checkout."""
    assert path.is_file(), path

    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def original(name: str) -> str:
    """Copy original device data without any byte or line-ending changes."""
    data = (BUILD / name).read_bytes()

    assert data.endswith(b"\n") and b"\r" not in data, name
    assert b"91809037" not in data, name
    assert b"46281FDAS002AL" not in data, name

    destination = ARCHIVE / name
    shutil.copyfile(BUILD / name, destination)

    assert destination.read_bytes() == data

    return sha256(destination)


def main() -> None:
    """Record all GPU, fault, invalid command and host-C observations."""
    ARCHIVE.mkdir(parents=True, exist_ok=True)

    device_runs = []
    fault_runs = []
    invalid_runs = []
    native_runs = []

    for device, abi in DEVICES.items():
        for stages in (1, 2, 3):
            for mode in MODES:
                for opt in OPTIMIZATIONS:
                    name = f"{device}-stage{stages}-{mode}-{opt}.log"
                    digest = original(name)
                    status = int(
                        device.startswith("redmagic")
                        and mode == "noncoherent10"
                    )
                    rows = (ARCHIVE / name).read_text(
                        encoding="utf-8"
                    ).splitlines()

                    assert rows[:6] == [
                        "TEST=vulkan-core10-dynamic-graph",
                        f"STAGES={stages}",
                        f"MODE={mode}",
                        f"OPTIMIZATION={opt}",
                        f"ABI={abi}",
                        f"ADB_EXIT_CODE={status}",
                    ]

                    device_runs.append({
                        "device": device,
                        "abi": abi,
                        "stages": stages,
                        "mode": mode,
                        "optimization": opt,
                        "file": name,
                        "exit_code": status,
                        "sha256": digest,
                    })

        name = f"{device}-faults.log"
        digest = original(name)

        assert sum(
            line.startswith("FAULT_CASE ")
            for line in (ARCHIVE / name).read_text(
                encoding="utf-8"
            ).splitlines()
        ) == 72

        fault_runs.append({
            "device": device,
            "abi": abi,
            "file": name,
            "sha256": digest,
            "fault_points": 72,
        })

        name = f"{device}-negative-controls.log"
        digest = original(name)

        invalid_runs.append({
            "device": device,
            "abi": abi,
            "file": name,
            "sha256": digest,
            "invalid_cases": 9,
        })

    for optimization in ("O0", "O2"):
        name = f"vulkan-graph-host-{optimization}.log"
        native_runs.append({
            "optimization": optimization,
            "file": name,
            "sha256": original(name),
            "exit_code": 0,
        })

    heap_profiles = {
        "redmagic-arm64": (
            "tests/fixtures/android-gpu/vulkan-heap-20261031/"
            "redmagic-allocation.log"
        ),
        "redmagic-armv7": (
            "tests/fixtures/android-gpu/vulkan-core-baseline-20261031/"
            "redmagic-armv7-memory-properties.log"
        ),
        "pixel-arm64": (
            "tests/fixtures/android-gpu/vulkan-heap-20261031/"
            "pixel-allocation.log"
        ),
    }

    manifest = {
        "date": "2026-10-31",
        "scope": (
            "variable-size public synthetic Vulkan 1.0 Compute graph, "
            "one to three GPU stages with capability-driven memory"
        ),
        "source_hash_policy": "UTF-8 with CRLF normalized to LF",
        "android_minimum_api": 24,
        "compiled_abis": ["arm64-v8a", "armeabi-v7a"],
        "physical_vulkan10_only_device_tested": False,
        "device_stage_topology": {
            str(stages): {
                "buffers": stages + 3,
                "descriptors": stages + 1,
                "pipelines": stages,
                "mapped_staging": 2,
                "acquisition_points": 16 + stages * 4,
                "verified_words_per_submission": (
                    12288 if stages == 3 else 2048
                ),
            }
            for stages in (1, 2, 3)
        },
        "submissions_per_run": 4,
        "shader_optimizations": list(OPTIMIZATIONS),
        "modes": list(MODES),
        "source_sha256": {
            name: source_sha256(ROOT / name)
            for name in SOURCES
        },
        "artifact_sha256": {
            name: sha256(BUILD / name)
            for name in ARTIFACTS
        },
        "heap_reference_sha256": {
            device: {
                "file": path,
                "sha256": sha256(ROOT / path),
            }
            for device, path in heap_profiles.items()
        },
        "device_runs": device_runs,
        "fault_runs": fault_runs,
        "invalid_runs": invalid_runs,
        "native_runs": native_runs,
        "limitations": [
            "Tests run under a core Vulkan 1.0 instance on physically newer Vulkan 1.1/1.4 GPUs; physical Vulkan-1.0-only devices were unavailable.",
            "Executables target Android API 24, not a tested Android 7.0 phone.",
            "This graph handles any caller-supplied nonzero number of buffers, storage descriptors and Compute pipeline stages, but these three public shaders specifically exercise stage counts one to three.",
            "Descriptor binding and shader interfaces are explicit contracts; no SPIR-V reflection or automatic algorithm transformation occurs.",
            "Stage A/B GPU state is read through the final shader output in the three-stage case; one- and two-stage layouts verify their direct last-stage output only.",
            "Each buffer uses independent VkDeviceMemory, not packed suballocations.",
            "Resource acquisition fault injection occurs after a real Vulkan object is created, not by forcing an actual device-loss/driver allocation failure.",
            "Resource cleanup counts are application-level evidence, not a validation-layer proof against driver-internal leaks.",
            "This is standalone noncryptographic Vulkan infrastructure, not a hashcat backend or a bcrypt implementation.",
        ],
    }

    (ARCHIVE / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n",
        encoding="utf-8",
        newline="\n",
    )

    assert len(list(ARCHIVE.glob("*.log"))) == 80

    print(
        "PASS original 60 real GPU success logs, "
        "12 incompatible staging memory rejections"
    )
    print("PASS 216 real Vulkan resource acquisition fault cases")
    print("PASS 27 invalid CLI controls and two host-C memory planner runs")
    print("PASS exact sources", len(SOURCES), "artifacts", len(ARTIFACTS))
    print("MANIFEST_SHA256", sha256(ARCHIVE / "manifest.json"))


if __name__ == "__main__":
    main()
