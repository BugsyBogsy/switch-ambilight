# Switch Ambilight v0.6 refactor, stage 1

This is an incremental cleanup and migration, **not a complete removal of Switch Frame Tap**.

## Changes

- Canonical configuration path: `sdmc:/config/switch-ambilight/config.ini`.
- If absent, the sysmodule, NRO and OVL read `config/switch-frame-tap/config.ini`.
- The first settings change copies old keys to the new file without changing the legacy file.
- Removed obsolete startup copying of DVR/audio IPS patches.
- Added a host regression test.

## Retained deliberately

- C20 and C21 program IDs, memory donor/launcher, `sftap` IPC service, VI/Binder interception and VIC framebuffer conversion.
- Existing compiled audio, NVENC, USB and streaming source code remains; removal requires individual linker/build/console tests.

## Validation before merging

1. Run `bash scripts/test.sh`.
2. Build and package with Docker: `bash scripts/build.sh && bash scripts/package.sh`.
3. Back up known-good C20/C21 and `config.ini`.
4. Test game capture, HOME, relaunch, WLED LEDs, network disconnect, overlay and NRO.
5. Confirm the new config is created after changing a setting and that the old config is untouched.

No release tag until the sysmodule is verified on a real Switch.

## Stage 2: remove standalone audio interceptors

- Removed the `audout:u` MITM service source and its service loop (`applet_mitm_audout.cpp/.hpp`).
- Removed the audio-memory research scanner (`applet_mitm_soundprobe.cpp/.hpp`).
- No longer starts the `grc:d` audio polling thread.
- Audio is explicitly forced off regardless of any stale `audio=1` setting or debug armed token.
- The compatibility audio ring helpers in `applet_mitm_audio.cpp/.hpp` and associated legacy NVENC branch remain until the H.264 code path is removed; **this is deliberately not a complete audio-source deletion**.

CI checks that the standalone audio interceptors are absent and the VIC Ambilight function is still present. Host tests do not compile a Switch sysmodule; Docker ARM64 build and device testing are required before merge.

## Stage 3: remove legacy NVENC experiments (build pending)

Removed NVENC magic/host1x probe, GRC IDR replay, and standalone 720p IDR, P-frame and 1080p encoder test routines from `applet_mitm_nv.cpp`. Disabled obsolete arm-file tokens `nvenc`, `nvgrc`, `nvframe`, `nvp`, and `nv1080`. Added static regression checks proving the working 320x180 VIC → color sampler → DDP send path still exists.

**Not yet removed:** the experimental `TryNvencStream` viewer implementation and shared `NvfSession`/`NvfLayout` helpers. Ambilight currently uses those shared helpers for raw VIC output. Deleting the encoder internals requires extracting a lean Ambilight-only session first.

**Safety:** This change passes text-based structural checks, not an ARM64 link or hardware test. Back up C20, compile, test on a Switch, and do not merge this draft until verified.

### NVENC cleanup compile fix and build provenance

The removed NVENC experiment call sites left two unused static helpers, `NvfEncode` and `NvfBitsLen`, which break `-Werror=unused-function`. Both are now deleted; `NvfSubmit` and `NvfWaitDone` remain because the old viewer/encoder is still present.

Packaging now requires `dist/.build-success`, recorded **only** after the full build succeeds, matching the current Git HEAD and all four binary SHA-256 digests. A failed build clears the stamp and previously staged ZIP, so a prior artifact cannot silently be shipped. Re-run `bash scripts/build.sh` before `bash scripts/package.sh` after each checkout or code change.

### ARM64 parse repair (v0.6 NVENC refactor)

The prior function-removal routine mistakenly treated the default argument `NvfJob{}` as the beginning of `NvfEncode`'s function body, leaving ` ) { ... }` at file scope and causing `expected unqualified-id before ')' token`. The complete leftover function body is now removed; a regression guard detects an orphan signature/body. The full ARM64 build and Mariko test remain required.

## Stage 4a: isolate Ambilight session type

The old `NvfSession` implementation is now `VicCaptureSession`, a shared low-level capture owner. The WLED pipeline uses `AmbilightSession final : VicCaptureSession`, while the legacy encoder/USB routines use a distinct `NvfSession final : VicCaptureSession`. Screenshot conversion accepts the VIC base type. **AmbilightSession does not derive from or contain NvfSession.** No VIC memory layout, conversion or timing code changed in this step.

The common VIC owner still has H.264-era fields and methods; `TryNvencStream` is not deleted yet. Source separation is structural, pending ARM64 and device tests.

## Stage 4b: H.264 streaming removed

Removed `TryNvencStream`, its game/HOME/probe entrypoints, and the orphaned NVENC submit, wait and poll helpers. The working VIC -> WLED loop was kept intact. Other encoder-era layouts, debug scanner and raw UVC remain pending cleanup. This requires a successful ARM64 build and hardware test before release.

## Stage 4c: physical separation of VIC capture

Moved the former capture implementation to `ambi_capture_session.inc`, included inside the original anonymous namespace. `AmbilightSession` uses the VIC owner independently of the old `NvfSession` adapter. The implementation is byte-for-byte identical; the refactor merely establishes a file boundary to make encoder-specific removal safer. Existing H.264-era fields and methods are still in the shared class and will be reviewed after ARM64 build results.

## Stage 4d: VIC capture no longer owns NVENC

The capture component now always uses VIC; its `Open` path never opens `/dev/nvhost-msenc`, maps an encoder channel, waits for H.264 jobs, or sets up a codec picture. Removed encoder stall, clock and setup methods and the `NvfSession` compatibility adapter. Raw UVC and screenshots still use the common VIC capture owner. H.264-related constants/diagnostic scaffolding in the surrounding source remain to be cleaned after successful ARM64 tests.

### Post-stream dead-code pruning

Removed the old NVENC 1080p layout factory and unused 720p slot-content test plus orphan NVENC completion globals. The 320x180 VIC raw layout used by WLED remains unchanged.

### VIC corner-probe regression fix

The `SlotContentIs720` function was mistakenly removed during H.264 clean-up, while `VicCaptureSession::Decide` still calls it from `ambi_capture_session.inc`. This breaks ARM64 compile. Restored the exact known working probe and added a host test that compiles production `BlOffset` / `SlotContentIs720` and validates black-outside, lit-outside, and lit-inside 720p corner cases. Future clean-ups must scan every `.inc` and `.hpp` dependency before deleting a function.

## Stage 5a: obsolete GRC scanner and audio

Removed the historical H.264 GRC memory scanner, the `grcscan` flag and their entrypoint. This **does not remove the early GRC service setup**, which remains part of the old startup path until separately audited. Removed the unused grc:d PCM recorder and audio ring implementation. The capture tail now holds only the screenshot PNG buffer and the v4 status IPC audio field stays zero. USB/UVC and old viewer transports remain pending further separation.
