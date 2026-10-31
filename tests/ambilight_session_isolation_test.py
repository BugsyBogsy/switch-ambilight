#!/usr/bin/env python3
from pathlib import Path
s=(Path(__file__).resolve().parents[1]/"tier4/applet-mitm/source/applet_mitm_nv.cpp").read_text()
assert "struct VicCaptureSession {" in s
assert "struct AmbilightSession final : VicCaptureSession {};" in s
assert "struct NvfSession final : VicCaptureSession {};" in s
assert "struct AmbilightSession final : NvfSession" not in s
a=s[s.rfind("StreamEnd TryAmbilightStream("):s.find("/* ---- M99:",s.rfind("StreamEnd TryAmbilightStream("))]
assert "AmbilightSession s;" in a and "NvfSession s;" not in a
assert "bool ShotFromSession(const VicCaptureSession &s)" in s
print("OK: AmbilightSession independent from NvfSession; shared VIC capture retained")
