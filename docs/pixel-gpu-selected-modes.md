# Pixel 9 Pro XL additional GPU mode verification (2026-10-30)

## Scope

This report records isolated GPU checks for hashcat modes `500` (md5crypt), `3200` (bcrypt) and `5600` (NetNTLMv2) on Pixel 9 Pro XL / Mali-G715. The tests use the current local Android arm64 runtime under `/data/local/tmp`; they do not install or modify an Android application. The tested frontend SHA-256 is `4ab04450ab7357f97e18f0e66497dd4776c4300fb7412e4b88eb8713a8ebbad9`.

The remote module and OpenCL-source SHA-256 values were read back from Pixel and matched the local `build/arm64-v8a` files byte-for-byte by digest. Exact values are retained in `tests/fixtures/android-gpu/Pixel_9_Pro_XL-selected-modes-20261030/manifest.json`.

## Results

| Mode | Kernel path | Result | Qualification |
| --- | --- | --- | --- |
| `500` md5crypt | pure, `-w 1` | **Mali compiler SIGABRT / exit 134** | Android crash buffer: `Scudo ERROR: internal map failure (error desc=Out of memory)`, `llvm::createValhallPreRAMachineScheduler`, `clcc::Kernel::build_kernel`, `clLinkProgram` |
| `500` md5crypt | optimized (`-O`), `-w 1` | **PASS**, exact plaintext `hashcat`, exit 0 | `Status: Cracked`, `Recovered: 1/1`, `Candidate.Engine: Device Generator`; `$1$38652870$DUjsu4TTlTsOe/xxZ05uf/` independently reproduced with OpenSSL and the offline Python md5crypt implementation |
| `3200` bcrypt | pure, `-w 1` | **PASS**, exact plaintext `hashcat`, exit 0 | Public `module_03200.c` self-test vector; no independent bcrypt verifier was installed on the Windows test host |
| `5600` NetNTLMv2 | pure, `-w 1` | **PASS**, exact plaintext `hashcat`, exit 0 | Public `module_05600.c` self-test vector; an attempted independent NetNTLMv2 recomputation through the remote execution environment was blocked, so no external-reference claim is made |

Mode `500` demonstrates that a Mali failure can be kernel-path specific: its pure OpenCL program aborts during compiler backend scheduling, while the optimized kernel for the same input completes and returns the correct plaintext. Therefore the pure-kernel crash must not be generalized to the entire hash mode.

Modes `3200` and `5600` both completed through their pure kernels. These are actual one-candidate GPU recovery checks rather than `-b` benchmarks, but they cover one deliberately bounded self-test vector each, not every input length or attack mode.

## Reproducibility

Raw synthetic hash inputs, successful device logs, exit markers, recovered plaintexts and the mode-500 pure-kernel Android crash block are committed under `tests/fixtures/android-gpu/Pixel_9_Pro_XL-selected-modes-20261030/`.

Run:

```text
python tools/test-pixel-selected-modes.py
```

The validator recomputes the md5crypt reference independently, requires exact plaintext recovery and completed GPU status for all successful modes, and checks the retained Mali compiler stack for the pure mode-500 failure. The bcrypt and NetNTLMv2 hashes remain explicitly identified as hashcat public self-test vectors rather than independently generated references.

## Device cleanup and remaining work

The test runtime is shell-owned under `/data/local/tmp`; no Pixel application files are involved. After evidence is committed, the temporary current-runtime tree and selected-vector inputs can be removed without affecting the older retained diagnostic runtime.

RedMagic NX709S was not available in ADB during this selected-mode pass, so modes `500`, `3200` and `5600` were not re-run there in this section. Existing Adreno results for other modes remain unchanged. The next cross-device step is to execute the same bounded vectors on RedMagic when serial `91809037` is available again.
