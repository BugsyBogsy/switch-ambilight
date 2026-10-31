/*
 * v0.7.14: the system overlays in the stream (applet_mitm_overlay.cpp) - the
 * volume bar (overlayDisp) and Tesla / Ultrahand, read from their processes'
 * memory and blended over the game by the VIC job that already converts each
 * frame. Off unless config.ini has overlays = 1 (arm token "overlays", the
 * manager's toggle) or sdmc:/config/switch-frame-tap/overlays.txt exists
 * (test builds); beta.
 */
#pragma once
#include <stratosphere.hpp>
#include <atomic>

namespace ams::mitm::applet {

    /* the Tesla / Ultrahand overlay loader (nx-ovlloader). Its first vi:m
     * session opens at boot, before this module; the overlays it shows open
     * new ones, which vi:m wraps while g_ovl_vi_mitm is set - their binder
     * transactions say when it presents, and are never the capture's */
    constexpr u64 TeslaProgramId = UINT64_C(0x420000000007E51A);
    extern std::atomic<bool> g_ovl_vi_mitm;
    extern std::atomic<u32> g_tesla_txns, g_tesla_presents;
    void OvlNoteTeslaTransaction(u32 code, const void *in, size_t in_size);

    /* at startup, before vi:m is registered: on (config overlays = 1) or
     * overlays.txt turns on the overlays, their thread and the Tesla wrap;
     * the overlay probe's file the wrap alone (ovl3 started the thread before
     * reading the file - it never ran) */
    void OverlayArm(bool on);
    bool OverlaysArmed();

    /* the VIC side. The engines lend the overlays a buffer area the VIC can
     * read (cpu/dev), while they are up; Down waits for the source thread to
     * leave it before the memory goes. */
    /* cached: plain cached memory for the CPU's work (the area is uncached) */
    void OverlayAreaUp(u8 *cpu, u32 dev, size_t size, u8 *cached, size_t cached_size);
    void OverlayAreaDown();
    /* the overlay blend test at the first engine setup said whether the
     * format reads alpha from the last byte (true: the overlays stream) */
    void OverlaySetCalibrated(bool ok);

    /* one overlay to blend over this frame: its surface (block-linear,
     * 16-GOB blocks), and its place on a 1280x720 screen */
    struct OverlayPlane {
        u32 dev;                         /* device address of the surface */
        u32 w, h;                        /* the surface, pixels */
        u32 fmt;                         /* VIC pixel format */
        u32 x, y;                        /* where it goes on a 1280x720 screen */
        u32 dw, dh;                      /* and how big it is there (w, h unless it was copied smaller) */
    };
    /* the overlays showing now, bottom first (0..max) */
    u32 OverlayPlanes(OverlayPlane *out, u32 max);

}
