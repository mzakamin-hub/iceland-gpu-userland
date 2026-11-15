# iceland GPU userland (macOS 27, arm64e)

Stage S11 of the OnePlus Pad 4 (`iceland`, SM8850) macOS port, route B (owner decision, 12 Nov): build UniXNU's macOS
GPU userland on GitHub's disposable Mac, because that is the only legal way for us to use Apple's macOS SDK.

This repository holds **this build definition only**: the workflow, the pins, and the preparation/capture scripts. It
does not hold, and must never hold:

- Apple-derived material of any kind: no IPSW parts, kernelcache, kexts, prelinked collection, corecrypto, Apple SDK
  files, trust caches, Data seeds or root policies;
- firmware blobs (the GPU firmware is fetched from its own public repository by UniXNU's scripts, pinned by SHA-256);
- keys, the owner key, identity headers or personal data.

Every source is fetched at build time from its public repository, at an exact commit (`pins/inputs.json`), and verified
with `git rev-parse HEAD` before anything is built (`scripts/verify-pins.py`).

Our own patches are few, each hash-pinned in `pins/inputs.json`, re-checked on every run, and either ours or a change to an
MIT upstream file under that file's licence. Each file's header says why it exists and where the finding came from.

| Patch | Applies to | Why |
|---|---|---|
| `adrenoprobed-macos-gate.patch` | AdrenoMetal | the probe daemon matched only iPadOS' `IOAcceleratorES`; on macOS it matches `AGXAccelerator` |
| `adrenometal-turnip-smoke-macos-service.patch` | AdrenoMetal | the daemon's Turnip phase defaulted `TU_DARWIN_SERVICE` to `IOAcceleratorES`; macOS: `AGXAccelerator` |
| `adrenometal-function-lookup-errors.patch` | AdrenoMetal | upstream 6d4a3f3 (MIT, unmerged in main): a function missing from a library answers `MTLLibraryErrorFunctionNotFound`, so Core Image tries its next library instead of failing (`_lanczosDownV`/`H`) |
| `adrenometal-universal-library-entries.patch` | AdrenoMetal | the .mm parts of upstream 99da261 + 0e018dc (MIT), re-based: a universal metallib's entries are read from its AIR64 slice, so the not-found answer also reaches universal libraries |
| `mesa-iceland-1-bo-finish-order.patch` | Mesa, after UniXNU's record | a freed BO's slot is zeroed inside the retire lock, before its handle can be handed out again |
| `mesa-iceland-2-ir3-round-default.patch` | Mesa, after UniXNU's record | `cov` rounds to nearest even when no rounding mode is declared (it truncated) |
| `mesa-iceland-4-macos-service.patch` | Mesa, after UniXNU's record | Turnip's own default service on macOS is `AGXAccelerator` |
| `mesa-iceland-5-dsb-submit.patch` | Mesa, after UniXNU's record | `dsb sy` before every Submit and at the end of `vkEndCommandBuffer` |

The Mesa patches are appended to UniXNU's own record (`scripts/data/adreno-compiler-inputs.json`) in the checkout, so
UniXNU's build guard (`scripts/mesa_patch_identity.py`) checks them with the rest. Not carried, on purpose:
`mesa-iceland-3` (no CPU wait on an already-fenced sync): UniXNU's `darwin_queue_submit` already waits only for syncs that
no submission has resolved yet. No `gpu_id` special case for the A840: Mesa `85fda3b`'s table matches chip `0x44050a31` by
its `0xffff44050A31` entry ("Adreno (TM) 840", fuse-id wildcard) whatever `gpu_id` the back end decodes.

The compile fix this repository carried for AdrenoMetal `c1351337` (`adrenometal-pinned-source-build.patch`) is gone:
`73d582f` has both defects fixed upstream.

## What it builds

Pinned sources: UniXNU `d8210e08` (main, 14 Nov 2026) with `AdrenoMetal` `73d582f`, `Adreno740` `54b41b8`,
`GopFramebuffer`, `AdrenoPM4` and `AdrenoFirmware`; the Mesa mirror at `85fda3b` plus UniXNU's 23 recorded patches and our
four; the metal2vulkan bridge at `555b13d` plus its own 12.

1. `python3 scripts/build-turnip-darwin.py --target macos27` — Mesa's freedreno Vulkan driver (Turnip) with UniXNU's
   Darwin kernel backend, `arm64e-apple-macos27.0`.
2. `python3 scripts/build-adreno-metal-plugin.py --target macos27 --no-replay` — the Metal plugin bundle
   (`NativeAdrenoMetal.bundle`, macOS `Contents/` layout) and the `adrenoprobed` probe daemon, both ad-hoc signed with
   4 KiB pages by Apple's own `codesign`.

Both scripts are UniXNU's, run unmodified. `--no-replay` only skips `gen-turnip-replay.py`, which captures a host
simulation's first submission; the plugin's `TurnipReplay.h` is optional (`NativeAdrenoUserTest.h` includes it under
`__has_include`), so the bundle builds unchanged.

The owner trust module (`scripts/make-owner-trust-bundle.py`) is **never** built here: it needs Apple's static trust
caches. It is built locally, later, from the CDHashes this workflow reports.

## Running it

`workflow_dispatch` only, one run at a time, standard `macos-26` runners, `permissions: contents: read`, no secrets.

| Stage | What | Timeout |
|---|---|---|
| `c0` | environment check: Xcode list and selection, macOS SDK, `arm64e-apple-macos27.0` compile and `-framework IOGPU` link, rust 1.98.0 + `rust-src`, python, disk, brew formula availability. No build. | 8 min |
| `c12` | job `turnip`: Turnip → `libvulkan_freedreno.dylib`; job `plugin` (needs it): plugin + daemon, the dylib passed inside the run | 40 min each |
| `absentd` | the S5 stand-in for a daemon on the kernel's deny-exec list (`absent-service/absentd.c`, copied from UniXNU with its provenance line and hash-pinned): built `arm64e-apple-macos27.0` with `-Os -Wall -Wextra -Werror -fblocks -framework CoreFoundation`, ad-hoc signed with `--pagesize 4096 --identifier org.unixnu.absentd`, no entitlements | 10 min |
| `shim` | the q33e owner boot shim: `scripts/macos-boot-shims/apfs_boot_util.c` built `arm64e-apple-macos27.0` and ad-hoc signed with `--pagesize 4096 --identifier org.unixnu.apfs_boot_util`, as `build-boot-runtime.py` does. No entitlements. | 10 min |

Artifacts are retained for one day and deleted as soon as they have been downloaded to the project machine.

## Cost

The free plan includes $12 of private Actions usage per month; macOS bills at $0.062 a minute. Every dispatch is
preceded by an account-wide preflight over all private repositories (runs created in the last 31 days, timing API,
per-job minutes rounded up per OS), plus the new run's worst case (its timeout times the macOS rate). A run is
dispatched only if the total stays at or under $12.00. The ledger lives with the project, not in this repository.
