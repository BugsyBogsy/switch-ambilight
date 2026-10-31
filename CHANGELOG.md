## Unreleased - v0.6.0 (refactor candidate)

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
