## Unreleased - v0.6.0 (refactor candidate)

- Remove obsolete GRC H.264 memory scanner and `grcscan` debug token, but retain boot GRC service handling.
- Remove unused audio recorder and 96 KiB of audio-only capture tail.
- Preserve stream status IPC layout with former audio field now zero.

- Fix 720p probe host fixture to allocate padded 0x870000 block-linear framebuffer rather than a smaller linear 1920x1088x4 buffer.

- Restore VIC 720p corner probe `SlotContentIs720` wrongly removed as H.264 dead code; capture session still calls it.
- Add a host C++ test that compiles the production probe and checks simulated 720p/1080p frame selection.

- Remove unused NVENC 1080p layout generation, legacy 720p check and orphaned encoder job stats.

- Make VIC capture truly encoder-free: delete the msenc device opening, channel mapping, encoder clock/stall logic, and encoder context fields.
- Remove the unused legacy NvfSession adapter; share VIC capture directly with UVC/screenshots.

- Extract low-level swapchain/VIC capture owner to private `ambi_capture_session.inc` without modifying its behaviour.
- Update host tests to expect the encoder submit/wait helpers to be absent.

- Remove the H.264 streaming implementation and three dispatch paths; use Ambilight capture for WLED.
- Drop orphaned encoder submit/wait/poll functions; other H.264 layouts and debug probes still require audit.

- Isolate AmbilightSession from the old NvfSession using a common VIC capture owner without changing the proven capture code.

- Fix orphaned `NvfEncode` function body after a source-removal script mistook the `NvfJob{}` default argument for the opening brace.
- Add a regression check for stray function-signature tails.

- Remove orphan `NvfEncode` and `NvfBitsLen` functions after removing NVENC test callers (fixes ARM64 -Werror=unused-function).
- Block packaging stale NSPs after unsuccessful builds by checking a successful-build stamp, current Git revision and output checksums.
- Prevent sysmodule build wrapper from copying outdated NSP outputs on failed compilations.

- Remove NVENC magic/host1x probes, GRC IDR replay and standalone 720p/P-frame/1080p encoder experiments.
- Disable unused debug arming tokens for the removed NVENC probes; retain VIC capture shared layout and legacy viewer encoder pending extraction.
- Add NVENC-removal regression tests.

- Remove `audout:u` interception, sound-memory probes and automatic audio-thread startup.
- Explicitly disable audio capture in both config entrypoints; retain legacy NVENC audio helpers temporarily for ABI/build safety.
- Add standalone-audio regression checks.

- Fix ARM64 build script CRLF line endings; enforce Unix LF for shell files and add regression checks.
- Fix the test runner's broken shell quoting and add a ZIP packaging fallback using Python 3.
- Fix misleading v0.5.1 output filename in the v0.6 packaging script.

- New own configuration directory with safe legacy fallback.
- Removed obsolete automatic audio/DVR IPS patch synchronization.
- Added migration regression test; kept C20/C21, VI/VIC and service names until hardware validation.
- Full USB/H.264/audio source removal is still pending.

# Changelog

## 0.5.1 (source packaging)
- Standalone GitHub-ready layout with capture sysmodule, donor/launcher, overlay and NRO.
- Restore NRO display by calling `consoleUpdate(nullptr)` in the rendering loop.
- Replace personal WLED IP defaults with the generic example `192.168.1.50` (user config takes precedence).
- Add Docker toolchain, build/staging scripts, host tests and project documentation.
- No changes to the working VIC/NV12 capture architecture.

## 0.5 (user-tested)
- Live control from Tesla overlay and NRO manager.
- Runtime-configurable IP, brightness, smoothing and frame interval.
- Ambilight-only network mode without viewer TCP/listener beacons.

## 0.4 (user-tested)
- Game capture via VIC and 320×180 NV12 sampling.
- 114-edge LED mapping and WLED DDP/UDP transport.
