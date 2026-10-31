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
