# RedMagic NX709S selected GPU mode verification (2026-10-30)

## Scope and isolation

This report compares exact bounded hashcat known vectors in modes `500` (md5crypt), `3200` (bcrypt), and `5600` (NetNTLMv2) on RedMagic NX709S / Qualcomm Adreno OpenCL 3.0 against Pixel 9 Pro XL / Mali-G715 results from `docs/pixel-gpu-selected-modes.md`.

The tested ARM64 frontend SHA-256 is `4ab04450ab7357f97e18f0e66497dd4776c4300fb7412e4b88eb8713a8ebbad9`. The script `tools/run-redmagic-selected-known-vector.sh` executes the staged frontend, module, and mode-specific OpenCL sources in an isolated app-cache directory. Shared runtime resources are read through symlinks. The standard app executable, modules, and OpenCL files are not overwritten. The test refuses a pre-existing hashcat process, stale test stage, or outstanding mode-22000 recovery backup.

All cases use one deliberately bounded synthetic/self-test record and candidate `hashcat`, mask attack (`-a 3`), workload `-w 1`, and normal hashcat device self-tests. No `--self-test-disable` option is used.

## Results

| Mode | RedMagic kernel path | Adreno result | Pixel comparison |
| --- | --- | --- | --- |
| `500` md5crypt | optimized (`-O`) | **PASS**; `Cracked`, `Recovered: 1/1`, candidate `hashcat`, exit **0** | Optimized kernel PASS; pure kernel crashed during Mali compilation |
| `3200` bcrypt | pure | **FAIL**; OpenCL kernel self-test failed, hashcat aborted, exit **245** | Pure kernel PASS, exit 0 |
| `5600` NetNTLMv2 | pure | **FAIL**; `clLinkProgram(): CL_OUT_OF_HOST_MEMORY`, `m05600_a3-pure.cl`, exit **251** | Pure kernel PASS, exit 0 |
| `5600` NetNTLMv2 | optimized (`-O`) | **PASS**; `Cracked`, `Recovered: 1/1`, candidate `hashcat`, exit **0** | The Pixel evidence covers the pure kernel; no optimized Pixel outcome is claimed |

The NetNTLMv2 Adreno failure is a program-link failure for a specific pure kernel. It is not evidence that NetNTLMv2 is unsupported on that GPU: the separate optimized GPU path completed its built-in self-test and recovered the expected plaintext. The Qualcomm `CL_OUT_OF_HOST_MEMORY` status alone does not establish actual physical host RAM exhaustion.

The bcrypt result is a **self-test failure**, not a wrong-password result or successful recovery. No attempt was made to bypass the self-test. The module and kernel SHA-256 values match the Pixel case, which indicates that a change in these exact staged files does not explain the differing result. The cause within the Adreno kernel execution, driver, workgroup configuration, or local-memory use is not yet established.

## Inputs, checks, and evidence

The directory `tests/fixtures/android-gpu/redmagic-selected-modes-20261030/` contains all three exact input hashes, raw device logs, exit-code files, plaintext output for both successes, and `manifest.json` recording the tested module and OpenCL-file hashes. Failure cases intentionally have no recovered-plaintext file.

`tools/test-redmagic-selected-modes.py` requires:
- identical mode input hashes across both devices;
- independent CPU calculations for the md5crypt and NetNTLMv2 references;
- bcrypt test input matching `module_03200.c`'s public self-test hash and password;
- correct `Cracked` status, GPU kernel feature, exact recovered `hashcat`, and process exit 0 for modes 500 optimized and 5600 optimized;
- exact bcrypt self-test failure/exit 245 and NetNTLMv2 pure-kernel link failure/exit 251;
- verified Pixel success of pure bcrypt and NetNTLMv2 and matching staged module/OpenCL hashes between devices.

On the host, run:

```text
python tools/test-redmagic-selected-modes.py
```

Expected final line: `REDMAGIC THREE MODE CROSS-DEVICE EVIDENCE VERIFIED`.

The bcrypt input is a public hashcat self-test vector, not an independently generated bcrypt reference. Both successful Adreno cases test one short password, not all candidate lengths, salts, attack modes, or GPU workgroup sizes.

## Reproducing and following up

The runner expects an explicitly staged `/data/local/tmp/hc-redmagic-selected-input/` tree with the ARM64 `hashcat`, `module_XXXXX.so`, exact `mXXXXX.hash`, and only matching mode-specific OpenCL sources under `OpenCL/`. The runner itself must be present at `/data/local/tmp/run-redmagic-selected-known-vector.sh`, and it must be invoked via `run-as fuck.system.hashcat` on **NX709S / serial 91809037**. It refuses unexpected mode inputs or nonmatching kernel variants. Logs and result markers are first retained in the app cache, copied to the archive, and validated before temporary test files are cleaned.

Next diagnostics should determine why the Adreno bcrypt self-test fails under the module's `FIXED_LOCAL_SIZE` and Blowfish local-memory configuration. Test only isolated variants; require a successful built-in self-test and full known-vector recovery before marking a variant as supported. Also add positive and negative candidate controls for optimized NetNTLMv2 before inferring broader correctness.

These results do not qualify the app's entire mode list for release.
