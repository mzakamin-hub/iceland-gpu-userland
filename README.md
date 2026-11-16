# iceland GPU userland (macOS 27, arm64e)

Stage S11 of the OnePlus Pad 4 (`iceland`, SM8850) macOS port, route B (owner decision, 12 Nov): build UniXNU's macOS
GPU userland on GitHub's disposable Mac, because that is the only legal way for us to use Apple's macOS SDK.

This repository holds **build definitions and small sources of ours only**: the workflows, the pins, the
preparation/capture scripts, and our own code that those workflows compile (`userland/`, `shaders/`, `tools/`). It does not
hold, and must never hold:

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
| `adrenometal-mac-gpu-profile.patch` | AdrenoMetal | ours (facts from macos-uke and SnapMetal), with boot-arg `agx-mac-profile=1` only: a Mac GPU of no Apple family (Common1-3, Mac2; featureProfile 10001), no function pointers, dynamic libraries, SIMD-group, tile, image-block or memoryless features, linear rows of 64 bytes or more, IOSurface and read-only linear alignment 16; framebuffer fetch stays the runtime's answer (yes on Turnip: without it Core Animation drew round shapes as diamonds and materials dark), and boot-arg `agx-mac-fetch=0` answers NO again with Core Animation's `CA_DISABLE_GENERIC_SHADERS` |
| `adrenometal-ci-mps.patch` | AdrenoMetal | ours (fact from upstream 19d6bcf, MIT), macOS only: with boot-arg `ci-mps=0` the plugin sets `CI_ENABLE_MPS=0` (not overwriting), so Core Image blurs with its own kernels instead of MPS (iconservicesagent's abort in MPSImage under `CIBlurProcessor`) |
| `adrenometal-half-each-operation.patch` | AdrenoMetal | ours (fact from upstream ce37cbc and ce8986f, MIT), macOS only: with boot-arg `agx-half-each=1` the translator states every `half` operation in `float` with its conversion (`HalfChainPolicy::EachOperation`) instead of leaving it to the GPU's 16-bit registers; a comparison switch, off by default |
| `adrenometal-recovered-object-restart.patch` | AdrenoMetal | upstream adddc7b's second hunk (MIT): the render server ends itself, for launchd to start it again, when a command buffer is refused with `MadeBeforeRecovery` (objects kept from before the runtime recovered its device) |
| `adrenometal-gpu-prime-switch.patch` | AdrenoMetal | ours (fact from upstream c890147, MIT), macOS only: with boot-arg `gpu-prime=0` the render server starts no shader priming thread; a diagnostic for pacing comparisons |
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

## Our own userland (`userland.yml`)

Code of ours that needs Apple's macOS SDK to compile, built on the same disposable Mac and for the same reason. Sources
are ours and use public SDK headers only; nothing is fetched, so there is nothing to cache.

| Component | Folder | Product | Lands at (boot add-on path) |
|---|---|---|---|
| `audio-hal` | `userland/audio-hal` | `IcelandAudio.driver`, a CoreAudio AudioServerPlugIn (HAL plug-in); a skeleton with no device yet | `Library/Audio/Plug-Ins/HAL/` |
| `camera-cmio` | `userland/camera-cmio` | `IcelandCameraHost.app`, which asks the system to activate the CoreMediaIO camera extension it carries (`org.iceland.camera.extension`: one device, "Front Camera", 816 x 612 BGRA at 30 fps, from the in-kernel `IcelandCamera` service or a test pattern), and `iceland-camera-probe` (the service, the CoreMediaIO device list, pictures as a client gets them) | `Library/Iceland/` and `bin/` |
| `tools` | `userland/tools` | `unixnu-absentd` (the pinned source of the `absentd` stage) and `iceland-audio-probe` (lists the audio devices coreaudiod publishes) and `iceland-audio-rec` (records from an input device through an AudioQueue into a WAV; entitled for the microphone) | `bin/` |

Each folder has a `build.sh` (compiles every source file of the folder for `arm64e-apple-macos27.0` into `$BUILD_DIR`)
and an `install.json` (the products, their signing identifiers and where the add-on puts them). To add code, drop
source files into the folder; to add a product or a tool, add it to `build.sh` and `install.json`.
`scripts/package-userland.py` signs every Mach-O ad hoc with 4 KiB pages (no identity, no key, no certificate: the
signature `adrenoprobed` carries), verifies it, and writes `userland-manifest.json` with each file's install path,
mode, SHA-256 and CDHash. The owner key is never on GitHub, not even as a secret; the owner trust entry and the boot
add-on are made on the project machine from that manifest.

`workflow_dispatch` only (`component`: `all`, `audio-hal`, `camera-cmio` or `tools`), one macOS job capped at 10
minutes, `permissions: contents: read`, no secrets, artifact `userland` kept one day.

## Shader check (`shader-check.yml`)

Our own Metal test shaders (`shaders/*.metal`: a rounded-rectangle distance field, a separable blur, a backdrop blend
through framebuffer fetch, and length/distance/normalize/rsqrt) through the two compilers of the tablet's Metal path,
on a Linux runner, with no GPU and no QEMU boot:

1. the pinned metal2vulkan with the patches the plugin's bridge applies (AIR to SPIR-V; it runs `spirv-val` on every
   module it writes);
2. Turnip's ir3 compiler: `tools/pipeline-check` creates each pipeline on Turnip, built for Linux from the same patched
   Mesa tree, over Mesa's no-op drm-shim as the Adreno 840 (`patches/mesa-shim-a840.patch`, for this job only).

A case fails when either compiler refuses it, when its SPIR-V lacks what `shaders/cases.json` names for it, or when a
module differs from its hash in `shaders/golden/<case>.json` (a case with no golden file is reported `unpinned`).

No Apple file is read or stored. The shaders are ours; the only Apple tool is the Metal compiler, on a short macOS job
that turns them into AIR objects (LLVM bitcode, not a metallib). That job runs only when the shaders changed: the
objects are cached by the shaders' hash, and are never committed. The Linux job refuses a metallib in its input.

`workflow_dispatch` only. With both caches warm the run is Linux only, about two minutes; a changed shader adds about
one macOS minute; a changed pin or patch rebuilds Mesa and the translator on Linux, about twenty minutes.

## Cost

This repository is public, so its runs on GitHub's standard runners (macOS and Linux) are not billed. Every workflow is
still `workflow_dispatch` only, one run at a time per workflow, with a cap on every job.

The account's free plan includes $12 of Actions usage a month for *private* repositories (macOS at $0.062 a minute,
Linux at $0.006). The account-wide preflight and the ledger, which live with the project and not here, concern those
repositories; they would apply to this one again only if it were made private.
