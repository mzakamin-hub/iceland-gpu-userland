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

## What it builds

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
| `c1` | Turnip → `libvulkan_freedreno.dylib` | 40 min |
| `c2` | plugin + daemon, with the c1 dylib restored from its artifact | 40 min |

Artifacts are retained for one day and deleted as soon as they have been downloaded to the project machine.

## Cost

The free plan includes $12 of private Actions usage per month; macOS bills at $0.062 a minute. Every dispatch is
preceded by an account-wide preflight over all private repositories (runs created in the last 31 days, timing API,
per-job minutes rounded up per OS), plus the new run's worst case (its timeout times the macOS rate). A run is
dispatched only if the total stays at or under $12.00. The ledger lives with the project, not in this repository.
