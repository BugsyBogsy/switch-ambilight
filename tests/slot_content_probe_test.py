#!/usr/bin/env python3
"""Compile and exercise production corner-mode detector on the host.

Prevents deleting a VIC dependency that cannot be detected by string-only
tests, even when it is used by another .inc source file.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
src = (root / "tier4/applet-mitm/source/applet_mitm_nv.cpp").read_text()
capture = (root / "tier4/applet-mitm/source/ambi_capture_session.inc").read_text()
assert "corner = SlotContentIs720(" in capture

def extract(start: str, end: str) -> str:
    a=src.find(start)
    b=src.find(end, a + len(start))
    assert 0 <= a < b, f"Missing production capture helper: {start}"
    return src[a:b]

offset=extract("        constexpr u64 BlOffset(", "        /* the MK8 formula")
probe=extract("        bool SlotContentIs720(", "        /* v0.7.3: what a 1080p")
assert "BlOffset(" in probe and "return zero" in probe
harness = r'''
#include <cassert>
#include <cstdint>
#include <vector>
#include <cstdio>
using u8=std::uint8_t;
using u32=std::uint32_t;
using u64=std::uint64_t;
''' + offset + "\n" + probe + r'''
int main() {
    // The framebuffer is block-linear, not a linear RGBA array.
    // A 1080p picture uses 9 blocks of 128 rows; each block contains
    // 120 GOB columns of 8192 bytes: 0x870000 (8,847,360 bytes).
    constexpr u32 block_rows = (1080u + 127u) / 128u;
    constexpr u32 gob_columns = (1920u * 4u) / 64u;
    constexpr u32 block_bytes = 512u << 4;
    std::vector<u8> slot(block_rows * gob_columns * block_bytes, 0);
    u32 count=0;
    assert(SlotContentIs720(slot.data(), &count, 1920, 4));
    assert(count > 100);
    // A visible pixel outside the 720p upper-left corner forces 1080p.
    const u64 off=BlOffset(1400,10,1920,4);
    assert(off+3 < slot.size());
    // Confirm the latest sampled outside-corner offset is in the padded surface.
    assert(BlOffset(1210,1040,1920,4)+3 < slot.size());
    slot[off]=255;
    assert(!SlotContentIs720(slot.data(), &count, 1920, 4));
    slot[off]=0;
    // A pixel inside the 720p corner must not force 1080p.
    slot[BlOffset(80,30,1920,4)]=255;
    assert(SlotContentIs720(slot.data(), &count, 1920, 4));
    std::puts("OK: production VIC 720p probe builds and handles corners correctly");
}
'''
with tempfile.TemporaryDirectory() as tmp:
    p=Path(tmp)
    (p/"probe.cpp").write_text(harness)
    subprocess.run(["g++", "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                    str(p/"probe.cpp"), "-o", str(p/"probe")],check=True)
    subprocess.run([str(p/"probe")],check=True)
