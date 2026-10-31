#!/usr/bin/env python3
from pathlib import Path
s=(Path(__file__).resolve().parents[1]/"tier4/applet-mitm/source/applet_mitm_nv.cpp").read_text()
for name in ("TryNvencStream(", "NvfSubmit(", "NvfWaitDone(", "NvfPollDone("):
    assert name not in s, f"Old encoder function restored: {name}"
assert "TryAmbilightStream(dbg, slot_base" in s
assert "TryAmbilightStream(dbg, base" in s
assert "AmbilightSession s;" in s
assert "NetAmbilightSend(rgb, sizeof(rgb))" in s
assert "sftambi::Sample(colors, Y, UV" in s
print("OK: H.264 stream dispatch/encoder submit removed; WLED VIC path retained")

for old in ("Layout1080(", "MakeLayout(", "g_nvf_last_fence"):
    assert old not in s, f"Unused H.264 helper restored: {old}"

# Probe used by the VIC capture/session's 720p corner decision is not H.264 dead code.
assert "bool SlotContentIs720(" in s
capture=(Path(__file__).resolve().parents[1]/"tier4/applet-mitm/source/ambi_capture_session.inc").read_text()
assert "corner = SlotContentIs720(" in capture
