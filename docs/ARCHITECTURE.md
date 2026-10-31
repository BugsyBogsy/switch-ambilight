# Architecture and compatibility boundaries

```
Game graphics -> VI/binder interception (C20 sysmodule) -> VIC downscale ->
NV12 edge sampler (114 LEDs) -> DDP UDP port 4048 -> WLED ESP32
                                ^
                              configuration from SD
                              ^                ^
                    Tesla overlay (.ovl)  Homebrew (.nro)
```

The `.ovl` and `.nro` update the legacy `config/switch-frame-tap/config.ini` file. They use the existing `sftap` IPC service to apply a change. The runtime reads the new `ambi_*` settings and restarts the capture session.

C20 intercepts game graphics and performs the color extraction. C21 is a separate boot launcher/memory donor, retained because C20 uses the upstream startup and memory allocation model. The original Switch Frame Tap viewer interfaces are not required. The internal capture implementation and legacy config/service names still need refactoring.

Do not rename C20/C21 IDs independently of the launcher and service references. The fixed LED mapping is clockwise from the top-left: `[0..35] top`, `[36..56] right`, `[57..92] bottom`, `[93..113] left`.

Dependencies fetched separately during `scripts/setup.sh`: Atmosphère 1.12.1 and libtesla. Toolchain is based on the devkitPro image plus a libnx revision compatible with Atmosphère 1.12.1.
