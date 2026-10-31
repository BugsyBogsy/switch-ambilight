#!/usr/bin/env python3
from pathlib import Path
root=Path(__file__).resolve().parents[1]
source=root/"tier4/applet-mitm/source"
assert not (source/"applet_mitm_audio.cpp").exists()
assert not (source/"applet_mitm_audio.hpp").exists()
nv=(source/"applet_mitm_nv.cpp").read_text()
main=(source/"applet_mitm_main.cpp").read_text()
control=(source/"applet_mitm_control.cpp").read_text()
header=(source/"applet_mitm_nv.hpp").read_text()
for name in ("TryGrcScan(", "GrcProgramIdValue", "ScanHit", "g_grcscan_armed"):
    assert name not in nv+main+header, f"Legacy GRC scanner restored: {name}"
for name in ("g_audio_stage", "g_audio_ring", "AudioRingBytes", "AudioStageSize"):
    assert name not in nv, f"Legacy audio capture allocation restored: {name}"
assert "constexpr size_t CaptureTailBytes = ShotOutBufSize;" in nv
assert "st.reserved     = 0;" in control
assert "NetAmbilightSend(rgb, sizeof(rgb))" in nv
print("OK: GRC scanner and audio recorder removed; VIC/WLED retained")
