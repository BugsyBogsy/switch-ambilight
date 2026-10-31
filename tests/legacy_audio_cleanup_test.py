#!/usr/bin/env python3
"""Guard the first Ambilight-only cleanup against accidental audio service reintroduction."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = root / "tier4" / "applet-mitm" / "source"
removed = (
    "applet_mitm_audout.cpp",
    "applet_mitm_audout.hpp",
    "applet_mitm_soundprobe.cpp",
    "applet_mitm_soundprobe.hpp",
)
for name in removed:
    assert not (source / name).exists(), f"Legacy audio service unexpectedly restored: {name}"

main = (source / "applet_mitm_main.cpp").read_text()
for name in ("StartAudio()", "StartAudOutMitm()", "SoundProbeStart()", "applet_mitm_audout.hpp", "applet_mitm_soundprobe.hpp"):
    assert name not in main, f"Unexpected old audio service in main: {name}"
assert main.count("g_audio_enabled.store(false)") == 2, "Audio must stay disabled on both config paths"
assert "TryAmbilightStream" in (source / "applet_mitm_nv.cpp").read_text()
print("OK: audio service and diagnostic probe removed; VIC Ambilight entrypoint retained")
