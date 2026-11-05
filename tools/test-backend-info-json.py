#!/usr/bin/env python3
"""Validate the machine-readable backend description of hashcat.

The contract is emitted by ``hashcat --backend-info-json``:

    {
      "schema": 1,
      "hashcat_version": "v7.1.2",
      "backends": [
        {"id": "cuda", "detected": false, "schedulable": false, "devices": []},
        ... exactly five entries in the order cuda, hip, metal, opencl, vulkan
      ],
      "modes": {}
    }

The test runs the real executable, so it verifies the parser, the fixed key
order and the interaction with ``--backend-ignore-opencl`` at the same time.

Usage:
    python tools/test-backend-info-json.py
    python tools/test-backend-info-json.py --hashcat build/windows-x64/hashcat.exe
"""

import argparse
import json
import os
import re
import subprocess
import sys

BACKENDS = ["cuda", "hip", "metal", "opencl", "vulkan"]

BACKEND_KEYS = ["id", "detected", "schedulable", "devices"]

DEVICE_KEYS = ["id", "name", "type", "vendor_id", "backend"]

DEVICE_TYPES = {"GPU", "CPU", "FPGA", "ACCEL"}

VERSION_PATTERN = re.compile(r"^v\d+\.\d+\.\d+")


class Failure(Exception):
    """Raised when the output violates the contract."""


def run_hashcat(executable, arguments):
    completed = subprocess.run(
        [executable] + arguments,
        cwd=os.path.dirname(os.path.abspath(executable)),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )

    if completed.returncode != 0:
        raise Failure(
            "hashcat %s exited with %d: %s"
            % (" ".join(arguments), completed.returncode, completed.stderr.decode("utf-8", "replace").strip())
        )

    return completed.stdout


def parse_document(raw):
    text = raw.decode("utf-8")

    if not text.endswith("\n"):
        raise Failure("output must end with a newline")

    try:
        document = json.loads(text)
    except json.JSONDecodeError as error:
        raise Failure("output is not valid JSON: %s" % error)

    return document


def require_keys(mapping, keys, where):
    if list(mapping.keys()) != keys:
        raise Failure("%s key order is %s, expected %s" % (where, list(mapping.keys()), keys))


def validate_device(device, backend_id):
    if not isinstance(device, dict):
        raise Failure("%s device is not an object" % backend_id)

    require_keys(device, DEVICE_KEYS, "%s device" % backend_id)

    if not isinstance(device["id"], int) or isinstance(device["id"], bool) or device["id"] < 1:
        raise Failure("%s device id must be a positive integer, got %r" % (backend_id, device["id"]))

    if not isinstance(device["name"], str) or not device["name"]:
        raise Failure("%s device name must be a non-empty string" % backend_id)

    if device["type"] not in DEVICE_TYPES:
        raise Failure("%s device type %r is outside %s" % (backend_id, device["type"], sorted(DEVICE_TYPES)))

    if not isinstance(device["vendor_id"], int) or isinstance(device["vendor_id"], bool) or device["vendor_id"] < 0:
        raise Failure("%s device vendor_id must be an unsigned integer" % backend_id)

    if device["backend"] != backend_id:
        raise Failure("%s device reports backend %r" % (backend_id, device["backend"]))


def validate_document(document):
    if not isinstance(document, dict):
        raise Failure("top level value is not an object")

    require_keys(document, ["schema", "hashcat_version", "backends", "modes"], "top level")

    if document["schema"] != 1 or isinstance(document["schema"], bool):
        raise Failure("schema must be the number 1, got %r" % (document["schema"],))

    if not isinstance(document["hashcat_version"], str) or not VERSION_PATTERN.match(document["hashcat_version"]):
        raise Failure("hashcat_version %r does not look like a version" % (document["hashcat_version"],))

    backends = document["backends"]

    if not isinstance(backends, list) or len(backends) != len(BACKENDS):
        raise Failure("backends must contain exactly %d entries" % len(BACKENDS))

    for index, backend in enumerate(backends):
        expected_id = BACKENDS[index]

        if not isinstance(backend, dict):
            raise Failure("backend entry %d is not an object" % index)

        require_keys(backend, BACKEND_KEYS, "backend %s" % expected_id)

        if backend["id"] != expected_id:
            raise Failure("backend %d is %r, expected %r (fixed order)" % (index, backend["id"], expected_id))

        if not isinstance(backend["detected"], bool):
            raise Failure("backend %s detected must be a boolean" % expected_id)

        if not isinstance(backend["schedulable"], bool):
            raise Failure("backend %s schedulable must be a boolean" % expected_id)

        if not isinstance(backend["devices"], list):
            raise Failure("backend %s devices must be a list" % expected_id)

        if expected_id == "vulkan":
            if backend["schedulable"] is not False:
                raise Failure("vulkan must never be schedulable in schema 1")

            # Vulkan devices may be registered and listed since device
            # registration landed. They are validated by the shared device
            # loop below and must still never be schedulable.

        if backend["detected"] and not backend["devices"] and expected_id != "vulkan":
            raise Failure("backend %s is detected but lists no devices" % expected_id)

        for device in backend["devices"]:
            validate_device(device, expected_id)

    if document["modes"] != {}:
        raise Failure("modes is reserved in schema 1 and must be an empty object")

    return document


def find_backend(document, backend_id):
    for backend in document["backends"]:
        if backend["id"] == backend_id:
            return backend

    raise Failure("backend %s is missing" % backend_id)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--hashcat",
        default=os.path.join("build", "windows-x64", "hashcat.exe"),
        help="hashcat executable to test (default: build/windows-x64/hashcat.exe)",
    )

    arguments = parser.parse_args()

    executable = os.path.abspath(arguments.hashcat)

    if not os.path.isfile(executable):
        print("FAIL: hashcat executable not found: %s" % executable)
        return 1

    failures = 0

    try:
        document = validate_document(parse_document(run_hashcat(executable, ["--backend-info-json"])))

        print("PASS: --backend-info-json matches the contract")
        print("      version=%s" % document["hashcat_version"])

        for backend in document["backends"]:
            print(
                "      %-7s detected=%-5s schedulable=%-5s devices=%d"
                % (backend["id"], backend["detected"], backend["schedulable"], len(backend["devices"]))
            )
    except Failure as error:
        print("FAIL: %s" % error)
        failures += 1

    try:
        ignored = validate_document(
            parse_document(run_hashcat(executable, ["--backend-info-json", "--backend-ignore-opencl"]))
        )

        opencl = find_backend(ignored, "opencl")

        if opencl["detected"] is not False or opencl["schedulable"] is not False or opencl["devices"]:
            raise Failure("--backend-ignore-opencl must clear the opencl entry, got %r" % (opencl,))

        print("PASS: --backend-ignore-opencl clears the opencl entry")
    except Failure as error:
        print("FAIL: %s" % error)
        failures += 1

    if failures:
        return 1

    print("OK: backend JSON contract holds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
