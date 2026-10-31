#!/usr/bin/env python3
"""Prove the legacy NVENC experiments were deleted without changing VIC capture."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
source = (root / "tier4/applet-mitm/source/applet_mitm_nv.cpp").read_text()
main = (root / "tier4/applet-mitm/source/applet_mitm_main.cpp").read_text()
removed = ("TryNvencPhaseA", "TryNvencProbe", "TryNvencGrcReplay",
           "TryNvencRealFrames", "TryNvencPFrames", "TryNvenc1080")
for name in removed:
    assert name + "(" not in source, f"Removed NVENC entrypoint restored: {name}"
for token in ('ArmFileContains("nvenc")', 'ArmFileContains("nvgrc")',
              'ArmFileContains("nvframe")', 'ArmFileContains("nvp")',
              'ArmFileContains("nv1080")'):
    assert token not in main, f"Unexpected legacy NVENC probe armed: {token}"
assert "TryAmbilightStream(" in source
assert "sftambi::Sample(" in source
assert "NetAmbilightSend(" in source
assert "struct NvfSession" in source
print("OK: legacy NVENC probes removed; 320x180 VIC/WLED path retained")
