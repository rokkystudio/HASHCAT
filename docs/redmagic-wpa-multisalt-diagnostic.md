# RedMagic WPA 22000: four independent salts on Adreno

## Scope

The established mode-22000 GPU checks cover PMKID (AUX4) and EAPOL
key versions 1, 2 and 3 (AUX1, AUX2, AUX3) with a single SSID.
See `docs/redmagic-wpa-eapol-verification.md` for those results.

This follow-up verifies all four paths in **one GPU workload with four
different SSIDs**, so PBKDF2 state and MIC verification must operate
on distinct salts, not merely distinct digests for a shared salt.

## Synthetic CPU reference vectors

`python tools/generate-m22000-multisalt-vectors.py` creates four
invented WPA records with independent SSIDs and MAC pairs. Every record
uses the deliberately synthetic password `hashcat!`.

| Record | SSID | CPU-derived PMKID or MIC |
| --- | --- | --- |
| `WPA*01`, PMKID/AUX4 | `lab-pmkid-01` | `faa014ed4f0b8e08888b32f9f8f390e6` |
| `WPA*02`, EAPOL/AUX1 | `lab-eapol-md5` | `02fd0d6593c8b5f9997f5889221e58f3` |
| `WPA*02`, EAPOL/AUX2 | `lab-eapol-sha1` | `ea01931ed410124810d20aafad597365` |
| `WPA*02`, EAPOL/AUX3 | `lab-eapol-cmac` | `ec540149e4a3efc486cf63c71f754329` |

The generator derives a unique PMK per SSID using PBKDF2-HMAC-SHA1
(4096 iterations), then recomputes PMKID with HMAC-SHA1 or EAPOL
MIC using HMAC-MD5, truncated HMAC-SHA1 or AES-CMAC. It does not
call the GPU implementation. These MACs, nonces and EAPOL frames
are generated test data, **not captured wireless traffic**.

`python tools/test-m22000-multisalt-reference.py` passed all
**12 reference cases**: four correct MIC/PMKID values, four
wrong-passphrase rejections, and four intentionally corrupted
MIC/PMKID rejections.

## Completed physical Adreno GPU verification (2026-10-30)

Target: RedMagic NX709S, ADB serial `91809037`, Qualcomm Adreno
OpenCL 3.0. The separate
`tools/run-redmagic-multisalt-wpa.sh` runtime stages a hashcat
frontend, module and OpenCL source in application-private temporary
cache; it reads but does not replace any stock application files.

The positive case (`hashcat!`) completed on-device with exit
**0**, `Status: Cracked`, **`Recovered: 4/4 (100.00%)` digests**,
and **`4/4 (100.00%)` salts**. The log explicitly reports
`Hashes: 4 digests; 4 unique digests, 4 unique salts` and
`Candidate.Engine.: Device Generator`. The recovered file contains
exactly four lines of `hashcat!`.

During initial GPU compilation, the *host* ADB shell lost its
connection and returned **255**. This did **not** mean the GPU
calculation failed. After the phone reconnected, the on-device exit
marker, complete crack log and recovered output were read and
preserved **as raw bytes**. Hashcat had continued independently
and completed successfully. The saved device exit marker is 0.

The negative case used exactly the same four hashes and the
incorrect candidate `hashcat2`. This produced on-device exit
**1** (expected for an exhausted keyspace), `Status: Exhausted`,
`Recovered: 0/4 (0.00%)`, no recovered-output file and the exact
`Guess.Mask: hashcat2 [8]`. The isolated runner reported
`MULTISALT_WPA_NEGATIVE_PASS mode=22000 exit=1`, and the
*host* ADB invocation returned **0**.

All four CPU reference records, the untouched raw GPU logs, exit
markers and four recovered plaintexts are in
`tests/fixtures/android-gpu/redmagic-multisalt-wpa/`.

The complete offline regression is:

```powershell
python tools/test-m22000-multisalt-reference.py
python tools/test-redmagic-multisalt-wpa.py
```

The second command validates the independent reference cryptography,
the four-salt log facts, both on-device exit codes and positive/negative
recovered outputs. Both checks passed.

## Device cleanup and runtime integrity

After archiving the positive run, removed its precisely named
application-private cache stage, log, marker and recovered copy.
The completed negative run removed its own stage; its log and
marker were archived then removed. After checking there was no
running hashcat process, removed only the temporary
`/data/local/tmp/hc-multisalt-wpa-input` and staged runner.
ADB confirmed both phones connected again.

The installed RedMagic WPA runtime files retained their
original SHA-256 digests:

- `module_22000.so`:
  `abff75bd2ad3dc4c4818419b500bf07e19340a28e4c506f024c11cc43bf0aaf0`
- `m22000-pure.cl`:
  `8ab35e16fa2055ba36939507ccac0bbcd5e52d9205d3d635417921f12582b428`

## Remaining coverage

This confirms **four distinct WPA salts, one correct password and one
wrong password** through all four AUX paths in one GPU workload. It
does not prove all SSID lengths, MIC corruption handling on GPU,
multiple passwords in a dictionary, message-pair subtypes, nonce
correction or all real-world capture imports. The Pixel 9 Pro XL
Mali-G715 compilation failure for mode 22000 remains a separate
device-driver issue. No release-readiness claim follows from the
bounded synthetic result.
