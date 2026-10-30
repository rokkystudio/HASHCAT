# RedMagic Adreno WPA 22000: EAPOL and mixed-hash correctness

The RedMagic NX709S (ADB serial `91809037`) executes a split-OpenCL
mode-22000 build: the PBKDF2 init/loop/comp entrypoints are in one
`cl_program`, and the WPA auxiliary verification kernels in another.
Both programs retain their original kernel bodies.

## Synthetic reference vectors

`python tools/generate-m22000-eapol-vectors.py` generates and verifies
three deterministic `WPA*02` records. The generator uses standard
Python PBKDF2/HMAC and PyCryptodome AES-CMAC, not the GPU under test.
The synthetic SSID, MAC addresses, nonces and EAPOL frames are not
captured network traffic.

| WPA type | Auxiliary kernel | Reference verification |
|---|---|---|
| `WPA*01` PMKID | `aux4` | HMAC-SHA1 over `PMK Name` and both MAC addresses |
| `WPA*02`, keyver 1 | `aux1` | HMAC-MD5 EAPOL MIC |
| `WPA*02`, keyver 2 | `aux2` | HMAC-SHA1 EAPOL MIC, first 16 bytes |
| `WPA*02`, keyver 3 | `aux3` | AES-CMAC EAPOL MIC |

All vectors use the synthetic passphrase `hashcat!`; the EAPOL key
confirmation keys are derived independently from a 4096-round
PBKDF2-HMAC-SHA1 PMK. The EAPOL MIC field is zero in the input message
before its reference MIC is computed.

## Results on the physical Adreno GPU

Each of the three EAPOL versions was tested separately against both
`hashcat!` (correct) and `hashcat2` (incorrect).

- All three positive checks reported `Status: Cracked`,
  `Recovered: 1/1`, `Candidate.Engine.: Device Generator`,
  and device exit code **0**. The recovered plaintext matched exactly.
- All three negative checks reported `Status: Exhausted`,
  `Recovered: 0/1`, the wrong candidate, and device exit code **1**.
- PMKID had already passed the same positive and negative controls.

In a separate **mixed-hash test**, a single file included all four
hash types at once. With `hashcat!`, one GPU invocation reported
`Recovered: 4/4` and returned code 0; the recovered file contained
exactly four copies of the expected passphrase. With `hashcat2`,
the same four records returned `Recovered: 0/4`, `Status: Exhausted`,
and the expected code 1. This confirms that the split program
dispatches all four verification paths within one mixed workload
for these synthetic vectors.

The runs used isolated application-private staging. The normal
`module_22000.so` and `m22000-pure.cl` were not replaced, and
their verified stock SHA-256 values remained:

- Module: `abff75bd2ad3dc4c4818419b500bf07e19340a28e4c506f024c11cc43bf0aaf0`.
- OpenCL source: `8ab35e16fa2055ba36939507ccac0bbcd5e52d9205d3d635417921f12582b428`.

The original logs, exit markers, input records and mixed recovered output
are available under `tests/fixtures/android-gpu/redmagic-eapol-vectors/`
and `tests/fixtures/android-gpu/redmagic-mixed-wpa/`.

## Offline regression verification

```powershell
python tools/test-android-gpu-known-vectors.py
python tools/test-redmagic-mixed-wpa.py
```

These read-only checks validate the fixture records against independently
computed cryptographic primitives, require the saved GPU statuses and
exit markers, and compare the mixed-batch output with the four
individually verified inputs. The EAPOL reference uses PyCryptodome
for AES-CMAC.

## Limits of this conclusion

These are **controlled synthetic tests** with one SSID, one passphrase,
fixed MAC/nonce ordering and a small EAPOL-Key frame. They do not cover
every message-pair value, corrupted MIC, nonce correction, packet
capture import, password length or device/driver version. In particular,
the Pixel 9 Pro XL Mali-G715 still fails to compile mode 22000 and
heavy SHA-512 modes in the existing driver environment.

The evidence supports the tested Adreno PMKID/EAPOL verification paths;
it does **not** establish release readiness or full WPA compatibility.
