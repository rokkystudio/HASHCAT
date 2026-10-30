# Pixel 9 Pro XL GPU candidate verification

The Pixel 9 Pro XL uses the Mali-G715 GPU and a standalone Android ARM64
hashcat runtime staged under `/data/local/tmp/hc-pixel-hashcat-20261030`.
The checks below were run against *synthetic* digests only and did not
install or modify an Android application.

## Reference password and hash modes

The known password is `benchdemo2026`. Python `hashlib` generated the
MD5 (mode 0), SHA-1 (100), and SHA-256 (1400) digests; PyCryptodome MD4
over the UTF-16LE password generated the NTLM digest (1000).
Original successful GPU results are archived under
`tests/fixtures/android-gpu/Pixel_9_Pro_XL-known-vectors-20261030/`.

## Wrong-candidate rejection

`tools/run-pixel-negative-vectors.sh` runs exactly one candidate,
`benchdemo2027`, against each synthetic reference digest.
The input candidate differs from `benchdemo2026` while preserving its
13-character length. All four modes returned the expected **hashcat exit
code 1**, `Status: Exhausted`, `Recovered: 0/1`, and no cracked result.
The *test harness* returned 0, confirming that all four rejection checks
passed. Original logs, status markers, and exit codes are archived under
`tests/fixtures/android-gpu/Pixel_9_Pro_XL-negative-vectors-20261030/`.

## Candidate selection from a wordlist

`tools/run-pixel-multiple-candidates.sh` tests a three-line wordlist:

```text
benchdemo2027
benchdemo2026
benchdemo2025
```

For modes 0, 100, 1000 and 1400, hashcat ran a straight wordlist attack
(`-a 0`) with `-w 1` and returned **exit 0**, `Status: Cracked`,
`Recovered: 1/1`, and exactly one recovered plaintext:
`benchdemo2026`. The device-side wrapper and ADB invocation also
returned 0. The original logs, recovered records, and exit markers
are archived under
`tests/fixtures/android-gpu/Pixel_9_Pro_XL-multiple-candidates-20261030/`.

## Reproducible evidence check

```powershell
python tools/test-pixel-gpu-candidates.py
```

This read-only validator independently calculates the digests,
compares the archived positive reference, negative control, and
three-candidate outcomes, and requires the expected hashcat status,
exit code and GPU evidence. It requires PyCryptodome for NTLM MD4.

The temporary input and execution directories were deleted from Pixel
**after** their logs were archived. The pre-existing standalone hashcat
test runtime remains available and unchanged.

These observations confirm correct matching and rejection for these
specific synthetic vectors and attack paths. They do not certify
all password lengths, salts, rules, workloads, or Mali kernel variants.
In particular, modes 1700, 1800, and 22000 still require separate work
around the Pixel Mali OpenCL compiler failures.
