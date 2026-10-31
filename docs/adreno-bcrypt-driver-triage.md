# Qualcomm Adreno bcrypt (mode 3200): driver triage report

**Status:** Open. The original bcrypt OpenCL self-test fails on RedMagic. The first incorrect GPU stage has not been identified. This is a source-grounded issue description using the publicly documented hashcat test vector.

## Environment and source identity

| Item | Observed value |
| --- | --- |
| Device | RedMagic NX709S, Android 13 |
| GPU | QUALCOMM Adreno(TM), OpenCL 3.0 |
| Reported OpenCL build | commit #21eaafda81, changeid #I578ccc16f0, dated 2022-07-19 |
| User-space hashcat | v7.1.2 |
| Mode | 3200 (bcrypt), pure kernel |
| Stock m03200-pure.cl SHA-256 | b4d6a2bf96a0c8326a4bcf930a7dbb6e51e2ae2fa6b1087b0c29e360cf8317c4 |
| Stock module_03200.so SHA-256 | 76418b1ab9cd0cf18873c386766988c5a7b3e001f8072477ba138790f91bb2fe |
| Stock Git blob SHA-1 | fcade9f0cbee1259d56019b7b5cb200b89fc3ef9; byte-identical to official hashcat master when checked |
| Public vector | tests/fixtures/android-gpu/redmagic-selected-modes-20261030/m03200.hash |

The date embedded in the Qualcomm runtime build string is 2022-07-19; it is not necessarily the date of the installed OS image or of a particular device bug.

## Device firmware and driver provenance (read-only audit, 2026-10-31)

The connected NX709S identifies as the REDMAGIC **7S Pro**. Android reports `ro.product.model=NX709S`, `ro.product.name=NX709S-UN`, and `ro.product.locale=zh-CN`. The exact regional firmware channel is **not established** by those values alone.

| On-device field | Observed value |
| --- | --- |
| System Android release | 13 |
| System build incremental | eng.nubia.20241204.165456 |
| Vendor Android release | 12 |
| Vendor build incremental | eng.nubia.20241204.170923 |
| System/vendor security patch | 2024-11-01 for both |
| /vendor/lib64/libOpenCL.so SHA-256 | 10f08bc7b8df69587447c2b8514230341867f8aac127b277654d73333227b713 |
| /vendor/lib64/libllvm-qcom.so SHA-256 | 3325d6d70c6142facfe0ae5977ac2918b1433f61fed76352b6650d69f5df3685 |
| /vendor/lib64/libOpenCL.so size | 163216 bytes |
| /vendor/lib64/libllvm-qcom.so size | 30529480 bytes |

The system and vendor partitions identify Android 13 and Android 12 respectively. Android's vendor/system separation can preserve an older vendor Android release, so this **does not by itself demonstrate a broken partition pairing or a defective driver**. The recorded 2022 Qualcomm OpenCL compiler build string is older than both on-device 2024 build dates; it must not be presented as proof that a newer compatible GPU driver is available.

Official REDMAGIC 7S Pro global support currently lists **V713 for Europe** and **V613 for North America, Asia and other regions**, with a page update dated **2025-01-21**:

https://redmagic.gorgias.help/en-US/redmagic-7s-pro-2430580?isEmbedded=true

The manufacturer also warns that a regional ROM must match the device and that converting Chinese-market handsets to a global ROM may require a full data wipe:

https://help.redmagic.gg/hc/en-us/articles/360033542691-Global-ROMs-and-Updates

**Safe next action:** Confirm the actual NX709S-UN software channel and any supported OTA update through the phone's Settings > System update or REDMAGIC customer support. Do not automatically flash the global V613/V713 packages on a device whose currently installed regional channel remains unknown. No update has been downloaded or installed in this investigation.

Qualcomm's own OpenCL optimization guidance documents the on-device local-memory query and work-group size constraints. The original bcrypt kernel allocates 4 KiB of static S-box storage for one local work-item or 32 KiB for eight; no hardware allocation failure has been established from the archived logs. A failed self-test after successful kernel initialization is not a substitute for the actual per-kernel compiler/device memory statistics.

https://docs.qualcomm.com/bundle/publicresource/80-NB295-11_REV_C_Qualcomm_Snapdragon_Mobile_Platform_Opencl_General_Programming_and_Optimization.pdf

## Distinct failure signatures

1. **Stock kernel:** OpenCL kernel and memory initialization complete, but hashcat's built-in known-vector self-test reports no match. Archived exit code **245**. No intermediate GPU computation was recorded.
2. **Single-thread variant:** The same stock self-test rejects the result with a one-thread setting. Archived exit code **245**. An eight-thread local-memory allocation cannot, by itself, explain the single-thread failure.
3. **Optimizer-disabled Qualcomm compiler:** Build options specify -D FIXED_LOCAL_SIZE=8 -cl-opt-disable. The process terminates **before** self-test with **SIGSEGV / exit 139** in /vendor/lib64/libllvm-qcom.so, via llvm::CloneFunctionInto and llvm::QGPUSplitCallGraphPass::runOnModule. This is a separate compiler-side crash, not the cause conclusively demonstrated for the normal self-test failure.
4. **Global S-box experiment:** A temporary diagnostic stores Blowfish S-box data in global memory using matching pointer address spaces. Kernel initialization succeeds; self-test still rejects the result (**245**).
5. **Standalone probe:** The isolated diagnostic did not discover a loadable OpenCL ICD and exited before program compilation (**2**). No GPU stages were captured.

Corresponding files include m03200-pure-failure.log, m03200-threads1-failure.log, m03200-cl-opt-disable-failure.crash.txt, m03200-global-scratch-failure.log, m03200-diagnostic-variants.json and m03200-standalone-probe-loader-failure.log, under tests/fixtures/android-gpu/redmagic-selected-modes-20261030/. See also docs/adreno-bcrypt-diagnostic-variants.md and docs/redmagic-gpu-selected-modes.md.

## Independent host-level evidence

- The stock bcrypt pure kernel matches the official hashcat source bytes.
- Native CPU emulation of the original m03200_init, m03200_loop and m03200_comp matches independently verified Blowfish stages and positive and negative public test candidates.
- GCC -O1 undefined-behavior traps detect a deliberately invalid positive control but not the two known bcrypt CPU test candidates.
- Independent Clang 21 compiles the stock OpenCL source at both -O0 and -O2 in OpenCL C 1.2 and 3.0 with local sizes 1 and 8 (eight configurations). SPIR64 LLVM IR retains 25 arguments in the expected address spaces and all 12 per-stage local S-box arrays: 4096 bytes per stage at local size 1 and 32768 bytes at local size 8. At -O2 the four m03200_comp arrays acquire unnamed_addr while remaining local, and four deliberately corrupted address-space declarations are detected. This Clang transformation is not evidence of Qualcomm compiler equivalence or the root cause of the RedMagic GPU error.
- Windows x86-64 GCC and Android AArch64 API 28 NDK Clang produce matching statically verified host structures: kernel_param_t 72 bytes (13 checked fields), pw_t 260 bytes (2 checked fields), salt_t 572 bytes (16 checked fields).
- Archived Pixel 9 Pro XL / Mali public bcrypt pure self-test succeeds. This is not a claim about Qualcomm's code generation.

Independent Clang compilation and native CPU success do not identify the cause of the optimized Qualcomm GPU self-test failure. Device-only memory behavior, GPU code generation, intermediate states and final result handling remain unverified. The LLVM SIGSEGV when optimization is disabled is a separately evidenced compiler crash.

## Evidence needed for a conclusive fix

A provenance-verified readback of the **real Qualcomm GPU's** intermediate bcrypt temporary state, with the corresponding kernel and module hashes, would allow comparison against the already archived native reference. Without those device data, changing the cryptographic transformations or declaring the device supported would be unsubstantiated.

For an OEM/Qualcomm issue, distinguish the ordinary **self-test rejection (245)** from the independent **compiler SIGSEGV (139)**, attach a sanitized compiler crash stack and the exact reported OpenCL build string, and ask for a supported explanation of both failures. Do not include device serial numbers, private credentials or unverified performance claims.

## Independent Vulkan Compute hardware check (2026-10-31)

A standalone, noncryptographic Vulkan 1.1 Compute program was compiled to SPIR-V by the Android NDK glslc tool and executed on the same connected RedMagic Adreno 730 and Pixel Mali-G715 GPUs. All eight combinations (two different public synthetic shaders, optimized and unoptimized, on both devices) exited 0 and matched an independent scalar CPU reference for all 8192 output words. The larger shader uses 32768 bytes of workgroup-shared memory and 256 cross-lane dependent reads per invocation with a barrier. SHA-256 source identities, actual GPU output logs and shader binary identities are archived in tests/fixtures/android-gpu/vulkan-compute-20261031/ and documented in docs/vulkan-compute-redmagic.md.

This confirms specific Vulkan Compute arithmetic and shared-memory behavior on the current Qualcomm driver, but it does **not** execute bcrypt or show whether Qualcomm's OpenCL compiler produces incorrect Blowfish instructions.

A separate synthetic three-stage Vulkan 1.1 program was also compiled and **executed on both devices** with two intermediate GPU buffers and explicit compute-to-compute barriers. Optimized and unoptimized shaders on RedMagic and Pixel each passed 12288 independent CPU word comparisons, including stage-A and stage-B readback, with identical FNV-1a checksums for all three stages. The four actual GPU logs and exact source/build identities are archived in `tests/fixtures/android-gpu/vulkan-pipeline-20261031/`; see `docs/vulkan-multistage-redmagic.md`. This proves the tested Vulkan multi-dispatch memory dependencies work on current devices, **not** that a Vulkan bcrypt implementation exists.

A further Vulkan 1.1 lifecycle diagnostic reuses a single recorded three-stage command buffer over four sequential queue submissions, changing public synthetic input between submits and resetting an already-completed fence. Both GPU devices passed all 98,304 checked 32-bit output words across the optimized and unoptimized variants per device. Four input rejection controls per phone returned code 2. Detailed provenance and real-device logs are in `docs/vulkan-command-reuse-redmagic.md` and `tests/fixtures/android-gpu/vulkan-resubmit-20261031/`.

A separate Vulkan 1.1 object lifecycle diagnostic tested 14 real resource acquisitions and cleanup actions on each device without submitting GPU work. Four complete acquisition/release cycles passed for each original stage-A shader optimization on RedMagic and Pixel. Each device also passed 14 deterministic failure injections after Vulkan resource creation (exit 42 with balanced cleanup) and five invalid CLI input checks (exit 2). Sources, exact artifact identities and the original device results are recorded in `docs/vulkan-resource-lifecycle-redmagic.md` and `tests/fixtures/android-gpu/vulkan-lifecycle-20261031/`. These checks cover application-side Vulkan object ownership, not internal driver memory accounting.

Another standalone Vulkan 1.1 memory experiment bound the unchanged synthetic three-stage workload's four storage buffers to **one physical Vulkan allocation**, selecting the compatible memory type from each device's `VkMemoryRequirements` and checking each offset's alignment. Both GPUs passed optimized and unoptimized real compute execution: 12,288 output words per run match the independent CPU reference. The measured 57,344-byte allocation uses memory type 6 on Adreno but type 0 on Mali. Three **application-simulated** failure paths (missing memory type, allocation failure and device loss after a real successful fence wait) returned explicit error codes with the expected cleanup on both devices, without actually losing a GPU. Full evidence is in `docs/vulkan-suballocation-redmagic.md` and `tests/fixtures/android-gpu/vulkan-suballocation-20261031/`.

A vendor-independent Vulkan 1.0 capability module now detects device compute limits, queue families, storage-buffer requirements, compatible memory-type bitmasks and noncoherent cache-atom alignment from Vulkan APIs. Neither GPU model nor vendor ID participates in the selection. On RedMagic and Pixel, both optimized and unoptimized **Vulkan 1.0** stage-A shaders passed an actual GPU dispatch using automatically chosen queue/memory types, including 2,048 exact CPU/GPU comparisons with matching independent Python FNV-1a checksum `00d6d6e4837edebc`. Four deliberately unsupported workload settings were rejected per successful run. The eight source hashes, seven exact build identities, actual GPU evidence and limitations are recorded in `docs/vulkan-portable-auto-configuration.md` and `tests/fixtures/android-gpu/vulkan-autoconfig-20261031/`. Actual noncoherent GPU execution, device-local-only staging and the production hashcat Vulkan backend remain untested or unimplemented.

The archived mode-3200 OpenCL self-test failure (245) and the distinct optimizer-disabled compiler SIGSEGV (139) remain unresolved.

**Current decision:** Preserve original production code and hashcat's self-test; do not declare Adreno bcrypt mode 3200 operational until an authentic device execution passes.
