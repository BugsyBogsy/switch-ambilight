#!/usr/bin/env python3
"""WLED-only code must not start USB gadget or UVC webcam services."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]/"tier4/applet-mitm/source"
nv=(root/"applet_mitm_nv.cpp").read_text()
main=(root/"applet_mitm_main.cpp").read_text()
control=(root/"applet_mitm_control.cpp").read_text()
assert not (root/"applet_mitm_uvc.cpp").exists()
assert not (root/"applet_mitm_uvc.hpp").exists()
for text in (nv,main,control):
    assert '#include "applet_mitm_uvc.hpp"' not in text
    assert "g_uvc_mode" not in text and "g_uvc_raw" not in text
assert "TryUvcRawStream(" not in nv
assert "g_usb_armed                 = false;" in main
assert 'ArmFileContains("usb")' not in main
assert "TryAmbilightStream(dbg, slot_base" in nv
assert "TryAmbilightStream(dbg, base" in nv
assert "NetAmbilightSend(rgb, sizeof(rgb))" in nv
print("OK: UVC webcam removed, USB gadget disabled, WLED capture retained")

capture=(root/"ambi_capture_session.inc").read_text()
assert "g_uvc_mode" not in capture and "g_uvc_raw" not in capture
