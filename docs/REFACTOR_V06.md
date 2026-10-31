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
