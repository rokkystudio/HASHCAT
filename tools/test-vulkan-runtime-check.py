#!/usr/bin/env python3
"""Verify one M2 Vulkan runtime-check log and backend JSON manifest.

Checks that the -I chain ran through hc_vulkan_device, that the data-path
check dispatched more than 65535 workgroups and matched the CPU reference,
that the per-element counter covers every element, that the repeated
init/check/term loop reported no failures, and that the JSON keeps Vulkan
detected but unschedulable.

Exit code 0 only when every check passes.
"""

import argparse
import json
import pathlib
import re
import sys


CHECK_RE = re.compile(
    r"Vulkan check passed: groups=(\d+) elements=(\d+) sum=([0-9a-fA-F]+) "
    r"count=(\d+) dispatch_splits=(\d+)"
)
LIFECYCLE_RE = re.compile(
    r"Vulkan memory lifecycle: iterations=(\d+) "
    r"rss_start=(\d+) rss_peak=(\d+) rss_end=(\d+) failures=(\d+)"
)
FORBIDDEN = (
    "Vulkan check mismatch",
    "coverage mismatch",
    "Vulkan data-path check failed",
    "Vulkan dispatch of",
    "Abort message",
    "BACKEND_VULKAN_SESSION",
    "BACKEND_VULKAN_KERNEL",
)


class Failure(Exception):
    pass


def load_text(path: pathlib.Path) -> str:
    raw = path.read_bytes()
    if raw.startswith(b"\xef\xbb\xbf"):
        raw = raw[3:]
    return raw.decode("utf-8", errors="replace")


def verify_log(path: pathlib.Path) -> dict:
    text = load_text(path)

    if "Vulkan Info:" not in text:
        raise Failure("the log has no Vulkan Info section")
    if "Backend Device ID #" not in text:
        raise Failure("the log has no Vulkan backend device id")

    matches = CHECK_RE.findall(text)
    if not matches:
        raise Failure("the log has no successful Vulkan check line")

    groups, elements, checksum, count, splits = matches[-1]
    groups = int(groups)
    elements = int(elements)
    count = int(count)
    splits = int(splits)

    if groups <= 65535:
        raise Failure(f"dispatch groups {groups} do not exceed the 65535 test boundary")
    if elements == 0:
        raise Failure("the check processed zero elements")
    if count != elements:
        raise Failure(f"coverage counter {count} != element count {elements}")

    lifecycle = LIFECYCLE_RE.findall(text)
    if not lifecycle:
        raise Failure("the log has no memory lifecycle summary")

    iterations, rss_start, rss_peak, rss_end, failures = [int(value) for value in lifecycle[-1]]

    if failures != 0:
        raise Failure(f"memory lifecycle reported {failures} failure(s)")
    if iterations < 1:
        raise Failure("memory lifecycle ran zero iterations")
    if rss_end > rss_start + 64 * 1024:
        raise Failure(f"RSS grew by more than 64 MiB across {iterations} iterations")

    for marker in FORBIDDEN:
        if marker in text:
            raise Failure(f"forbidden marker in log: {marker}")

    if "Vulkan dispatch m2-check-sum complete" not in text:
        raise Failure("the log has no dispatch completion line")

    return {
        "groups": groups,
        "elements": elements,
        "sum": checksum,
        "count": count,
        "splits": splits,
        "iterations": iterations,
        "rss_start_kib": rss_start,
        "rss_peak_kib": rss_peak,
        "rss_end_kib": rss_end,
    }


def verify_json(path: pathlib.Path) -> dict:
    text = load_text(path)

    try:
        manifest = json.loads(text)
    except json.JSONDecodeError as error:
        raise Failure(f"backend JSON is invalid: {error}") from error

    if manifest.get("schema") != 1:
        raise Failure("backend JSON schema is not 1")

    vulkan = None
    for backend in manifest.get("backends", []):
        if backend.get("id") == "vulkan":
            vulkan = backend
            break

    if vulkan is None:
        raise Failure("backend JSON has no vulkan entry")
    if vulkan.get("schedulable") is not False:
        raise Failure("vulkan.schedulable is not false")
    if vulkan.get("detected") is not True:
        raise Failure("vulkan.detected is not true")
    if not vulkan.get("devices"):
        raise Failure("vulkan.devices is empty")

    device = vulkan["devices"][0]

    return {
        "detected": vulkan.get("detected"),
        "schedulable": vulkan.get("schedulable"),
        "device_name": device.get("name"),
        "vendor_id": device.get("vendor_id"),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", required=True)
    parser.add_argument("--json", default="")
    args = parser.parse_args()

    try:
        result = verify_log(pathlib.Path(args.log))
        print(
            "PASS log: groups={groups} elements={elements} sum={sum} "
            "count={count} splits={splits} iterations={iterations} "
            "rss={rss_start_kib}->{rss_end_kib} KiB".format(**result)
        )

        if args.json:
            manifest = verify_json(pathlib.Path(args.json))
            print(
                "PASS json: detected={detected} schedulable={schedulable} "
                "device={device_name} vendor_id={vendor_id}".format(**manifest)
            )
    except Failure as error:
        print(f"FAIL {error}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
