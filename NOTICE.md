# Attribution and third-party notices

**Switch Ambilight** is a modified derivative of [Switch Frame Tap](https://github.com/TomasUribe/switch-frame-tap), by TomasUribe and contributors. Its game capture, VIC buffer handling, Atmosphère MITM services, network support, launcher and donor originate from that project. The original work is licensed under GPL-2.0. The copyright notices and history of the upstream project remain attributable to their respective authors.

**libtesla** by WerWolv and contributors is used as an external header dependency for the overlay and is GPL-2.0 licensed. It is fetched during setup and not vendored here.

**Atmosphère**, **libnx** and **devkitPro** are external toolchain dependencies fetched separately. Their respective licences apply.

Changes in this derivative include: edge-color extraction from a downscaled NV12 capture, DDP output to WLED, Ambilight-only networking, runtime settings, the NRO settings interface and Tesla overlay.

This repository is not affiliated with or endorsed by Nintendo, Atmosphère, Switch Frame Tap or WLED. No Nintendo firmware, encryption keys, or proprietary binaries are distributed.
