# Switch Ambilight

**Reactive Ambilight for Nintendo Switch + WLED, directly from the console. No HDMI capture card, computer or Hyperion required during play.**

Switch Ambilight is an **experimental open-source Atmosphère sysmodule** that captures a low-resolution copy of game frames through the Switch's VIC graphics engine, samples their edges, and sends DDP/UDP colors to a WLED controller. It also includes a Tesla overlay and a Homebrew Menu settings application.

> **Project status:** v0.6.0 refactor candidate (not yet hardware-tested). The v0.5 sysmodule, overlay and NRO were reported working on one Switch V2 (Mariko) with Atmosphère 1.12.1 / firmware 23.0.1 and 114 LEDs. This GitHub-ready source package changes the default destination IP and includes the previously tested NRO display fix. The full standalone release build has **not yet been independently reproduced**. Treat this as experimental software.

## Features

- Runs autonomously on the Switch while playing; WLED receives DDP on UDP port 4048.
- Captures game images at 320×180 NV12, targeting up to 30 updates/second (actual rate varies).
- Tesla overlay: on/off, brightness, smoothing and update rate.
- NRO manager: WLED IPv4 configuration and the same key settings.
- Network viewer TCP server and discovery advertisements disabled in Ambilight mode.
- Doesn't require the original Switch Frame Tap manager or overlay.

## Compatibility and limitations

- **Tested by the project user:** Mariko V2, Atmosphère 1.12.1, Nintendo firmware 23.0.1, a docked 1920×1080 output and WLED (114 LEDs).
- **Not yet verified:** all Switch models, all firmware versions, every game, power consumption, wireless latency, and long-running stability.
- **Fixed LED geometry in v0.5.1:** 36 top, 21 right, 36 bottom, 21 left. LED 0 starts at the **top-left**, moving clockwise around the display as seen from the front. Other arrangements require editing/recompiling the sampling implementation.
- Current binaries inherit the sysmodule IDs and several internal services from Switch Frame Tap. **Do not install both projects simultaneously.** The legacy `config/switch-ambilight/config.ini` path is kept for compatibility; changing it requires a coordinated update to the sysmodule, manager, overlay and launcher.
- The `0100000000000C21` launcher/memory donor is **required**. Do not delete it. Full removal of former streaming/audio code is future work, not a claim for this version.
- Tesla/Ultrahand and a compatible overlay loader such as nx-ovlloader are required for the `.ovl` UI.

## Building from source (Linux or WSL2)

Requirements: Git, Bash, Python 3, `rsync`, `g++` for host tests, `zip`, Docker with permission to run containers, and Internet access for the toolchain. Build scripts download Atmosphère and libtesla only into ignored `ref/` directories. No proprietary Nintendo SDK or console keys required.

```bash
sudo apt update && sudo apt install -y git rsync python3 build-essential zip
bash scripts/test.sh
bash scripts/build.sh
bash scripts/package.sh
```

The build uses a Docker image derived from `devkitpro/devkita64:20260219` with libnx revision `b156b4774fb2df7b93402169509266da7547fc4c`, which exposes functions required by Atmosphère 1.12.1. The image is built automatically if unavailable. `scripts/setup.sh` checks out the Atmosphère 1.12.1 tag and libtesla headers. The capture sysmodule is built inside a copy of Atmosphère's tree using the inherited Switch Frame Tap build process.

Successful output: `dist/switch-ambilight-v0.6.0-switch.zip`, containing **both system components** and the two menus.

## Installing (experimental)

1. **Back up your SD card and working Atmosphère setup.** If updating an existing installation, save both `0100000000000C20` and `0100000000000C21`, plus your existing `config.ini`. You need Hekate SD access or a card reader for recovery from a boot failure.
2. Power down the console completely. Extract the compiled `dist/switch-ambilight-v0.6.0-switch.zip` onto the root of its SD card.
3. Configure `SD:/config/switch-ambilight/config.ini` with the values from `config.example.ini`. **Change `ambi_ip` to your WLED device's IP**. Don't overwrite an existing working config blindly.
4. Remove the old `switch/switch-frame-tap/switch-frame-tap.nro` and `switch/.overlays/switch-frame-tap.ovl` if present. Keep the two Atmosphère module folders. Ensure any stale root-level `applet-mitm.armed` debug file is absent.
5. Disable any PC Hyperion instance controlling the same WLED LEDs. Boot Atmosphère, launch a game and check whether the colors match the display. You can adjust settings through the NRO or Tesla overlay.

Installation layout:

```
SD:/
├── atmosphere/contents/0100000000000C20/exefs.nsp       # Ambilight capture sysmodule
├── atmosphere/contents/0100000000000C21/exefs.nsp       # Boot launcher and memory donor
├── atmosphere/contents/0100000000000C21/flags/boot2.flag
├── atmosphere/contents/0100000000000C21/toolbox.json
├── switch/.overlays/switch-ambilight.ovl
├── switch/switch-ambilight/switch-ambilight.nro
└── config/switch-ambilight/config.ini                     # user-created settings
```

### Settings

| Key | Default | Meaning |
| --- | --- | --- |
| `ambilight` | `1` | Enable autonomous Ambilight mode. |
| `ambi_ip` | `192.168.1.50` | **Replace with your WLED IPv4 address.** |
| `ambi_brightness` | `190` | 0–255 RGB scaling. |
| `ambi_smoothing` | `160` | 0–255 response factor; higher = faster. |
| `ambi_fps` | `25` | Capture target, 5–30 Hz. |

An invalid setting falls back to a default. Changes through the manager/overlay are saved to the SD card and reload the sysmodule capture session, so a brief flicker may occur.

## Safety and rollback

A sysmodule that hooks game graphics can cause crashes or failed boots. If this happens, power off and restore your backed-up `exefs.nsp` files. Alternatively, remove the experimental `0100000000000C20` and `0100000000000C21` folders from the SD after confirming they belong to this project and not another required module. Do **not** install this as a game via a title installer.

## Roadmap

- User-configurable LED counts/order/rotation.
- Migrate the configuration directory and module naming without breaking upgrades.
- Strip remaining unnecessary streaming/USB/audio functionality after regression tests.
- More testing across Switch models, firmware versions and WLED configurations.
- Published release binaries and build CI, once the standalone build has been reproduced on a clean machine.

## Credits and licensing

This is a **GPL-2.0 licensed derivative** of [Switch Frame Tap](https://github.com/TomasUribe/switch-frame-tap) by TomasUribe and contributors, using [libtesla](https://github.com/WerWolv/libtesla) for the overlay and [WLED](https://github.com/wled/WLED) for lighting. See [NOTICE.md](NOTICE.md) and [LICENSE](LICENSE) for attribution and terms.

**Disclaimer:** Not affiliated with Nintendo or the original projects. No Nintendo copyrighted firmware or keys are included.

## v0.6 development status

Config now lives under `config/switch-ambilight/config.ini`, retaining legacy fallback and first-write migration. The audio/DVR patch synchronizer is removed. The graphics capture and donor still retain Switch Frame Tap code, so this is **not a full rewrite**. This candidate requires Docker compilation and Mariko regression testing. See [refactoring checklist](docs/REFACTOR_V06.md).
