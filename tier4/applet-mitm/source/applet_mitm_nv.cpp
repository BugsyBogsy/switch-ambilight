/*
 * applet-mitm - hand-rolled nvdrv access + VIC hardware blit.
 *
 * Stage 1 (M4b..M7d, verified on hardware): the sequence below hand-rolls the
 * nvdrv IPC (libnx's nvInitialize() fatals a sysmodule), imports the game's
 * swapchain nvmap, surveys engines, allocates our own linear buffer, and shows
 * the VIC channel is usable. It then RELEASES everything - holding an nvdrv
 * session wedged homebrew.
 *
 * Stage 2 (M8, this file): TryVicBlit() does the real thing. One
 * NVHOST_IOCTL_CHANNEL_SUBMIT that kicks the VIC to de-swizzle + format-convert
 * a crop of the live frame (block-linear, kind 0xFE) into a linear buffer we
 * own, then waits the syncpoint and checksums the result. No MAP_CMD_BUFFER
 * (it crashed nvservices); the reloc list lets the kernel pin per-submit.
 *
 * nvdrv IPC (libnx nx/source/services/nv.c):
 *   cmd 0 Open      : InBuffer(MapAlias) = path -> {u32 fd, u32 error}
 *   cmd 1 Ioctl     : in {u32 fd, u32 request}, out u32 error, AutoSelect in+out = argp
 *   cmd 2 Close     : in {u32 fd}, out u32 error
 *   cmd 3 Initialize: in u32 transfermem_size, handles {process, tmem}
 *
 * CHANNEL_SUBMIT arg layout (switchbrew NV_services, libnx nvchannel.c):
 *   u32 num_cmdbufs, num_relocs, num_syncpt_incrs, num_fences;
 *   cmdbuf      cmdbufs[n]      {u32 mem, offset, words}                 (12)
 *   reloc       relocs[n]       {u32 cmdbuf_mem, cmdbuf_offset, target, target_offset} (16)
 *   reloc_shift reloc_shifts[n] {u32 shift}                             (4)
 *   syncpt_incr syncpt_incrs[n] {u32 syncpt_id, syncpt_incrs, rsvd[3]}  (20)
 *   u32         fence_thresholds[n]  (out)                              (4)
 * request = _NV_IOWR(0, 0x01, sizeof) -> 0xC0??0001 with ?? = total size.
 */
#include "applet_mitm_nv.hpp"
#include "applet_mitm_sm.hpp"
#include "applet_mitm_log.hpp"
#include "applet_mitm_dmdbg.hpp"
#include "applet_mitm_uisource.hpp"
#include "vic40_config.hpp"
#include "nvenc_drv_h264.h"
#include "applet_mitm_nvjpg.hpp"
#include "nvjpg_drv.h"
#include "applet_mitm_clk.hpp"
#include "nvjpg_dec_control.h"
#include "nvenc_grc_idr.h"
#include "nvenc_grc_hdrs.h"
#include "vic_csc_bt709.h"
#include "nvenc_grc_p.h"
#include "nvenc_1080p.h"
#include "applet_mitm_dbgpump.hpp"
#include "applet_mitm_shot.hpp"
#include "applet_mitm_control.hpp"
#include "applet_mitm_donor.hpp"
#include "applet_mitm_audio.hpp"
#include "applet_mitm_uvc.hpp"
#include "applet_mitm_net.hpp"
#include "applet_mitm_ambi_core.hpp"
#include "ambi_runtime.hpp"
#include "applet_mitm_slotmatch.hpp"
#include "applet_mitm_idlework.hpp"
#include "applet_mitm_ovlprobe.hpp"
#include "applet_mitm_overlay.hpp"
#include <atomic>
#include <cstring>
#include <cstdio>
#include <utility>

namespace ams::mitm::applet {

    constinit GameSurface g_game_surface = {};
    constinit GameSurface g_applet_surface = {};   /* v0.7.14 */
    constinit std::atomic<u64> g_applet_target_pid{0};
    constinit std::atomic<u64> g_present_pid{0};
    constinit std::atomic<u64> g_app_present_tick{0};

    namespace {
        /* v0.7.17: a web applet is open and the game has stopped drawing -
         * Super Mario 3D All-Stars' "-" menu (OfflineWeb). It grows the
         * Applet pool from there ("back to the game menu" ran it out with
         * our 41 MB in it: LibAppletOf aborted 2003-0008, the console went
         * down), so the donor's memory goes back at once and stays back
         * until the game draws again. */
        bool WebMenuUp() {
            const u64 lp = g_app_present_tick.load(std::memory_order_relaxed);
            if (lp != 0 && armTicksToNs(armGetSystemTick() - lp) < UINT64_C(500000000)) { return false; }
            return WebAppletOpen();
        }
    }
    /* the record a running session's geometry came from: a new buffer set
     * there restarts it (the game's, or v0.7.14 the Album's / hbmenu's) */
    constinit const GameSurface *g_session_surface = &g_game_surface;
    constinit bool g_vic_armed   = false;
    constinit bool g_vic_execute = false;
    constinit std::atomic<const char *> g_vic_stage{"idle"};
    constinit u64 g_game_aruid = 0;
    constinit bool g_dbg_armed = false;
    constinit u32  g_pmdmnt_rc = 0xFFFFFFFF;

    /* M59 streaming prototype. 480x270 default: the only size that fits 60 fps
     * on USB 2.0 (518,400 B/frame = 12.4 ms transfer against a 16.67 ms budget).
     * 640x360 is selectable but caps near 45 fps until NV12 or SuperSpeed. */
    constinit bool g_bench_armed   = false;
    constinit bool g_nvenc_armed   = false;
    constinit bool g_sweep_armed   = false;
    constinit bool g_stream_auto   = true;   /* pick the size from the link speed */
    constinit bool g_matrix_armed  = false;
    constinit bool g_grcscan_armed = false;
    constinit bool g_jpgdec_armed  = false;
    constinit bool g_nvgrc_armed   = false;
    constinit bool g_csc_armed     = false;
    constinit bool g_nvframe_armed = false;
    constinit u32  g_nvframe_n     = 120;
    constinit bool g_nvstream_armed = false;
    constinit u32  g_nvstream_n    = 3600;
    constinit u32  g_nvstream_qp   = 20;
    constinit u32  g_nvstream_gop  = 0;
    constinit bool g_live_armed    = false;
    constinit bool g_ambi_armed    = false;
    /* Default on: release the debug attach when the game stops presenting so
     * HOME / Album is not fighting an active capture session. Set
     * detach_on_home=0 (arm "nodethome") to keep showing the suspended game. */
    constinit std::atomic<bool> g_detach_on_home{true};
    /* Present stall is a probe only (loads pause queueBuffer too). Confirm with
     * HOME button / Album / applet, else free after this fallback (v0.7.16: 5 s). */
    constinit u32 g_home_detach_ms   = 200;
    constinit u32 g_home_fallback_ms = 5000;
    constinit bool g_nv1080_armed  = false;
    constinit bool g_cap720_armed  = false;
    constinit bool g_anyfw_armed   = false;
    constinit u32  g_nv1080_n      = 120;
    constinit bool g_nvp_armed     = false;
    constinit u32  g_nvp_n         = 30;
    constinit u32  g_matrix_mode   = 0;
    constinit bool g_stream_armed  = false;
    constinit u32  g_stream_w      = 480;
    constinit u32  g_stream_h      = 270;
    constinit u32  g_stream_frames = 600;

    namespace {

        constinit std::atomic<bool> g_blit_done{false};

        /* Static so the allocator is never in the path. v0.7.10: it must be
         * our own memory - it cannot live in the donor's (applet_mitm_donor):
         * transfer memory cannot be made from memory that is itself lent
         * (vb:2_tmem failed, test10). */
        /* v0.7.13: nvdrv's transfer memory (256 KB) comes from the donor with
         * its heap (sftap command 12) - it was .bss, in the System group */

        /* VIC buffers live on the REAL process heap, not in .bss.
         *
         * nvmap ALLOC binds the caller's pages, but a device-shared buffer also
         * has to have its CPU mapping marked uncached - libnx's nvMapCreate does
         * armDCacheFlush + svcSetMemoryAttribute(.., 8, 8) for exactly this. That
         * SVC is only permitted on MemoryState_Normal, i.e. heap; a sysmodule's
         * .bss is code-mutable and it would be rejected. Every previous run had
         * cached, non-shared .bss standing in for device memory, which is why the
         * engine reported OP_DONE and we read back nothing. */
        constexpr size_t VicCfgSize = 0x4000;    /* VicConfigStruct (1552 B) */
        constexpr size_t VicCmdSize = 0x1000;    /* host1x pushbuf           */
        /* M59: 1 MB, sized for a streamed frame rather than the 64x64 self-blit.
         * 640x360 RGBA is 921,600 B; 480x270 is 518,400. The old 0x10000 held
         * neither. */
        /* M63: 3.5 MB so a full 1920x1080 NV12 frame (3,110,400 B) fits, which
         * is both the widest useful VIC output and exactly what NVENC wants as
         * input. */
        constexpr size_t VicDstSize = 0x340000;  /* linear output            */
        constexpr size_t VicSrcSize = 0x10000;   /* a SOURCE WE OWN, for the self-blit */
        /* A fixed 8 MB request was refused with os::ResultOutOfMemory (0x1003):
         * a sysmodule's heap is capped well below that. Ask for the largest the
         * process will actually grant rather than guessing, and size the capture
         * to whatever we get. 2 MB granularity is the AllocateMemoryBlock unit. */
        constexpr size_t VicBufsEnd  = 0x360000;  /* cfg+cmd+dst+src all live below this */
        /* v0.7.10: the dmnt:cht bounce used to be 2 MB of .bss for the whole
         * boot, in the Applet pool the browser applets need nearly all of
         * (2210-1024); it now sits here, taken when a viewer first connects.
         * Not nvmap-mapped (it must not be). */
        constexpr size_t VicBounceOff = 0x400000;                   /* 2 MB: dmnt:cht read bounce */
        constexpr size_t VicBounceSize = 0x200000;
        constexpr size_t VicBounceOverlayOff = 0xC0000;            /* v0.7.14: the overlays' cached working memory from here */
        /* the heap held once streaming has started - engine buffers and the above */
        constexpr size_t VicBaseHeap = VicBounceOff + VicBounceSize;
        static_assert(VicBufsEnd <= VicBounceOff);
        /* M59 RELAYOUT - the blocker M56 predicted. Staging used to start
         * 983,040 B into the capture region, which collided with a resident
         * full frame (8,294,400 B). It now starts past a whole slot. */
        constexpr size_t FbSlotBytes     = 8847360;
        constexpr size_t FbBlockRowStage = FbSlotBytes;
        /* M62: 0x220000 so a 960x540 frame (2,073,600 B) fits. (The 16 MB
         * heap this was once sized against is gone: the capture region is the
         * donor's memory, and MinCaptureBytes below is what it must hold.) */
        constexpr size_t StreamStageSize = 0x220000;   /* per stream stage buffer */
        constinit size_t g_vic_heap_size = 0;

        /* The capture buffer stays on the HEAP, deliberately.
         *
         * Moving it to a 4 MB .bss array to dodge the 2 MB heap cap fataled a
         * different sysmodule (0100000000000023) with 0x10801 LimitReached at
         * boot: a sysmodule's static memory is drawn from the shared system
         * pool (pool_partition 2), so taking 4 MB of it starves everyone else.
         * This project already learned that once, when the early recon module
         * carried 9.3 MB of .bss.
         *
         * Heap is the safe place, so we live within the 2 MB ceiling and let
         * the resolution picker choose whatever actually fits. */
        constinit size_t g_ind_size = 0;

        constinit uintptr_t g_vic_heap    = 0;
        constinit bool g_engine_wedged = false;   /* v0.7.10: up here - CaptureRelease checks it */
        constinit u8       *g_vic_cfg_buf = nullptr;
        constinit u8       *g_vic_cmd_buf = nullptr;
        constinit u8       *g_vic_dst_buf = nullptr;
        constinit u8       *g_vic_src_buf = nullptr;
        constinit u8       *g_ind_buf     = nullptr;
        /* v0.7.13: past the stream's g_ind_size, the capture region's tail holds
         * the screenshot PNG's buffer, the audio stage and the audio ring -
         * memory from the donor while a viewer is connected, instead of 224 KB
         * of .bss in the System group the boot needs (#15) */
        constexpr size_t AudioStageSize = 32 * 1024;   /* ~170 ms - more than a frame ever carries */
        constexpr size_t CaptureTailBytes = ShotOutBufSize + AudioStageSize + AudioRingBytes;
        /* v0.7.14: what a donor's capture region must hold - a whole slot copy,
         * the stream stages CaptureAcquire points past it, and the tail. The
         * donor's smallest size (tier4/donor/source/main.c sizes_mb[], less its
         * 256 KB of nvdrv transfer memory) has to cover it with the engine
         * buffers. */
        constexpr size_t MinCaptureBytes = FbBlockRowStage + 3 * StreamStageSize + CaptureTailBytes;
        constexpr size_t SmallestDonorBytes = 22 * 1024 * 1024 - 0x40000;
        static_assert(VicBaseHeap + MinCaptureBytes <= SmallestDonorBytes,
                      "the donor's smallest size must hold the engine buffers and the capture region");
        constinit u8       *g_shot_out    = nullptr;
        constinit u8       *g_audio_stage = nullptr;
        constinit u8       *g_audio_ring  = nullptr;
        void SetCaptureTail(u8 *tail) {
            g_shot_out    = tail;
            g_audio_stage = tail != nullptr ? tail + ShotOutBufSize : nullptr;
            g_audio_ring  = tail != nullptr ? tail + ShotOutBufSize + AudioStageSize : nullptr;
        }
        /* cached heap past the nvmap'd block-row, used to stage uncached engine
         * output before it goes to the filesystem */
        constinit u8       *g_stage_buf   = nullptr;
        /* M59: two stage buffers so the USB transfer of frame N overlaps the
         * capture of frame N+1. Each holds [32-byte header][frame], posted as a
         * single URB - one transfer per frame, not two. */
        constinit u8       *g_stream_stage[2] = { nullptr, nullptr };

        /* M57: usbDs requires a 0x1000-aligned buffer, so the 32-byte frame
         * header cannot be posted from the stack. */
        alignas(0x1000) constinit u8 g_usb_hdr[0x1000] = {};
        /* v0.3: queued game audio, sent as its own packet before each frame */


        /* M54: the decisive measurement. LogMemoryPools() in applet_mitm_main.cpp
         * is in an anonymous namespace, so it cannot be reached from here - this
         * is the same survey, taken either side of the heap grab.
         *
         * pool_partition moved 2 -> 1, so the bytes must now come out of Applet
         * (511,936 KB free at boot) and NOT out of System (15,452 KB), which is
         * the pool am was starved of when M50 fataled the console. Measuring
         * both sides is the point: the kernel chain says Applet, but M49's
         * reading was reasoning that turned out to be wrong, and only the
         * before/after delta shows which pool actually drained. */
        void LogPools(const char *when) {
            static const char *const names[4] = { "Application", "Applet", "System", "SystemUnsafe" };
            for (u64 pool = 0; pool < 4; ++pool) {
                u64 tot = 0, used = 0;
                const auto r1 = ::ams::svc::GetSystemInfo(std::addressof(tot),  ::ams::svc::SystemInfoType_TotalPhysicalMemorySize, ::ams::svc::InvalidHandle, pool);
                const auto r2 = ::ams::svc::GetSystemInfo(std::addressof(used), ::ams::svc::SystemInfoType_UsedPhysicalMemorySize,  ::ams::svc::InvalidHandle, pool);
                if (R_FAILED(r1) || R_FAILED(r2)) {
                    LogLine("   [%s] pool %llu %-12s rc=%x/%x", when,
                            static_cast<unsigned long long>(pool), names[pool], r1.GetValue(), r2.GetValue());
                    continue;
                }
                LogLine("   [%s] pool %llu %-12s used=%8llu KB  free=%9lld KB", when,
                        static_cast<unsigned long long>(pool), names[pool],
                        static_cast<unsigned long long>(used / 1024),
                        static_cast<long long>((static_cast<s64>(tot) - static_cast<s64>(used)) / 1024));
            }
        }

        /* v0.7.10: the VIC heap and capture region come from the memory donor
         * (applet_mitm_donor.hpp), not our own heap */
        constinit bool g_donor_mode = false;

        void SetVicLayout(uintptr_t addr, size_t size) {
            g_vic_heap_size = size;
            g_vic_heap    = addr;
            g_vic_cfg_buf = reinterpret_cast<u8 *>(addr + 0x0000);
            g_vic_cmd_buf = reinterpret_cast<u8 *>(addr + 0x4000);
            g_vic_dst_buf = reinterpret_cast<u8 *>(addr + 0x10000);
            g_vic_src_buf = reinterpret_cast<u8 *>(addr + 0x350000);
            g_ind_buf = nullptr;
            SetCaptureTail(nullptr);
            g_ind_size = 0;
            g_stage_buf = nullptr;
            g_stream_stage[0] = g_stream_stage[1] = nullptr;
            std::memset(reinterpret_cast<void *>(addr), 0, VicBaseHeap);
        }

        bool AllocVicHeap() {
            if (g_vic_heap != 0) { return true; }

            {
                uintptr_t daddr = 0;
                size_t dsize = 0;
                if (DonorObtain(std::addressof(daddr), std::addressof(dsize))) {
                    /* v0.7.14: room for MinCaptureBytes - the gate was 6 MB,
                     * and a 12-20 MB donor (a donor before v0.7.14 descends
                     * that far when the Applet pool refuses a size) let the
                     * first frame's read run past the capture region */
                    if (dsize >= VicBaseHeap + MinCaptureBytes) {
                        g_donor_mode = true;
                        SetVicLayout(daddr, VicBaseHeap);
                        g_vic_heap_size = dsize;
                        LogLine("   VIC heap from the donor: %zu MB at 0x%lx (engine buffers %zu MB, capture region %zu MB)",
                                dsize >> 20, static_cast<unsigned long>(daddr), VicBaseHeap >> 20, (dsize - VicBaseHeap) >> 20);
                        return true;
                    }
                    LogLine("   donor gave only %zu MB (streaming needs %zu)", dsize >> 20, (VicBaseHeap + MinCaptureBytes + 0xFFFFF) >> 20);
                    DonorRelease();
                } else {
                    LogLine("   no donor");
                }
                /* v0.7.10: the module lives in the System pool, where every
                 * custom sysmodule shares a few MB - never take the streaming
                 * memory from there */
                if (g_release_mode) { LogLine("   not streaming: no memory from the donor (is atmosphere/contents/0100000000000C21 installed?)"); return false; }
            }

            size_t want = 0;
            /* measurement point 2 of 3: what the budget looks like at PROBE
             * time, with a game resident - to be compared against the boot
             * figure. 5 MB and 3 MB are dropped from the ladder: they returned
             * 0xca01 (kernel InvalidSize) because svcSetHeapSize requires 2 MB
             * granularity, so they were never valid requests and only added
             * noise. */
            {
                u64 t = 0, u = 0;
                ::ams::svc::GetInfo(std::addressof(t), ::ams::svc::InfoType_TotalMemorySize, ::ams::svc::PseudoHandle::CurrentProcess, 0);
                ::ams::svc::GetInfo(std::addressof(u), ::ams::svc::InfoType_UsedMemorySize,  ::ams::svc::PseudoHandle::CurrentProcess, 0);
                LogLine("   [probe, BEFORE heap grab] process total=%llu KB used=%llu KB free=%lld KB",
                        static_cast<unsigned long long>(t / 1024),
                        static_cast<unsigned long long>(u / 1024),
                        static_cast<long long>((static_cast<s64>(t) - static_cast<s64>(u)) / 1024));
            }
            /* M54 widens the ladder, and the precondition the M51 comment named
             * has now been met: pool_partition is 1 (Applet). Verified chain,
             * not assumed - ldr_process_creation.cpp:464 maps pool_partition 1
             * to CreateProcessFlag_PoolPartitionApplet, KProcess::Initialize
             * threads that pool into m_allocate_option
             * (kern_k_page_table_base.cpp:513), and SetHeapSize allocates
             * through exactly that option (same file, line 1930).
             *
             * This does NOT raise our ceiling: pm_spec.cpp:145 picks the
             * resource-limit group from application_type, never from
             * pool_partition, so we stay in the System group at ~14 MB. What
             * changes is whose memory we spend. 2 MB granularity is a kernel
             * requirement - 5 MB and 3 MB return 0xca01 (InvalidSize) - and the
             * ladder still descends, so a refusal costs us nothing. */
            LogPools("before heap grab");
            /* M56 actually spends it. M55 measured 411,260 KB free at probe with
             * BOTH gates on Applet (pool_partition 1, application_type 2), so
             * this is a request against a measured number rather than an
             * inferred one - the distinction M49/M54 got wrong.
             *
             * Sizes are 2 MB multiples: the kernel returns 0xca01 (InvalidSize)
             * otherwise, which is what made 5 MB and 3 MB look like refusals.
             *
             * 8 MB is deliberately NOT the top rung. VicBufsEnd is 0x30000, so
             * an 8 MB heap leaves 8,192,000 B of capture region against a
             * 8,294,400 B frame - 102 KB short. 10 MB is the first rung where a
             * full 1080p frame physically fits. The ladder descends, so a
             * refusal costs nothing and we still learn the real ceiling. */
            /* M63: reaches 24 MB. M55 measured 411,260 KB free in the Applet
             * resource-limit group at probe time, so 16 MB was never the
             * ceiling - it was simply the largest rung the ladder offered. */
            /* M85: 32 and 28 MB first. A P-frame stream needs its reference
             * pictures and ME buffers past the stream stages, which ends at
             * ~22.1 MB of capture region; 24 MB gives 21.1. The Applet group had
             * ~400 MB free at probe (M55), and a refusal just descends. */
            /* M94: 40 and 36 MB first - the 1080p pipeline adds a second 3 MB
             * picture to a 14 MB arena. */
            /* v0.7.10: only the engine buffers (cfg, cmd, guard pages, dst, our
             * own source - VicBufsEnd, 3.4 MB) are held for the boot. The
             * capture region (frame copy, encode arena, stream stages, ~36 MB)
             * is taken by CaptureAcquire when a stream starts and given back
             * by CaptureRelease when there is none: 40 MB held from the first
             * game on left the Applet pool short, and the web applet (Smash's
             * news, SooraMaru) crashed - even with streaming off. */
            want = VicBaseHeap;
            if (const auto rc = os::SetMemoryHeapSize(want); R_FAILED(rc)) {
                LogLine("   SetMemoryHeapSize(%zu MB) rc=0x%x - no heap", want / (1024 * 1024), rc.GetValue());
                return false;
            }
            uintptr_t addr = 0;
            if (const auto rc = os::AllocateMemoryBlock(std::addressof(addr), want); R_FAILED(rc)) {
                LogLine("   AllocateMemoryBlock(%zu MB) FAILED rc=0x%x", want / (1024 * 1024), rc.GetValue());
                return false;
            }
            LogPools("after heap grab");
            g_vic_heap_size = want;
            g_vic_heap    = addr;
            g_vic_cfg_buf = reinterpret_cast<u8 *>(addr + 0x0000);
            g_vic_cmd_buf = reinterpret_cast<u8 *>(addr + 0x4000);
            g_vic_dst_buf = reinterpret_cast<u8 *>(addr + 0x10000);
            g_vic_src_buf = reinterpret_cast<u8 *>(addr + 0x350000);
            g_ind_buf = nullptr;
            SetCaptureTail(nullptr);
            g_ind_size = 0;
            g_stage_buf = nullptr;
            g_stream_stage[0] = g_stream_stage[1] = nullptr;
            std::memset(reinterpret_cast<void *>(addr), 0, want);
            LogLine("   VIC heap %zu MB at 0x%lx (engine buffers only; the capture region is taken per stream): cfg=%p cmd=%p dst=%p src=%p",
                    want / (1024 * 1024), static_cast<unsigned long>(addr),
                    static_cast<void *>(g_vic_cfg_buf), static_cast<void *>(g_vic_cmd_buf),
                    static_cast<void *>(g_vic_dst_buf), static_cast<void *>(g_vic_src_buf));
            return true;
        }

        /* nvmap handles on the capture region not yet freed (NvfSession
         * creates two per session: the slot copy and the arena) - the region
         * is never given back while any is alive */
        constinit u32 g_capture_handles = 0;

        /* v0.7.10: the capture region, right after the engine buffers. The
         * ladder is the old one less the 4 MB base: 1080p pipelining wants the
         * arena up to ~31.5 MB; a refusal just descends (720p needs ~22 MB). */
        bool CaptureAcquire(const char *who) {
            if (g_ind_buf != nullptr) { return true; }
            if (g_vic_heap == 0) { return false; }
            if (g_donor_mode) {
                const size_t cap = g_vic_heap_size - VicBaseHeap - CaptureTailBytes;
                g_ind_buf  = reinterpret_cast<u8 *>(g_vic_heap + VicBaseHeap);
                g_ind_size = cap;
                SetCaptureTail(g_ind_buf + cap);
                g_stage_buf = g_ind_buf + FbBlockRowStage;
                g_stream_stage[0] = g_stage_buf + StreamStageSize;
                g_stream_stage[1] = g_stream_stage[0] + StreamStageSize;
                std::memset(g_ind_buf, 0, cap);
                LogLine("   %s: capture memory (donor): %zu MB at %p", who, cap / (1024 * 1024), static_cast<void *>(g_ind_buf));
                return true;
            }
            size_t total = 0;
            /* v0.7.10: every rung 2 MB up with the base heap, so the capture
             * region is exactly the size it was (frame rate and latency unchanged) */
            for (const size_t sz : { 42_MB, 38_MB, 34_MB, 30_MB, 26_MB, 22_MB, 18_MB, 14_MB, 12_MB }) {
                if (R_SUCCEEDED(os::SetMemoryHeapSize(sz))) { total = sz; break; }
            }
            if (total == 0) { LogLine("   %s: capture memory: no heap size accepted - the Applet pool is short (an applet open?)", who); return false; }
            const size_t block = total - VicBaseHeap;
            const size_t cap = block - CaptureTailBytes;
            uintptr_t addr = 0;
            if (const auto rc = os::AllocateMemoryBlock(std::addressof(addr), block); R_FAILED(rc)) {
                LogLine("   %s: capture memory: AllocateMemoryBlock(%zu MB) rc=0x%x", who, cap / (1024 * 1024), rc.GetValue());
                static_cast<void>(os::SetMemoryHeapSize(VicBaseHeap));
                return false;
            }
            g_ind_buf  = reinterpret_cast<u8 *>(addr);
            g_ind_size = cap;
            SetCaptureTail(g_ind_buf + cap);
            g_stage_buf = g_ind_buf + FbBlockRowStage;
            g_stream_stage[0] = g_stage_buf + StreamStageSize;
            g_stream_stage[1] = g_stream_stage[0] + StreamStageSize;
            g_vic_heap_size = total;
            std::memset(g_ind_buf, 0, cap);
            LogLine("   %s: capture memory taken: %zu MB at %p (heap %zu MB)", who, cap / (1024 * 1024), static_cast<void *>(g_ind_buf), total / (1024 * 1024));
            return true;
        }

        /* the capture region back to the system - only with no nvmap handle
         * on it and no engine job that may still write it */
        void CaptureRelease(const char *who) {
            if (g_ind_buf == nullptr) { return; }
            static bool said_kept = false;
            if (g_engine_wedged || g_capture_handles != 0) {
                if (!said_kept) {
                    LogLine("   %s: capture memory kept (%s)", who, g_engine_wedged ? "an engine job never completed" : "an nvmap handle on it is still alive");
                    said_kept = true;
                }
                return;
            }
            if (g_donor_mode) {
                /* the donor's memory stays mapped (experiment 1) */
                g_ind_buf = nullptr;
                SetCaptureTail(nullptr);
                g_ind_size = 0;
                g_stage_buf = nullptr;
                g_stream_stage[0] = g_stream_stage[1] = nullptr;
                LogLine("   %s: capture memory (donor) released", who);
                return;
            }
            os::FreeMemoryBlock(reinterpret_cast<uintptr_t>(g_ind_buf), g_ind_size + CaptureTailBytes);
            const auto rc = os::SetMemoryHeapSize(VicBaseHeap);
            LogLine("   %s: capture memory given back (%zu MB), heap back to %zu MB rc=0x%x", who, g_ind_size / (1024 * 1024), VicBaseHeap / (1024 * 1024), rc.GetValue());
            g_ind_buf = nullptr;
            SetCaptureTail(nullptr);
            g_ind_size = 0;
            g_stage_buf = nullptr;
            g_stream_stage[0] = g_stream_stage[1] = nullptr;
            g_vic_heap_size = VicBaseHeap;
        }

        /* Phase B geometry: a scale-free 64x64 crop of the frame's top-left.
         * The output STRIDE is aligned to 256 pixels, not to the width: that is
         * what libdrm's vic_image_new does for every VIC surface, pitch-linear
         * included (align = 256, stride = ALIGN(width, align)). 64 px of stride
         * would be a 256-byte pitch, well under what the engine expects. */
        constexpr u32 DstW        = 64;
        constexpr u32 DstH        = 64;
        constexpr u32 DstStridePx = 256;                 /* 1024 B pitch */
        constexpr u32 DstPitch    = DstStridePx * 4;
        constexpr u32 DstSize     = DstPitch * DstH;     /* 65536 */
        static_assert(DstSize <= VicDstSize);

        constinit ::Service        g_nv_srv  = {};
        constinit os::NativeHandle g_nv_tmem_handle = os::InvalidNativeHandle;
        constinit u64 g_nv_tmem_size = 0;

        /* Linux-style ioctl encoding: (dir<<30)|(size<<16)|(type<<8)|nr, IOWR dir=3 */
        constexpr u32 MakeIowr(u32 type, u32 nr, u32 size) {
            return (UINT32_C(3) << 30) | (size << 16) | (type << 8) | nr;
        }
        constexpr u32 MakeIow(u32 type, u32 nr, u32 size) {
            return (UINT32_C(1) << 30) | (size << 16) | (type << 8) | nr;
        }
        constexpr u32 NvmapIocCreate = MakeIowr(0x01, 0x01, 8);
        constexpr u32 NvmapIocFromId = MakeIowr(0x01, 0x03, 8);
        constexpr u32 NvmapIocAlloc  = MakeIowr(0x01, 0x04, 32);
        constexpr u32 NvmapIocGetId  = MakeIowr(0x01, 0x0E, 8);
        constexpr u32 NvmapIocFree   = MakeIowr(0x01, 0x05, 24);   /* v0.7.10: libnx nvioctlNvmap_Free */
        /* {u64 aruid; u32 handle; u8 pad[4]} - asks whether an nvmap object is
         * bound to a given AppletResourceUserId. A pure query, so it doubles as
         * a safe way to DISCOVER the game's aruid. */
        constexpr u32 NvmapIocIsOwnedByAruid = MakeIow(0x01, 0x13, 16);   /* 0x40100113 */

        constexpr u32 NvHostIocChannelGetSyncpoint  = MakeIowr(0x00, 0x02, 8);   /* {u32 module_id; u32 syncpt}   */
        constexpr u32 NvHostIocChannelSetSubmitTo   = MakeIow (0x00, 0x07, 4);   /* {u32 timeout}                 */
        constexpr u32 NvHostIocChannelSetNvmapFd    = MakeIow (0x48, 0x01, 4);   /* {u32 fd}  -> 0x40044801       */
        /* MAP_CMD_BUFFER pins nvmap handles and RETURNS their device physical
         * address. Horizon replaced Linux's per-submit pinning with this, which
         * is why a submit carrying relocs for unpinned handles answers
         * InvalidState(8). Header is 12 B (num_handles, reserved, is_compr,
         * pad[3]) + 8 B per handle -> 20 for one. */
        constexpr u32 NvHostIocChannelMapCmdBuf     = MakeIowr(0x00, 0x09, 20);  /* -> 0xC0140009 */
        constexpr u32 NvHostIocChannelUnmapCmdBuf   = MakeIowr(0x00, 0x0A, 20);
        constexpr u32 NvHostIocChannelMapCmdBufEx   = MakeIowr(0x00, 0x25, 20);   /* unpins on error */
        constexpr u32 NvHostIocCtrlSyncptRead       = MakeIowr(0x00, 0x14, 8);   /* {u32 id; u32 value}           */
        constexpr u32 NvHostIocCtrlSyncptWait       = MakeIowr(0x00, 0x16, 12);  /* {u32 id; u32 thresh; u32 to}  */
        constexpr u32 NvHostIocCtrlSyncptIncrW      = MakeIow (0x00, 0x15, 4);   /* {u32 id} - switchbrew 0x40040015 */
        constexpr u32 NvHostIocCtrlSyncptIncrWR     = MakeIowr(0x00, 0x15, 4);   /* libnx encodes this one instead   */

        struct MapCmdBufArgs {
            u32 num_handles;
            u32 reserved;
            u8  is_compr;
            u8  padding[3];
            u32 handle_id_in;
            u32 phys_addr_out;
        };
        static_assert(sizeof(MapCmdBufArgs) == 20);
        static_assert(__builtin_offsetof(MapCmdBufArgs, handle_id_in) == 12);

        /* breadcrumb + stage, so the heartbeat can name where a hang happened */
        /* M64b: LogMark does WriteWhole(applet-mitm.last) AND LogLine - two SD
         * writes per call. RunOneJob calls this on entry, so the stage
         * breadcrumb alone cost ~41 ms/job: the bench measured 0.83 ms of
         * engine time inside a 39,418 us wall clock. The atomic store is free
         * and the heartbeat still reports it, so quiet mode keeps the store and
         * drops the write. */
        /* declared here because VicStage below is the first user */
        constinit bool g_vic_quiet = false;

        void VicStage(const char *s) {
            g_vic_stage.store(s, std::memory_order_relaxed);
            if (!g_vic_quiet) { LogMark(s); }
        }

        ::Result NvIoctl(u32 fd, u32 request, void *argp, size_t argsz, u32 *out_err) {
            const struct { u32 fd; u32 request; } in = { fd, request };
            u32 error = 0;
            const ::Result rc = serviceDispatchInOut(std::addressof(g_nv_srv), 1, in, error,
                .buffer_attrs = {
                    SfBufferAttr_HipcAutoSelect | SfBufferAttr_In,
                    SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out,
                },
                .buffers = { { argp, argsz }, { argp, argsz } },
            );
            *out_err = error;
            return rc;
        }

        ::Result NvOpen(const char *path, u32 *out_fd, u32 *out_err) {
            struct { u32 fd; u32 error; } out = {};
            const ::Result rc = serviceDispatchOut(std::addressof(g_nv_srv), 0, out,
                .buffer_attrs = { SfBufferAttr_In | SfBufferAttr_HipcMapAlias },
                .buffers      = { { path, std::strlen(path) } });
            *out_fd  = out.fd;
            *out_err = out.error;
            return rc;
        }

        /* nvdrv cmd 7. Plain u64 in, u32 err out - no PID descriptor, so unlike
         * vi's OpenLayer this one is actually expressible for us. Needs
         * NvDrvPermission bit 10, which only nvdrv:t grants.
         *
         * libnx's nvInitialize does the equivalent for every normal client:
         *     u64 aruid = appletGetAppletResourceUserId();
         *     if (aruid) _nvSetClientPID(aruid);
         * Our session has never set one, so it is aruid 0 while the game's
         * buffers belong to the game's aruid. That asymmetry is the last
         * candidate explanation for the silent phys=0. */
        ::Result NvSetAruidWithoutCheck(u64 aruid, u32 *out_err) {
            u32 err = 0;
            const ::Result rc = serviceDispatchInOut(std::addressof(g_nv_srv), 7, aruid, err);
            *out_err = err;
            return rc;
        }

        bool NvIsOwnedByAruid(u32 fd, u64 aruid, u32 handle) {
            struct { u64 aruid; u32 handle; u32 pad; } a = { aruid, handle, 0 };
            u32 nverr = 0;
            const ::Result rc = NvIoctl(fd, NvmapIocIsOwnedByAruid, std::addressof(a), sizeof(a), std::addressof(nverr));
            return R_SUCCEEDED(rc) && nverr == 0;
        }

        void NvClose(u32 fd) {
            const struct { u32 fd; } in = { fd };
            u32 e = 0;
            serviceDispatchInOut(std::addressof(g_nv_srv), 2, in, e);
        }

        /* Pin one nvmap handle into the channel and get its device physical
         * address. Each call is breadcrumbed by the caller so that if this ever
         * takes nvservices down again we know exactly which handle did it. */
        ::Result NvmapOwn(u32 fd, void *cpu, u32 size, u8 kind, u32 *out_handle, u32 *out_id, bool cacheable = false);

        /* v0.7.1: device address 0 is a real address the channels hand out
         * when it is free, and to every check here 0 means "not pinned" (a zero
         * address hangs the VIC). The guard page mapped at setup only takes it
         * if it is free then; a later session found it freed and got it for its
         * slot buffer ("slot pin returned 0"). So whenever a mapping comes back
         * at 0: unmap it, map a fresh guard page from the unused gap in our
         * heap (it lands at 0 and stays there, never unmapped), map again. */
        constinit u32 g_guard_nvmap_fd = 0;
        constinit u32 g_guard_next = 0x9000;      /* 0x9000..0xF000: the gap between cmd and dst */

        bool MapCmdBuffer(u32 chan_fd, u32 handle, u32 *out_addr, const char *what, u8 is_compr = 0, u32 req = 0);

        bool AbsorbDeviceZero(u32 chan_fd, u32 req) {
            if (g_guard_nvmap_fd == 0 || g_vic_heap == 0 || g_guard_next >= 0x10000) { return false; }
            u32 gh = 0, gid = 0;
            if (R_FAILED(NvmapOwn(g_guard_nvmap_fd, reinterpret_cast<u8 *>(g_vic_heap + g_guard_next), 0x1000, 0, std::addressof(gh), std::addressof(gid), false))) { return false; }
            g_guard_next += 0x1000;
            MapCmdBufArgs a = {};
            a.num_handles  = 1;
            a.handle_id_in = gh;
            u32 nverr = 0;
            const ::Result rc = NvIoctl(chan_fd, req, std::addressof(a), sizeof(a), std::addressof(nverr));
            LogLine("   guard page mapped at 0x%x (rc=0x%x nverr=%u) - device address 0 is held from now on", a.phys_addr_out, rc, nverr);
            return R_SUCCEEDED(rc) && nverr == 0 && a.phys_addr_out == 0;
        }

        bool MapCmdBuffer(u32 chan_fd, u32 handle, u32 *out_addr, const char *what, u8 is_compr, u32 req) {
            MapCmdBufArgs a = {};
            a.num_handles  = 1;
            a.is_compr     = is_compr;
            a.handle_id_in = handle;
            u32 nverr = 0;
            if (req == 0) { req = NvHostIocChannelMapCmdBuf; }
            const ::Result rc = NvIoctl(chan_fd, req, std::addressof(a), sizeof(a), std::addressof(nverr));
            LogLine("   MAP_CMD_BUFFER(%s handle=%u compr=%u req=0x%08x) rc=0x%x nverr=%u -> phys=0x%x",
                    what, handle, is_compr, req, rc, nverr, a.phys_addr_out);
            if (R_FAILED(rc) || nverr != 0) { return false; }
            if (a.phys_addr_out == 0 && AbsorbDeviceZero(chan_fd, req)) {
                /* ours was at 0: move it off */
                MapCmdBufArgs u = {};
                u.num_handles  = 1;
                u.handle_id_in = handle;
                u32 ue = 0;
                NvIoctl(chan_fd, NvHostIocChannelUnmapCmdBuf, std::addressof(u), sizeof(u), std::addressof(ue));
                a = {};
                a.num_handles  = 1;
                a.is_compr     = is_compr;
                a.handle_id_in = handle;
                nverr = 0;
                const ::Result rc2 = NvIoctl(chan_fd, req, std::addressof(a), sizeof(a), std::addressof(nverr));
                LogLine("   MAP_CMD_BUFFER(%s) again -> phys=0x%x (rc=0x%x nverr=%u)", what, a.phys_addr_out, rc2, nverr);
                if (R_FAILED(rc2) || nverr != 0) { return false; }
            }
            *out_addr = a.phys_addr_out;
            return true;
        }

        void UnmapCmdBuffer(u32 chan_fd, u32 handle) {
            MapCmdBufArgs a = {};
            a.num_handles  = 1;
            a.handle_id_in = handle;
            u32 nverr = 0;
            NvIoctl(chan_fd, NvHostIocChannelUnmapCmdBuf, std::addressof(a), sizeof(a), std::addressof(nverr));
        }

        bool NvmapFree(u32 fd, u32 handle);

        /* v0.7.13.1: before a session pins its buffers, a guard page first. A
         * hole at device address 0 opens when another process's buffers that
         * held it go away (a homebrew app closing): the guard fills it and
         * stays; with 0 held already it is not needed and goes again. Without
         * it the slot copy could land on 0, which the VIC hangs on - a black
         * picture until a reboot. (Moving our own mapping off 0 afterwards
         * does not work: an unmapped handle keeps its address until freed.) */
        void HoldDeviceZero(u32 chan_fd) {
            if (g_guard_nvmap_fd == 0 || g_vic_heap == 0 || g_guard_next >= 0x10000) { return; }
            u32 gh = 0, gid = 0;
            if (R_FAILED(NvmapOwn(g_guard_nvmap_fd, reinterpret_cast<u8 *>(g_vic_heap + g_guard_next), 0x1000, 0, std::addressof(gh), std::addressof(gid), false))) { return; }
            MapCmdBufArgs a = {};
            a.num_handles  = 1;
            a.handle_id_in = gh;
            u32 nverr = 0;
            const ::Result rc = NvIoctl(chan_fd, NvHostIocChannelMapCmdBuf, std::addressof(a), sizeof(a), std::addressof(nverr));
            if (R_SUCCEEDED(rc) && nverr == 0 && a.phys_addr_out == 0) {
                g_guard_next += 0x1000;
                LogLine("   device address 0 was free - a guard page holds it now");
                return;
            }
            if (R_SUCCEEDED(rc) && nverr == 0) { UnmapCmdBuffer(chan_fd, gh); }
            static_cast<void>(NvmapFree(g_guard_nvmap_fd, gh));
        }

        /* CREATE + ALLOC(kind, our cpu pages) + GET_ID for a buffer we own. */
        ::Result NvmapOwn(u32 fd, void *cpu, u32 size, u8 kind, u32 *out_handle, u32 *out_id, bool cacheable) {
            u32 nverr = 0;
            struct { u32 size; u32 handle; } cr = { size, 0 };
            ::Result rc = NvIoctl(fd, NvmapIocCreate, std::addressof(cr), sizeof(cr), std::addressof(nverr));
            if (R_FAILED(rc) || nverr != 0) { LogLine("   nvmapOwn CREATE fail rc=0x%x nverr=%u", rc, nverr); return rc; }

            struct {
                u32 handle; u32 heapmask; u32 flags; u32 align;
                u8 kind; u8 pad[7]; u64 addr;
            } al = {};
            al.handle = cr.handle;
            al.flags  = cacheable ? 1u : 0u;      /* bit0 = CACHEABLE */
            al.align  = 0x1000;
            al.kind   = kind;
            al.addr   = reinterpret_cast<u64>(cpu);
            rc = NvIoctl(fd, NvmapIocAlloc, std::addressof(al), sizeof(al), std::addressof(nverr));
            if (R_FAILED(rc) || nverr != 0) { LogLine("   nvmapOwn ALLOC fail rc=0x%x nverr=%u", rc, nverr); return rc; }

            /* flags bit0 is CACHEABLE (libnx passes is_cpu_cacheable?1:0), not
             * read-write as the wiki's comment suggests. For a non-cacheable
             * buffer we owe the matching CPU-side step that libnx does and we
             * never did: flush, then mark the mapping uncached. Without it the
             * CPU keeps a cached, non-coherent view.
             *
             * The capture buffer is the exception. It is written by
             * ReadDebugProcessMemory - a kernel memcpy of 983,040 B, every
             * frame - and an uncached destination would cost far more than
             * cache maintenance does. It stays CACHED, and the caller flushes
             * it before the engine reads. */
            armDCacheFlush(cpu, size);
            if (!cacheable) {
                const auto sma = svc::SetMemoryAttribute(reinterpret_cast<uintptr_t>(cpu), size,
                                                         svc::MemoryAttribute_Uncached,
                                                         svc::MemoryAttribute_Uncached);
                LogLine("   SetMemoryAttribute(%p, 0x%x, uncached) rc=0x%x", cpu, size, sma.GetValue());
            } else {
                LogLine("   nvmapOwn(%p, 0x%x) kept CACHEABLE - caller must flush before submit", cpu, size);
            }

            struct { u32 id; u32 handle; } gi = { 0, cr.handle };
            rc = NvIoctl(fd, NvmapIocGetId, std::addressof(gi), sizeof(gi), std::addressof(nverr));
            *out_handle = cr.handle;
            *out_id     = gi.id;
            LogLine("   nvmapOwn ok handle=%u id=%u kind=%u size=0x%x", cr.handle, gi.id, kind, size);
            return rc;
        }

        /* v0.7.10: free one of our nvmap handles. Until now none was ever freed
         * (two per stream session); the capture region can only go back to
         * the system once nothing references it. True when nvmap let go of the
         * memory (flags bit 0, NOT_FREED_YET, clear). */
        /* The reply (yuzu's nvmap IocFree) is the freed memory's address and
         * size, and flags bit 0 = it was mapped uncached. Address and size 0:
         * our handle is gone but another reference (an internal dup) still
         * holds the memory. v0.7.10: test12 read flags bit 0 as "still
         * referenced", so every free counted as failed and the capture region
         * (and the donor's memory) was never given back. */
        bool NvmapFree(u32 fd, u32 handle) {
            struct { u32 handle; u32 pad; u64 address; u32 size; u32 flags; } a = { handle, 0, 0, 0, 0 };
            static_assert(sizeof(a) == 24);
            u32 nverr = 0;
            const ::Result rc = NvIoctl(fd, NvmapIocFree, std::addressof(a), sizeof(a), std::addressof(nverr));
            if (R_FAILED(rc) || nverr != 0) {
                LogLine("   nvmap FREE(handle %u) rc=0x%x nverr=%u - failed", handle, rc, nverr);
                return false;
            }
            if (a.address == 0 && a.size == 0) {
                LogLine("   nvmap FREE(handle %u): our handle is gone, the memory is still referenced elsewhere", handle);
                return false;
            }
            return true;
        }

        /* Three jobs, run back-to-back in one probe so a single reboot answers
         * everything:
         *   Fill       - no source at all. Isolates the OUTPUT path: config
         *                struct, dst address, EXECUTE, cache handling.
         *   BlitDirect - source address inlined from MAP_CMD_BUFFER.
         *   BlitReloc  - source address left to nvservices via a reloc, which
         *                knows the game buffer's address even though pinning
         *                would not tell us. */
        enum class VicJob { Fill, BlitSelf, BlitGame, BlitStrip };

        /* stride_px is what goes into SlotLumaWidth (+1). For pitch-linear that
         * is plainly the pixel stride. For BLOCK-linear nobody has told us
         * whether the field wants pixels or BYTES: libdrm only ever blits
         * pitch surfaces and marks both SlotBlkHeight and SlotCacheWidth
         * "XXX". So it is swept below rather than guessed. */
        /* w/h describe the SURFACE; rect_w/rect_h the region actually sampled.
         * They differ only for the unscaled 1:1 control. */
        /* rect_x/rect_y (v0.7.17): the sampled part's top-left - a centred crop
         * scaled by the VIC (Kingdom Hearts' zooms, for viewers without SFTC) */
        struct SrcDesc { u32 w, h, stride_px, blk_kind, blk_h_log2, pixfmt, cache_w, rect_w, rect_h; u32 rect_x = 0, rect_y = 0; };

        /* our own pitch-linear 64x64 buffer, stride 256 px like the destination */
        constexpr SrcDesc SelfSrc { DstW, DstH, DstStridePx, vic::BLK_KIND_PITCH, 0, vic::PIXFMT_A8B8G8R8, vic::CACHE_WIDTH_64Bx4, DstW, DstH };

        /* The OUTPUT format has been hardcoded A8B8G8R8 since M19 and never
         * swept. The 64x64 self-blit could not reveal an R<->B swap either: its
         * painted ramp differed in R and B only by a constant. */
        /* M82: block-linear output, for NVENC's input. NVENC reads GPU
         * block-linear only (its surface config has no pitch mode), with
         * grc's block height 2 = 32-row blocks. Pitch stays the default. */
        struct OutDesc { u32 w, h, stride_px, fmt; u32 blk_kind = vic::BLK_KIND_PITCH; u32 blk_h_log2 = 0; };

        /* the 64x64 self-blit target, unchanged: it is the byte-exact regression
         * reference and must keep producing identical output */
        constexpr OutDesc SelfOut { DstW, DstH, DstStridePx, vic::PIXFMT_A8B8G8R8 };

        /* One block-row of the GAME's surface: full 1920 px width by 128 rows,
         * block-linear, exactly what the binder parcels describe.
         *
         * NOTE the kind. SlotBlkKind is a 4-BIT field carrying the VIC's own
         * enum - 0 pitch, 1 generic 16Bx2 - not the nvmap kind 0xFE that the
         * parcels report. Writing 0xFE there truncates to 0xE and hands the
         * engine a surface layout that does not exist, and a bad layout hangs
         * the VIC, which takes the compositor and the console with it. */
        constexpr u32 StripW = 1920, StripH = 128;
        constexpr SrcDesc StripSrc { StripW, StripH, StripW,
                                     vic::BLK_KIND_GENERIC_16Bx2, 4, vic::PIXFMT_A8B8G8R8,
                                     vic::CACHE_WIDTH_64Bx4, StripW, StripH };
        static_assert(StripSrc.rect_w > 0 && StripSrc.rect_h > 0,
                      "a zero rect underflows to 0xFFFFFFFF in FillBlitConfig");
        /* set per variant before each BlitStrip submit */
        constinit SrcDesc g_strip_src = StripSrc;

        /* 4x downscale into the existing 64 KB destination. The stride is
         * aligned to 256 px the way libdrm aligns every VIC surface (M13):
         * 512 px = 2048 B, x 32 rows = 65536 B, exactly DstSize. */
        /* M41 produced a completed blit whose pixels were scrambled: the right
         * brightness structure, no coherent image. That is a source-LAYOUT
         * error, not scale or crop. Three fields could cause it and none has a
         * reference, so sweep them in one boot the way M16 settled SETCL. */
        /* M43 settled it, and it was never the layout. The engine's output came
         * back as [0xFF, G_src, B_src, A_src] - the source shifted down one lane
         * with alpha forced to max by ConstantAlpha/PlanarAlpha. Per-lane error
         * against a software de-swizzle of the same bytes:
         *
         *     aligned:  out[1]vsR 7.15   out[2]vsG 21.02   out[3]vsB 195.71
         *     shifted:  out[1]vsG 1.87   out[2]vsB  1.69   out[3]vsA   0.70
         *
         * Under 2 LSB on every lane once shifted - filter-difference magnitude.
         * So geometry, scale and block-linear addressing were all correct, and
         * the engine simply ate the source's R byte as alpha.
         *
         * The game's surface is R,G,B,A in memory, which the VIC calls
         * R8G8B8A8, not the A8B8G8R8 I declared. Sweep the three candidates
         * rather than assume, keeping the layout that already works. */
        /* M44's mapping table showed every format is a LOSSLESS permutation -
         * each output lane matched some source lane under 5 LSB, alpha included.
         * Nothing is destroyed; the channels are merely shuffled. P32_argb came
         * closest, with only R and B transposed:
         *
         *     out0(A)=srcA 1.11   out1(R)=srcB 4.12
         *     out2(G)=srcG 4.52   out3(B)=srcR 4.98
         *
         * So this sweeps SOURCE x OUTPUT format - the output side has been fixed
         * at A8B8G8R8 since M19 and never tested - and one of the four must be
         * the identity mapping.
         *
         * ONE2ONE is the separate question: the residual 4-5 LSB is either the
         * VIC's polyphase scaler differing from a box filter, or a small
         * sampling offset. An unscaled 480x32 crop cannot have a filter error,
         * so if it lands near zero the residual is filtering and harmless. */
        /* M45 settled the VIC completely. ONE2ONE - the unscaled 480x32 crop -
         * matched a software de-swizzle with err 0.00 on ALL FOUR LANES. Not
         * approximately: bit for bit. So block-linear addressing, GOB tiling,
         * stride and block height are exact, and the 2-3 LSB seen on scaled
         * variants is just the VIC's polyphase scaler differing from a box
         * filter - not an error.
         *
         * Channel order: S32O33 maps out=ABGR, a pure R<->B swap from identity.
         * Every variant is a LOSSLESS permutation, so this costs nothing to undo
         * on the PC, or by choosing NVENC's input format - which has to be
         * configured anyway. Not worth another docked run.
         *
         * Two variants kept as regression only. */
        struct StripVariant { const char *name; u32 src_fmt; u32 out_fmt; u32 rect_w, rect_h; };
        constexpr StripVariant StripVariants[] = {
            { "BEST",    vic::PIXFMT_A8R8G8B8, vic::PIXFMT_A8B8G8R8, StripW, StripH },
            { "ONE2ONE", vic::PIXFMT_A8R8G8B8, vic::PIXFMT_A8B8G8R8, 480,    32     },
        };
        constexpr u32 StripVariantCount = sizeof(StripVariants) / sizeof(StripVariants[0]);

        constexpr OutDesc StripOut { 480, 32, 512, vic::PIXFMT_A8B8G8R8 };
        constinit OutDesc g_strip_out = StripOut;   /* set per variant */
        /* v0.4.1: the present's flips, applied on the VIC's output - what the
         * compositor does for a game that renders upside down */
        constinit u32 g_strip_flip = 0;
        constexpr u32 StripOutSize = StripOut.stride_px * 4 * StripOut.h;
        static_assert(StripOutSize <= DstSize);

        void FillOutputConfig(vic::VicConfigStruct *c, const OutDesc &out) {
            c->outputConfig.TargetRectLeft   = 0;
            c->outputConfig.TargetRectTop    = 0;
            c->outputConfig.TargetRectRight  = out.w - 1;
            c->outputConfig.TargetRectBottom = out.h - 1;

            c->outputSurfaceConfig.OutPixelFormat   = out.fmt;
            c->outputSurfaceConfig.OutBlkKind       = out.blk_kind;
            c->outputSurfaceConfig.OutBlkHeight     = out.blk_h_log2;
            c->outputSurfaceConfig.OutSurfaceWidth  = out.w - 1;
            c->outputSurfaceConfig.OutSurfaceHeight = out.h - 1;
            c->outputSurfaceConfig.OutLumaWidth     = out.stride_px - 1;
            c->outputSurfaceConfig.OutLumaHeight    = out.h - 1;
            /* 16383 is the "no chroma plane" sentinel the RGBA path has always
             * used. NV12 needs real dimensions: the UV plane is half resolution
             * in both axes, interleaved, so it is stride/2 x h/2 pairs. */
            if (vic::IsNv12(out.fmt)) {
                c->outputSurfaceConfig.OutChromaWidth  = (out.stride_px / 2) - 1;
                c->outputSurfaceConfig.OutChromaHeight = (out.h / 2) - 1;

                /* ---- RGB -> YUV, the missing half of NV12 -----------------
                 * The first NV12 run produced a correct Y plane and a green
                 * image: green is what a decoder shows when U and V are both
                 * ~0, i.e. luma written and chroma left untouched. The engine
                 * will not do a colour-space conversion unless this matrix is
                 * enabled, and nothing in the project had ever set it - every
                 * prior blit was RGBA -> RGBA, where none is needed.
                 *
                 * VIC 4.0 packs the 3x4 matrix COLUMN-major: word j carries
                 * c0j, c1j, c2j, so word 3 is the offset column.
                 *   out_i = (sum_j c_ij * in_j + c_i3) >> r_shift
                 *
                 * BT.601 limited range at r_shift = 8:
                 *   Y =  ( 66R + 129G +  25B)/256 + 16
                 *   U =  (-38R -  74G + 112B)/256 + 128
                 *   V =  (112R -  94G -  18B)/256 + 128
                 * Offsets are carried at the same fixed-point scale (x256).
                 *
                 * Coefficients are 20-bit signed, so negatives are encoded two's
                 * complement in 20 bits. Getting these wrong is SAFE - it is
                 * arithmetic, not an address, so the worst case is wrong colour
                 * rather than a hung engine. */
                /* M64b: DISABLED BY DEFAULT. Enabling this matrix turned a
                 * correct luma plane (M63: recognisable image, chroma zero)
                 * into a constant 4 (M64: nothing at all). So the VIC already
                 * applies a sensible default RGB->YUV when the output format is
                 * YUV, and this override replaced it with something wrong -
                 * most likely the column-major packing or the fixed-point scale
                 * of the offset column is not what I inferred from the field
                 * names. Kept, behind "mtx" in the arm file, so the hypothesis
                 * can be retested without a rebuild. */
                auto s20 = [](int v) -> u64 { return static_cast<u64>(static_cast<u32>(v) & 0xFFFFFu); };
                auto &m = c->outColorMatrixStruct;
                /* M65b: the matrix goes on the SLOT, not the output.
                 *
                 * Evidence from the M65 dump: chroma IS written (U and V differ,
                 * correlation +0.888) but their means are 47 and 70 where
                 * neutral is 128, both spanning 0..255. That is raw channel
                 * pass-through, not a conversion - so the VIC does no implicit
                 * RGB->YUV and the matrix is mandatory.
                 *
                 * M64 enabled outColorMatrixStruct and luma collapsed to a
                 * constant 4. The VIC pipeline converts INPUT to its internal
                 * space at the slot (slotStruct[i].colorMatrixStruct) and only
                 * converts again on the way out; feeding RGB to a YUV output
                 * wants the conversion on the way IN. */
                if (!g_matrix_armed) { m.matrix_enable = 0; } else {
                m.matrix_coeff00 = s20(  66); m.matrix_coeff10 = s20( -38); m.matrix_coeff20 = s20( 112);
                m.matrix_coeff01 = s20( 129); m.matrix_coeff11 = s20( -74); m.matrix_coeff21 = s20( -94);
                m.matrix_coeff02 = s20(  25); m.matrix_coeff12 = s20( 112); m.matrix_coeff22 = s20( -18);
                m.matrix_coeff03 = s20(4096); m.matrix_coeff13 = s20(32768); m.matrix_coeff23 = s20(32768);
                m.matrix_r_shift = 8;
                m.matrix_enable  = 1;
                }
                /* the slot-side attempt, selected with mtx=2 in the arm file */
                if (g_matrix_mode == 2) {
                    m.matrix_enable = 0;              /* output matrix off */
                    auto &sm = c->slotStruct[0].colorMatrixStruct;
                    sm.matrix_coeff00 = s20(  66); sm.matrix_coeff10 = s20( -38); sm.matrix_coeff20 = s20( 112);
                    sm.matrix_coeff01 = s20( 129); sm.matrix_coeff11 = s20( -74); sm.matrix_coeff21 = s20( -94);
                    sm.matrix_coeff02 = s20(  25); sm.matrix_coeff12 = s20( 112); sm.matrix_coeff22 = s20( -18);
                    sm.matrix_coeff03 = s20(4096); sm.matrix_coeff13 = s20(32768); sm.matrix_coeff23 = s20(32768);
                    sm.matrix_r_shift = 8;
                    sm.matrix_enable  = 1;
                }
            } else {
                c->outColorMatrixStruct.matrix_enable = 0;
                c->outputSurfaceConfig.OutChromaWidth   = 16383;
                c->outputSurfaceConfig.OutChromaHeight  = 16383;
            }
        }

        /* Bytes of the luma plane, i.e. where the interleaved UV plane starts.
         * Must be 256-aligned because host1x carries addresses shifted right by
         * 8; every resolution we use satisfies that (1920x1080 = 256*8100).
         * M82: a block-linear plane occupies whole blocks, so its height
         * rounds up to 8 << blk_h_log2 rows (720 -> 736 at grc's 32). */
        constexpr u32 Nv12PlaneRows(u32 rows, const OutDesc &out) {
            const u32 bh = (out.blk_kind == vic::BLK_KIND_PITCH) ? 1u : (8u << out.blk_h_log2);
            return (rows + bh - 1) / bh * bh;
        }
        constexpr u32 Nv12LumaBytes(const OutDesc &out) { return out.stride_px * Nv12PlaneRows(out.h, out); }
        constexpr u32 Nv12ChromaBytes(const OutDesc &out) { return out.stride_px * Nv12PlaneRows(out.h / 2, out); }
        static_assert(Nv12LumaBytes(OutDesc{ 1280, 720, 1280, vic::PIXFMT_Y8_U8V8_N420, vic::BLK_KIND_GENERIC_16Bx2, 2 }) == 1280 * 736);
        static_assert(Nv12ChromaBytes(OutDesc{ 1280, 720, 1280, vic::PIXFMT_Y8_U8V8_N420, vic::BLK_KIND_GENERIC_16Bx2, 2 }) == 1280 * 384);
        static_assert(Nv12LumaBytes(OutDesc{ 640, 360, 640, vic::PIXFMT_Y8_U8V8_N420 }) == 640 * 360, "pitch output unchanged");

        /* libdrm vic40_fill / vic_clear: paint the whole target one colour with
         * no slot enabled. Anything non-zero in dst afterwards proves the whole
         * output half of the pipeline. */
        void FillClearConfig(vic::VicConfigStruct *c) {
            std::memset(c, 0, sizeof(*c));
            FillOutputConfig(c, SelfOut);
            /* Four DISTINCT levels so the output byte order can be read straight
             * off the dump: 1023->0xFF, 768->0xC0, 512->0x80, 256->0x40.
             * The previous red gave 'ff ff 00 00', which could not say which
             * byte was alpha and which was red. */
            c->outputConfig.BackgroundAlpha = 1023;   /* 0xFF */
            c->outputConfig.BackgroundR     = 768;    /* 0xC0 */
            c->outputConfig.BackgroundG     = 512;    /* 0x80 */
            c->outputConfig.BackgroundB     = 256;    /* 0x40 */
        }

        constinit std::atomic<bool> g_enlarge_2tap{true};     /* v0.7.19 (the A/B probe flips it) */
        constinit std::atomic<bool> g_enlarged_window{false};  /* this stream window enlarged */

        void FillBlitConfig(vic::VicConfigStruct *c, const SrcDesc &src, const OutDesc &out) {
            std::memset(c, 0, sizeof(*c));

            FillOutputConfig(c, out);
            c->outputConfig.BackgroundAlpha  = 1023;
            c->outputConfig.BackgroundR      = 1023;
            c->outputConfig.BackgroundG      = 1023;
            c->outputConfig.BackgroundB      = 1023;

            /* slot 0: 1:1 crop of the frame's top-left DstW x DstH */
            vic::SlotConfig *slot = std::addressof(c->slotStruct[0].slotConfig);
            slot->SlotEnable          = 1;
            slot->CurrentFieldEnable  = 1;
            slot->PlanarAlpha         = 1023;
            slot->ConstantAlpha       = 1;
            slot->SourceRectLeft      = static_cast<u64>(src.rect_x) << 16;
            slot->SourceRectRight     = static_cast<u64>(src.rect_x + src.rect_w - 1) << 16;   /* 16.16 fixed point */
            slot->SourceRectTop       = static_cast<u64>(src.rect_y) << 16;
            slot->SourceRectBottom    = static_cast<u64>(src.rect_y + src.rect_h - 1) << 16;
            slot->DestRectLeft        = 0;
            slot->DestRectRight       = out.w - 1;
            slot->DestRectTop         = 0;
            slot->DestRectBottom      = out.h - 1;
            slot->SoftClampHigh       = 1023;
            /* v0.7.19: enlarging (the HOME menu's 720p, or a handheld picture, in
             * a 1080p recording) came out blocky - the VIC's default, a 1-tap
             * filter. 2 taps, no coefficient table (5 taps without one came out
             * "very blurry", ovl4). Shrinking and 1:1 stay as they were. */
            if (out.w > src.rect_w || out.h > src.rect_h) {
                g_enlarged_window.store(true, std::memory_order_relaxed);
                const u64 taps = g_enlarge_2tap.load(std::memory_order_relaxed) ? 1 : 0;
                slot->FilterLengthX = out.w > src.rect_w ? taps : 0;
                slot->FilterLengthY = out.h > src.rect_h ? taps : 0;
                static bool said = false;
                if (!said) { said = true; LogLine("   vic: enlarging %ux%u to %ux%u - 2-tap filter", src.rect_w, src.rect_h, out.w, out.h); }
            }

            vic::SlotSurfaceConfig *s = std::addressof(c->slotStruct[0].slotSurfaceConfig);
            s->SlotPixelFormat   = src.pixfmt;
            s->SlotBlkKind       = src.blk_kind;
            s->SlotBlkHeight     = src.blk_h_log2;
            s->SlotCacheWidth    = src.cache_w;
            s->SlotSurfaceWidth  = src.w - 1;
            s->SlotSurfaceHeight = src.h - 1;
            s->SlotLumaWidth     = src.stride_px - 1;
            s->SlotLumaHeight    = src.h - 1;
            s->SlotChromaWidth   = 16383;
            s->SlotChromaHeight  = 16383;
        }

        /* v0.7.14 ovl2: a second input blended over slot 0 in the same job - an
         * overlay layer (the volume bar, Tesla). nullptr, the default: slot 0
         * alone and the job byte-for-byte as before. Only BlitStrip jobs (the
         * stream's) take it. Its address is guarded like the others in
         * RunOneJob: a zero address hangs the VIC and the console with it. */
        struct BlendSlot {
            u32 addr;                           /* device address of its surface */
            SrcDesc src;                        /* rect_w/rect_h: the part sampled */
            u32 dst_l, dst_t, dst_r, dst_b;     /* where it lands, inclusive */
            u32 k1;                             /* AlphaK1, 1023 = 1.0 */
            u8 src_c, dst_c, src_a, dst_a;      /* vic::BLEND_* selects */
            bool constant_alpha;                /* PlanarAlpha instead of the pixels' alpha */
            bool slot0_factors;                 /* also give slot 0 explicit replace factors */
        };
        /* VIC slots 1.. (bottom first); n = 0: slot 0 alone */
        constinit const BlendSlot *g_ovl_slots = nullptr;
        constinit u32 g_ovl_nslots = 0;
        constexpr u32 MaxOverlaySlots = 2;

        void FillSlot(vic::VicConfigStruct *c, u32 idx, const BlendSlot &b) {
            vic::SlotConfig *slot = std::addressof(c->slotStruct[idx].slotConfig);
            slot->SlotEnable          = 1;
            slot->CurrentFieldEnable  = 1;
            slot->PlanarAlpha         = 1023;
            slot->ConstantAlpha       = b.constant_alpha ? 1 : 0;
            slot->SourceRectLeft      = 0;
            slot->SourceRectRight     = static_cast<u64>(b.src.rect_w - 1) << 16;
            slot->SourceRectTop       = 0;
            slot->SourceRectBottom    = static_cast<u64>(b.src.rect_h - 1) << 16;
            slot->DestRectLeft        = b.dst_l;
            slot->DestRectRight       = b.dst_r;
            slot->DestRectTop         = b.dst_t;
            slot->DestRectBottom      = b.dst_b;
            slot->SoftClampHigh       = 1023;
            /* (ovl4 set FilterLengthX/Y = 2 for a scaled overlay: "very
             * blurry", and longer filters likely read a coefficient table
             * through SET_FILTER_STRUCT_OFFSET, which is never set - left at
             * the default, as libdrm and the game's slot do) */

            vic::SlotSurfaceConfig *s = std::addressof(c->slotStruct[idx].slotSurfaceConfig);
            s->SlotPixelFormat   = b.src.pixfmt;
            s->SlotBlkKind       = b.src.blk_kind;
            s->SlotBlkHeight     = b.src.blk_h_log2;
            s->SlotCacheWidth    = b.src.cache_w;
            s->SlotSurfaceWidth  = b.src.w - 1;
            s->SlotSurfaceHeight = b.src.h - 1;
            s->SlotLumaWidth     = b.src.stride_px - 1;
            s->SlotLumaHeight    = b.src.h - 1;
            s->SlotChromaWidth   = 16383;
            s->SlotChromaHeight  = 16383;

            vic::BlendingSlotStruct *bl = std::addressof(c->slotStruct[idx].blendingSlotStruct);
            bl->AlphaK1 = b.k1;
            bl->SrcFactCMatchSelect = b.src_c;
            bl->DstFactCMatchSelect = b.dst_c;
            bl->SrcFactAMatchSelect = b.src_a;
            bl->DstFactAMatchSelect = b.dst_a;
            if (b.slot0_factors) {
                vic::BlendingSlotStruct *b0 = std::addressof(c->slotStruct[0].blendingSlotStruct);
                b0->AlphaK1 = 1023;
                b0->SrcFactCMatchSelect = vic::BLEND_SRCFACTC_K1;
                b0->DstFactCMatchSelect = vic::BLEND_DSTFACTC_ZERO;
                b0->SrcFactAMatchSelect = vic::BLEND_SRCFACTA_K1;
                b0->DstFactAMatchSelect = vic::BLEND_DSTFACTA_ZERO;
            }
        }

        /* v0.7.14: the overlays showing now as slots, placed and scaled from a
         * 1280x720 screen to this output */
        u32 OverlaySlotsFor(const OutDesc &out, BlendSlot *ov) {
            OverlayPlane p[MaxOverlaySlots];
            const u32 n = OverlayPlanes(p, MaxOverlaySlots);
            for (u32 i = 0; i < n; ++i) {
                BlendSlot &b = ov[i];
                b = {};
                b.addr = p[i].dev;
                b.src = SrcDesc{ p[i].w, p[i].h, p[i].w, vic::BLK_KIND_GENERIC_16Bx2, 4, p[i].fmt, vic::CACHE_WIDTH_64Bx4, p[i].w, p[i].h };
                const u32 r = (p[i].x + p[i].dw) * out.w / 1280, bt = (p[i].y + p[i].dh) * out.h / 720;
                b.dst_l = p[i].x * out.w / 1280;
                b.dst_t = p[i].y * out.h / 720;
                b.dst_r = (r > out.w ? out.w : r) - 1;
                b.dst_b = (bt > out.h ? out.h : bt) - 1;
                b.k1 = 1023;
            }
            return n;
        }

        /* host1x pushbuf: VIC_PUSH_METHOD / VIC_PUSH_BUFFER from libdrm vic.h.
         * Each entry: [INCR(0x10,2)] [method>>2] [value|reloc-placeholder]. */
        constexpr u32 Host1xIncr0x10x2 = (UINT32_C(1) << 28) | (0x10u << 16) | 2u;

        /* Word indices of the three address slots, for the reloc variant. */
        constexpr u32 CfgAddrWord = 8, DstAddrWord = 11, SrcAddrWord = 14;   /* + 1 if SETCL is emitted */

        u32 BuildCmdbuf(u32 *w, VicJob job, u32 cfg_addr, u32 dst_addr, u32 src_addr, bool set_class,
                        u32 chroma_addr = 0, const BlendSlot *slots = nullptr, u32 nslots = 0) {
            const bool has_src  = (job == VicJob::BlitSelf || job == VicJob::BlitGame || job == VicJob::BlitStrip);
            u32 n = 0;
            /* Point the channel at the VIC's register space before touching
             * METHOD_OFFSET/METHOD_DATA, which are per-class registers. */
            if (set_class) { w[n++] = vic::Host1xOpcodeSetClass(0, vic::HOST1X_CLASS_VIC, 0); }
            w[n++] = Host1xIncr0x10x2; w[n++] = vic::SET_APPLICATION_ID >> 2; w[n++] = 1;
            w[n++] = Host1xIncr0x10x2; w[n++] = vic::SET_CONTROL_PARAMS >> 2;
            w[n++] = (static_cast<u32>(sizeof(vic::VicConfigStruct) / 16)) << 16;
            /* MAP_CMD_BUFFER already handed back a device address for buffers we
             * own, so those go in directly and the submit needs no reloc list.
             * The BlitReloc variant instead leaves all three as placeholders and
             * lets nvservices patch them. */
            w[n++] = Host1xIncr0x10x2; w[n++] = vic::SET_CONFIG_STRUCT_OFFSET >> 2;
            w[n++] = cfg_addr >> 8;
            w[n++] = Host1xIncr0x10x2; w[n++] = vic::SET_OUTPUT_SURFACE_LUMA_OFFSET >> 2;
            w[n++] = dst_addr >> 8;
            /* NV12 writes two planes, so the engine needs the UV base too. */
            if (chroma_addr != 0) {
                w[n++] = Host1xIncr0x10x2; w[n++] = vic::SET_OUTPUT_SURFACE_CHROMA_U_OFFSET >> 2;
                w[n++] = chroma_addr >> 8;
            }
            if (has_src) {
                w[n++] = Host1xIncr0x10x2; w[n++] = vic::SET_SURFACE0_SLOT0_LUMA_OFFSET >> 2;
                w[n++] = src_addr >> 8;
            }
            /* v0.7.14: the overlays' surfaces, slots 1.. */
            for (u32 i = 0; has_src && i < nslots; ++i) {
                w[n++] = Host1xIncr0x10x2; w[n++] = vic::SlotLumaOffsetMethod(1 + i) >> 2;
                w[n++] = slots[i].addr >> 8;
            }
            w[n++] = Host1xIncr0x10x2; w[n++] = vic::EXECUTE >> 2; w[n++] = 1u << 8;
            return n;
        }

        void TryIndirectCapture();
        void TryDebugCapture(u32 vfd, u32 nvmap_fd, u32 cmd_handle, u32 syncpt, u32 cfg_addr, u32 dst_addr);

        /* NVENC, phase A: exactly what M11 did for the VIC before trusting it
         * with real work - open the channel, take its syncpoint, bind the nvmap
         * fd, and submit a command buffer that does nothing but increment that
         * syncpoint. No SETCL, no method writes, so the engine cannot be pointed
         * at a bad address and cannot hang. If the fence advances, the msenc
         * channel and submit ABI are usable and the encode config is the only
         * thing left unknown. */

        void TryNvjpgProbe(u32 nvmap_fd, u32 cmd_handle, u32 dst_handle);
        void TryNvjpgDecodeControl(u32 nvmap_fd, u32 cmd_handle, u32 dst_handle);

        void TryGrcScan();
        void TryCscSweep(u32 vfd, u32 cmd_handle, u32 syncpt, u32 cfg_addr, u32 dst_addr, u32 self_addr);

        /* M86: why a stream ended - live mode decides what to wait for next */
        enum class StreamEnd { Done, GameGone, ViewerGone, EngineStall, Failed, Reconfigure, Disabled, Regrow };
        /* M99: attach, read one finished frame, write it as a PNG, detach */
        bool ShotStandalone();
        StreamEnd TryAmbilightStream(::ams::svc::Handle dbg, u64 slot_base, u32 vfd, u32 nvmap_fd,
                                     u32 cmd_handle, u32 vsyncpt, u32 cfg_addr, const GameSurface *geo);
        StreamEnd TryUvcRawStream(::ams::svc::Handle dbg, u64 slot_base, u32 vfd, u32 nvmap_fd,
                                  u32 cmd_handle, u32 vsyncpt, u32 cfg_addr, const GameSurface *geo);

        bool WriteSdVerified(const char *path, const u8 *src, size_t len, u8 *verify, u32 *out_fnv);
        bool WriteEngineOutputToSd(const char *path, const u8 *uncached_src, size_t len, u32 *out_fnv);

        /* ---- M82: the VIC colour matrix, measured rather than guessed -----
         * Three attempts to program RGB->YUV (M64 output matrix, M66 slot
         * matrix) produced a constant: the offset column landed and every
         * coefficient term came out as zero. The field layout matches the one
         * Ryujinx and our header agree on, so the unknown is the arithmetic -
         * what one unit of a coefficient is worth. A probe states a whole 3x4
         * matrix; RunOneJob applies it after the normal config fill. */
        struct CscProbe {
            char name[16];
            u32  where;         /* 0 none, 1 output matrix, 2 slot-0 matrix */
            u32  shift;         /* matrix_r_shift */
            s32  c[3][4];       /* row i: c[i][0..2] on inputs 0..2, c[i][3] the offset column */
        };
        constinit const CscProbe *g_csc_override = nullptr;

        /* M76: set when an engine job never completed and its channel was left
         * open. Every later engine probe in this boot refuses to run. */



        struct JobCtx {
            u32 vfd, cmd_handle, syncpt;
            u32 cfg_addr, dst_addr, src_addr, src_off;
            u32 cfg_handle, dst_handle, src_handle;
            SrcDesc game_src;
            /* M83: false when dst_addr is NOT g_vic_dst_buf (nvframe/nvstream
             * write into their own arena). M82 Run H's "[nf:vic0] changed=0/65536
             * (untouched)" was this check reading g_vic_dst_buf while the VIC
             * wrote the arena - the output was fine. */
            bool out_is_dst_buf = true;
        };


        /* ---- M63: the VIC was never slow ---------------------------------
         * M59 measured 119,023 us per full-frame blit and concluded the VIC is
         * unusable as a per-frame stage. That conclusion was wrong, and it was
         * published to STATUS.md, the README and GBAtemp as a hardware finding.
         *
         * What was actually being timed, PER JOB:
         *   - ~7 LogLine calls, each fs::OpenFile + WriteFile(WriteOption::Flush)
         *     + CloseFile against the SD card. An SD flush is 5-20 ms.
         *   - NvOpen + NvClose of /dev/nvhost-ctrl.
         *   - a 65,536-iteration byte loop computing a checksum of the output,
         *     plus a poison memset of the same buffer.
         *
         * That is 35-140 ms of instrumentation around a 1-2 ms engine op. The
         * compositor starvation followed from the same cause: blocking SD I/O
         * in a tight loop, not from contention for the engine.
         *
         * g_vic_quiet turns all of it off for streaming. The diagnostic path is
         * unchanged when quiet is false, so the one-shot probes still produce
         * exactly the evidence they used to. */
        constinit u32  g_ctrl_fd       = 0;
        constinit u64  g_vic_ns_total  = 0;
        constinit u64  g_vic_ns_worst  = 0;
        constinit u32  g_vic_jobs      = 0;
        constinit u32  g_vic_timeouts  = 0;

        /* One open for the life of the process, instead of one per job. */
        u32 CtrlFd() {
            if (g_ctrl_fd == 0) {
                u32 fd = 0, e = 0;
                if (R_SUCCEEDED(NvOpen("/dev/nvhost-ctrl", std::addressof(fd), std::addressof(e))) && e == 0) {
                    g_ctrl_fd = fd;
                }
            }
            return g_ctrl_fd;
        }

        /* Program the syncpoint increment the submit already promised. Omitting
         * this is what froze the console: nvhost raised syncpoint 12's max to
         * the fence, nothing ever incremented it, and nvnflinger - which
         * composites every frame on that same VIC syncpoint - waited forever. */
        u32 AppendIncrSyncpt(u32 *w, u32 n, u32 syncpt, bool after_engine_op) {
            const u32 cond = after_engine_op ? vic::INCR_SYNCPT_COND_OP_DONE
                                             : vic::INCR_SYNCPT_COND_IMMEDIATE;
            w[n++] = vic::Host1xOpcodeNonIncr(vic::UCLASS_INCR_SYNCPT, 1);
            w[n++] = (cond << vic::INCR_SYNCPT_COND_SHIFT) | syncpt;
            return n;
        }

        /* One VIC job end to end: config, cmdbuf, submit, wait, rescue, read
         * back. Called once per VicJob so a single reboot answers which half of
         * the pipeline works. */
        /* Returns false if the engine did not complete, so the caller can stop.
         *
         * The guard below is the lesson from two console freezes: a VIC handed a
         * zero address does not fail politely, it HANGS, and a hung VIC takes the
         * compositor and the whole console with it. Every address the engine will
         * dereference is checked here, once, rather than trusted per call site. */
        bool RunOneJob(const char *stage, VicJob job, bool set_class, const JobCtx &c) {
            VicStage(stage);

            const bool needs_src = (job == VicJob::BlitSelf || job == VicJob::BlitGame || job == VicJob::BlitStrip);
            /* v0.7.14: and the overlay slots' */
            const u32 nslots = (job == VicJob::BlitStrip && g_ovl_slots != nullptr) ? (g_ovl_nslots < MaxOverlaySlots ? g_ovl_nslots : MaxOverlaySlots) : 0;
            u32 zero_slot = 0;
            for (u32 i = 0; i < nslots; ++i) { if (g_ovl_slots[i].addr == 0) { zero_slot = 1 + i; } }
            if (c.cfg_addr == 0 || c.dst_addr == 0 || (needs_src && c.src_addr == 0) || zero_slot != 0) {
                LogLine("   [%s] REFUSING to submit: cfg=0x%x dst=0x%x src=0x%x zero slot %u - a zero "
                        "address hangs the VIC and freezes the console",
                        stage, c.cfg_addr, c.dst_addr, c.src_addr, zero_slot);
                return false;
            }

            auto *cfg = reinterpret_cast<vic::VicConfigStruct *>(g_vic_cfg_buf);
            switch (job) {
                case VicJob::Fill:     FillClearConfig(cfg);            break;
                case VicJob::BlitSelf:  FillBlitConfig(cfg, SelfSrc,   SelfOut);  break;
                case VicJob::BlitGame:  FillBlitConfig(cfg, c.game_src, SelfOut); break;
                case VicJob::BlitStrip:
                    FillBlitConfig(cfg, g_strip_src, g_strip_out);
                    cfg->outputConfig.OutputFlipX = (g_strip_flip & 1) ? 1 : 0;
                    cfg->outputConfig.OutputFlipY = (g_strip_flip & 2) ? 1 : 0;
                    break;
            }
            if (g_csc_override != nullptr && g_csc_override->where != 0) {
                const CscProbe &p = *g_csc_override;
                auto s20 = [](s32 v) -> u64 { return static_cast<u64>(static_cast<u32>(v) & 0xFFFFFu); };
                cfg->outColorMatrixStruct.matrix_enable = 0;
                auto &m = (p.where == 2) ? cfg->slotStruct[0].colorMatrixStruct : cfg->outColorMatrixStruct;
                m.matrix_coeff00 = s20(p.c[0][0]); m.matrix_coeff01 = s20(p.c[0][1]); m.matrix_coeff02 = s20(p.c[0][2]); m.matrix_coeff03 = s20(p.c[0][3]);
                m.matrix_coeff10 = s20(p.c[1][0]); m.matrix_coeff11 = s20(p.c[1][1]); m.matrix_coeff12 = s20(p.c[1][2]); m.matrix_coeff13 = s20(p.c[1][3]);
                m.matrix_coeff20 = s20(p.c[2][0]); m.matrix_coeff21 = s20(p.c[2][1]); m.matrix_coeff22 = s20(p.c[2][2]); m.matrix_coeff23 = s20(p.c[2][3]);
                m.matrix_r_shift = p.shift;
                m.matrix_enable  = 1;
            }
            for (u32 i = 0; i < nslots; ++i) { FillSlot(cfg, 1 + i, g_ovl_slots[i]); }

            /* pick the OutDesc this job will actually use, so the chroma base
             * matches the config struct we just filled */
            const OutDesc &job_out = (job == VicJob::BlitStrip) ? g_strip_out : SelfOut;
            const u32 chroma_addr = vic::IsNv12(job_out.fmt) ? (c.dst_addr + Nv12LumaBytes(job_out)) : 0u;

            auto *w = reinterpret_cast<u32 *>(g_vic_cmd_buf);
            u32 words = BuildCmdbuf(w, job, c.cfg_addr, c.dst_addr, c.src_addr + c.src_off, set_class, chroma_addr,
                                    g_ovl_slots, nslots);
            const u32 rw = set_class ? 1u : 0u;   /* reloc word indices shift when SETCL leads */
            words = AppendIncrSyncpt(w, words, c.syncpt, true);

            /* Prefill with a poison pattern rather than zero. "All zero" cannot
             * distinguish "engine wrote zeros" from "engine never touched our
             * memory"; surviving 0xAB proves the latter outright. */
            if (!g_vic_quiet && c.out_is_dst_buf) { std::memset(g_vic_dst_buf, 0xAB, DstSize); }
            armDCacheFlush(g_vic_dst_buf, DstSize);
            armDCacheFlush(g_vic_cfg_buf, VicCfgSize);
            armDCacheFlush(g_vic_cmd_buf, VicCmdSize);
            const u64 job_t0 = armTicksToNs(armGetSystemTick());

            const u32 nr = 0u;   /* relocs are inert on Horizon - always inline the addresses */

            alignas(8) u8 sb[16 + 12 + 3 * (16 + 4) + 20 + 4] = {};
            u32 off = 0;
            auto put = [&](u32 v) { std::memcpy(sb + off, std::addressof(v), 4); off += 4; };

            put(1); put(nr); put(1); put(1);
            put(c.cmd_handle); put(0); put(words);
            put(c.syncpt); put(1); put(0); put(0); put(0);
            const u32 fence_off = off; put(0);
            const u32 sz = off;

            const u32 req = (UINT32_C(3) << 30) | (sz << 16) | (0x00u << 8) | 0x01u;
            u32 nverr = 0, fence_val = 0;
            ::Result rc = NvIoctl(c.vfd, req, sb, sz, std::addressof(nverr));
            std::memcpy(std::addressof(fence_val), sb + fence_off, 4);
            if (!g_vic_quiet) {
                LogLine("   [%s] setcl=%d words=%u cmd[0..3]=%08x %08x %08x %08x",
                        stage, static_cast<int>(set_class), words, w[0], w[1], w[2], w[3]);
                LogLine("   [%s] req=0x%08x sz=%u nr=%u rc=0x%x nverr=%u -> fence=%u",
                        stage, req, sz, nr, rc, nverr, fence_val);
            }
            if (R_FAILED(rc) || nverr != 0) { LogLine("   [%s] SUBMIT REJECTED", stage); return false; }

            /* Read the cmdbuf back: nvservices patches reloc targets in place,
             * so these words now hold the addresses the engine would be given. */
            armDCacheFlush(g_vic_cmd_buf, VicCmdSize);
            if (!g_vic_quiet) LogLine("   [%s] resolved addrs: cfg=0x%08x dst=0x%08x src=0x%08x (<<8: 0x%x 0x%x 0x%x)",
                    stage, w[CfgAddrWord + rw], w[DstAddrWord + rw],
                    (job == VicJob::Fill) ? 0u : w[SrcAddrWord + rw],
                    w[CfgAddrWord + rw] << 8, w[DstAddrWord + rw] << 8,
                    (job == VicJob::Fill) ? 0u : (w[SrcAddrWord + rw] << 8));

            bool completed = false;
            const u32 cfd = CtrlFd();       /* opened once, never closed per job */
            if (cfd != 0) {
                struct { u32 id; u32 thresh; u32 timeout; } a = { c.syncpt, fence_val, 100 };
                nverr = 0;
                rc = NvIoctl(cfd, NvHostIocCtrlSyncptWait, std::addressof(a), sizeof(a), std::addressof(nverr));
                struct { u32 id; u32 value; } r = { c.syncpt, 0 };
                u32 e2 = 0;
                NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e2));
                completed = (r.value >= fence_val);

                /* M63b: SYNCPT_WAIT returns before the threshold is met. The
                 * loud path only ever "worked" because ~15 ms of SD-flushing log
                 * calls sat between the submit and this read, giving the engine
                 * time to finish - the bench made that obvious the moment the
                 * logging went away (warm-up OK, then 1/60 and 0/60).
                 *
                 * Poll instead. This does not depend on the wait ioctl's
                 * semantics, and the elapsed time it reports IS the engine time,
                 * which is the number M59 never actually measured. */
                if (!completed) {
                    /* v0.7.1: 300 polls (~30-60 ms) were not enough when the
                     * compositor shares the VIC - Minecraft in ReverseNX's docked
                     * mode took up to 450 ms - and a miss ended the stream.
                     * A stream waits up to ~0.5 s; the one-shot probe as before. */
                    const u32 spins = g_vic_quiet ? 5000 : 300;
                    for (u32 spin = 0; spin < spins && !completed; ++spin) {
                        os::SleepThread(TimeSpan::FromMicroSeconds(100));
                        r.value = 0;
                        NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e2));
                        completed = (r.value >= fence_val);
                    }
                    if (!completed) { ++g_vic_timeouts; }
                }
                if (!g_vic_quiet) {
                    LogLine("   [%s] WAIT nverr=%u  syncpt=%u (want >= %u)%s",
                            stage, nverr, r.value, fence_val,
                            completed ? "  OP_DONE fired" : "  ENGINE DID NOT COMPLETE");
                }

                /* Never leave a shared syncpoint short of its declared max.
                 *
                 * M63b: NOT in quiet mode. This ran 59 times in the bench, each
                 * pushing up to 64 manual increments into syncpoint 12 - the one
                 * nvnflinger composites on. Thousands of spurious increments
                 * into the compositor's syncpoint is almost certainly what froze
                 * the game, and it is a far worse failure than leaving a fence
                 * short. A one-shot probe can still rescue; a loop must not. */
                if (!g_vic_quiet && r.value < fence_val) {
                    VicStage("vb:RESCUE_INCR");
                    u32 ireq = NvHostIocCtrlSyncptIncrW;
                    for (u32 k = 0, missing = fence_val - r.value; k < missing && k < 64; k++) {
                        struct { u32 id; } ai = { c.syncpt };
                        u32 ae = 0;
                        NvIoctl(cfd, ireq, std::addressof(ai), sizeof(ai), std::addressof(ae));
                        if (k == 0 && ae != 0) { ireq = NvHostIocCtrlSyncptIncrWR;
                            NvIoctl(cfd, ireq, std::addressof(ai), sizeof(ai), std::addressof(ae)); }
                    }
                    if (!g_vic_quiet) LogLine("   [%s] rescued syncpt %u up to fence %u", stage, c.syncpt, fence_val);
                }
                /* cfd is process-wide now - closing it here is what cost a
                 * device open/close on every single job */
            }
            {
                const u64 dt = armTicksToNs(armGetSystemTick()) - job_t0;
                g_vic_ns_total += dt; ++g_vic_jobs;
                if (dt > g_vic_ns_worst) { g_vic_ns_worst = dt; }
            }

            /* The VIC wrote through the device side; our cache still holds the
             * zeros we just stored. Without this invalidate the read-back is
             * guaranteed to look empty no matter what the engine did. */
            armDCacheFlush(g_vic_dst_buf, DstSize);
            if (g_vic_quiet) { return completed; }
            if (!c.out_is_dst_buf) {
                LogLine("   [%s] output went to a caller-owned buffer at %#x - the caller checks and saves it", stage, c.dst_addr);
                return completed;
            }

            u32 sum = 0, changed = 0;
            for (u32 i = 0; i < DstSize; i++) {
                sum += g_vic_dst_buf[i];
                if (g_vic_dst_buf[i] != 0xAB) { changed++; }
            }
            char hex[3 * 32 + 1];
            int k = 0;
            for (u32 i = 0; i < 32; i++) { k += std::snprintf(hex + k, sizeof(hex) - k, "%02x ", g_vic_dst_buf[i]); }
            LogLine("   [%s] dst sum32=%u changed=%u/%u  %s", stage, sum, changed, DstSize,
                    (changed > 0) ? "*** ENGINE WROTE OUR MEMORY ***"
                                  : "(untouched - poison 0xAB intact)");
            LogLine("   [%s] dst[0..32]: %s", stage, hex);
            const u8 *mid = g_vic_dst_buf + (DstH / 2) * DstPitch;
            LogLine("   [%s] row %u: %02x %02x %02x %02x %02x %02x %02x %02x",
                    stage, DstH / 2, mid[0], mid[1], mid[2], mid[3], mid[4], mid[5], mid[6], mid[7]);
            return completed;
        }

        /* ---- M82: the colour-matrix probe sweep ---------------------------
         * Source: a 64x64 card in our own buffer, 16 flat 16x16 patches, bytes
         * R,G,B,A in memory - the game's order - declared A8R8G8B8 exactly as
         * the game path declares it, so what the fit finds applies to game
         * frames unchanged. Output: NV12, pitch. Flat patches make the chroma
         * subsampling filter irrelevant: the PC reads each patch's centre and
         * fits every plane as a*R + b*G + c*B + d over the 16 patches.
         *
         * The probes separate the unknowns one at a time:
         *   none        the known pass-through (M67: luma = B, U = R, V = G)
         *   out_k8..k19 one coefficient per row, 2^8 to 2^19-1, shift 0: which
         *               scale gives a gain of 1 is what a coefficient unit is
         *               worth, and row i -> plane / input i -> channel falls out
         *   out_k16_s8  whether matrix_r_shift divides the products too
         *   out_off     the offset column alone: its unit
         *   out_neg     a negative coefficient: is it two's complement?
         *   out_dense   all nine coefficients distinct: is every field where
         *               the struct says?
         *   slot_k16    the slot-side matrix under the same law?
         *   m64_bt601   M64's exact matrix: the harness must reproduce M64's
         *               constant output, or it is not measuring the same thing
         * Pure arithmetic in a VIC job on our own buffers: nothing here can
         * hand the engine a bad address. */
        constexpr u32 CscW = 64, CscH = 64, CscStridePx = 256;
        constexpr s32 K8 = 1 << 8, K11 = 1 << 11, K12 = 1 << 12, K16 = 1 << 16, K19 = (1 << 19) - 1;
        constexpr CscProbe CscProbes[] = {
            { "none",       0, 0, {} },
            { "out_k8",     1, 0, { { K8, 0, 0, 0 },  { 0, K8, 0, 0 },  { 0, 0, K8, 0 } } },
            { "out_k12",    1, 0, { { K12, 0, 0, 0 }, { 0, K12, 0, 0 }, { 0, 0, K12, 0 } } },
            { "out_k16",    1, 0, { { K16, 0, 0, 0 }, { 0, K16, 0, 0 }, { 0, 0, K16, 0 } } },
            { "out_k19",    1, 0, { { K19, 0, 0, 0 }, { 0, K19, 0, 0 }, { 0, 0, K19, 0 } } },
            { "out_k16_s8", 1, 8, { { K16, 0, 0, 0 }, { 0, K16, 0, 0 }, { 0, 0, K16, 0 } } },
            { "out_off",    1, 0, { { 0, 0, 0, 256 }, { 0, 0, 0, 512 }, { 0, 0, 0, 768 } } },
            { "out_neg",    1, 0, { { K16, 0, 0, 0 }, { 0, -K16, 0, 768 }, { 0, 0, K16, 0 } } },
            { "out_dense",  1, 0, { { 1 * K11, 2 * K11, 3 * K11, 0 }, { 4 * K11, 5 * K11, 6 * K11, 0 }, { 7 * K11, 8 * K11, 9 * K11, 0 } } },
            { "slot_k16",   2, 0, { { K16, 0, 0, 0 }, { 0, K16, 0, 0 }, { 0, 0, K16, 0 } } },
            { "m64_bt601",  1, 8, { { 66, 129, 25, 4096 }, { -38, -74, 112, 32768 }, { 112, -94, -18, 32768 } } },
            /* M83: the production matrix, measured on the card (tools/vic_csc.py
             * checks it against the float BT.709 conversion) */
            { "bt709",      1, vic_csc::Shift, {
                { vic_csc::Bt709[0][0], vic_csc::Bt709[0][1], vic_csc::Bt709[0][2], vic_csc::Bt709[0][3] },
                { vic_csc::Bt709[1][0], vic_csc::Bt709[1][1], vic_csc::Bt709[1][2], vic_csc::Bt709[1][3] },
                { vic_csc::Bt709[2][0], vic_csc::Bt709[2][1], vic_csc::Bt709[2][2], vic_csc::Bt709[2][3] } } },
        };
        constexpr u32 CscNumProbes = sizeof(CscProbes) / sizeof(CscProbes[0]);
        /* the 16 patch colours, R,G,B: the corners of the {40,200} cube, grey,
         * and seven mixed colours, so the per-plane fit is well conditioned */
        constexpr u8 CscPatch[16][3] = {
            {  40,  40,  40 }, { 200,  40,  40 }, {  40, 200,  40 }, {  40,  40, 200 },
            { 200, 200,  40 }, { 200,  40, 200 }, {  40, 200, 200 }, { 200, 200, 200 },
            { 120, 120, 120 }, { 200, 120,  40 }, {  40, 200, 120 }, { 120,  40, 200 },
            { 160,  80, 200 }, {  80, 160,  40 }, { 200, 200, 120 }, {  60, 100, 160 },
        };
        struct __attribute__((packed)) CscFileHdr { u32 magic, version, count, w, h, src_fmt, out_fmt, reserved; };
        struct __attribute__((packed)) CscFileRec { char name[16]; u32 where, shift; s32 c[3][4]; u32 completed; };
        static_assert(sizeof(CscFileHdr) == 32 && sizeof(CscFileRec) == 76, "must match tools/vic_csc.py");
        constexpr size_t CscRecBytes = sizeof(CscFileRec) + CscW * CscH * 3 / 2;
        constexpr size_t CscFileBytes = sizeof(CscFileHdr) + CscW * CscH * 4 + CscNumProbes * CscRecBytes;
        static_assert(CscStridePx * 4 * CscH <= VicSrcSize, "the card must fit our source buffer");
        static_assert(CscStridePx * CscH * 3 / 2 <= DstSize, "the output must fit what RunOneJob poisons and flushes");

        void TryCscSweep(u32 vfd, u32 cmd_handle, u32 syncpt, u32 cfg_addr, u32 dst_addr, u32 self_addr) {
            VicStage("csc:1");
            LogLine("   ---- VIC COLOUR-MATRIX PROBES (M82/M83): %u jobs on a 64x64 card we own ----", CscNumProbes);
            if (g_stage_buf == nullptr || 2 * ((CscFileBytes + 0xFFF) & ~static_cast<size_t>(0xFFF)) > StreamStageSize
                || g_ind_size < FbBlockRowStage + StreamStageSize) {
                LogLine("   csc: no stage buffer - not running");
                return;
            }
            /* the card: bytes R,G,B,A per pixel, like the game's surface */
            for (u32 y = 0; y < CscH; ++y) {
                u8 *row = g_vic_src_buf + y * CscStridePx * 4;
                for (u32 x = 0; x < CscW; ++x) {
                    const u8 *c = CscPatch[(y / 16) * 4 + (x / 16)];
                    row[x * 4 + 0] = c[0]; row[x * 4 + 1] = c[1]; row[x * 4 + 2] = c[2]; row[x * 4 + 3] = 0xFF;
                }
            }
            armDCacheFlush(g_vic_src_buf, VicSrcSize);

            u8 *const f = g_stage_buf;
            const CscFileHdr hdr = { 0x43534356u /* "VCSC" */, 1, CscNumProbes, CscW, CscH,
                                     vic::PIXFMT_A8R8G8B8, vic::PIXFMT_Y8_U8V8_N420, 0 };
            std::memcpy(f, std::addressof(hdr), sizeof(hdr));
            size_t off = sizeof(hdr);
            for (u32 y = 0; y < CscH; ++y) { std::memcpy(f + off, g_vic_src_buf + y * CscStridePx * 4, CscW * 4); off += CscW * 4; }

            const JobCtx ctx{ vfd, cmd_handle, syncpt, cfg_addr, dst_addr, self_addr, 0, 0, 0, 0, SelfSrc };
            u32 n_ok = 0;
            for (u32 i = 0; i < CscNumProbes; ++i) {
                const CscProbe &p = CscProbes[i];
                g_strip_src = SrcDesc{ CscW, CscH, CscStridePx, vic::BLK_KIND_PITCH, 0, vic::PIXFMT_A8R8G8B8,
                                       vic::CACHE_WIDTH_64Bx4, CscW, CscH };
                g_strip_out = OutDesc{ CscW, CscH, CscStridePx, vic::PIXFMT_Y8_U8V8_N420 };
                g_csc_override = std::addressof(p);
                char stage[32];
                std::snprintf(stage, sizeof(stage), "csc:%s", p.name);
                const bool ok = RunOneJob(stage, VicJob::BlitStrip, true, ctx);
                g_csc_override = nullptr;
                if (ok) { ++n_ok; }

                CscFileRec rec = {};
                std::memcpy(rec.name, p.name, sizeof(rec.name));
                rec.where = p.where; rec.shift = p.shift; rec.completed = ok ? 1 : 0;
                std::memcpy(rec.c, p.c, sizeof(rec.c));
                std::memcpy(f + off, std::addressof(rec), sizeof(rec)); off += sizeof(rec);
                /* g_vic_dst_buf is uncached: these reads see what the engine wrote */
                const u8 *yp = g_vic_dst_buf, *uvp = g_vic_dst_buf + CscStridePx * CscH;
                for (u32 y = 0; y < CscH; ++y)     { std::memcpy(f + off, yp + y * CscStridePx, CscW); off += CscW; }
                for (u32 y = 0; y < CscH / 2; ++y) { std::memcpy(f + off, uvp + y * CscStridePx, CscW); off += CscW; }
                /* the centre of patch 7 (white-ish 200,200,200) and patch 1 (red), for the log */
                const u32 cy = 16 * 1 + 8, cx7 = 16 * 3 + 8, cx1 = 16 * 1 + 8;
                LogLine("   csc[%-10s] %s  patch(200,200,200): Y=%u U=%u V=%u  patch(200,40,40): Y=%u U=%u V=%u",
                        p.name, ok ? "done" : "DID NOT COMPLETE",
                        yp[cy * CscStridePx + cx7], uvp[(cy / 2) * CscStridePx + (cx7 & ~1u)], uvp[(cy / 2) * CscStridePx + (cx7 & ~1u) + 1],
                        yp[8 * CscStridePx + cx1], uvp[4 * CscStridePx + (cx1 & ~1u)], uvp[4 * CscStridePx + (cx1 & ~1u) + 1]);
                if (!ok) { LogLine("   csc: stopping at the first job that did not complete"); break; }
            }
            u32 fnv = 0;
            const size_t slot = (off + 0xFFF) & ~static_cast<size_t>(0xFFF);
            WriteSdVerified("sdmc:/vic-csc.bin", f, off, f + slot, std::addressof(fnv));
            LogLine("   csc: %u of %u probes completed; %zu B -> sdmc:/vic-csc.bin (decode: tools/vic_csc.py)", n_ok, CscNumProbes, off);
            VicStage("csc:done");
        }

        /* ---- v0.7.14: the overlays' blend check ----------------------------
         * Before overlays are blended into the stream, one VIC job on cards we
         * own checks what they rely on: the format the stream reads game
         * frames in (A8R8G8B8 here: memory bytes R,G,B,A) takes alpha from
         * the last byte, as the overlays store it, and slot 1 lands over
         * slot 0 with straight alpha. The blend card (ovl2,
         * logs/v0714-ovl2-run1) found the VIC blends that way whatever the
         * factor fields say, and that format 33 reads alpha from the first
         * byte. Overlay bands, 8 rows each: transparent, opaque black, opaque
         * white, half-transparent white. */
        bool OverlayBlendCheck(u32 vfd, u32 cmd_handle, u32 syncpt, u32 cfg_addr, u32 dst_addr, u32 self_addr) {
            constexpr u32 W = 64, H = 32, Stride = 256;
            constexpr size_t Surf = Stride * 4 * H;
            static_assert(2 * Surf <= VicSrcSize && Surf <= DstSize);
            constexpr u8 Band[4][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 255 }, { 255, 255, 255, 255 }, { 255, 255, 255, 128 } };
            constexpr u8 Bg[3] = { 40, 120, 200 };
            u8 *const s0 = g_vic_src_buf, *const s1 = g_vic_src_buf + Surf;
            std::memset(g_vic_src_buf, 0, 2 * Surf);
            for (u32 y = 0; y < H; ++y) {
                for (u32 x = 0; x < W; ++x) {
                    u8 *p = s0 + y * Stride * 4 + x * 4;
                    p[0] = Bg[0]; p[1] = Bg[1]; p[2] = Bg[2]; p[3] = 255;
                    std::memcpy(s1 + y * Stride * 4 + x * 4, Band[y / 8], 4);
                }
            }
            armDCacheFlush(g_vic_src_buf, VicSrcSize);
            const SrcDesc card{ W, H, Stride, vic::BLK_KIND_PITCH, 0, vic::PIXFMT_A8R8G8B8, vic::CACHE_WIDTH_64Bx4, W, H };
            g_strip_src = card;
            g_strip_out = OutDesc{ W, H, Stride, vic::PIXFMT_A8R8G8B8 };
            g_strip_flip = 0;
            BlendSlot b = {};
            b.addr = self_addr + static_cast<u32>(Surf);
            b.src = card;
            b.dst_r = W - 1; b.dst_b = H - 1;
            b.k1 = 1023;
            g_ovl_slots = std::addressof(b);
            g_ovl_nslots = 1;
            std::memset(g_vic_dst_buf, 0xAB, DstSize);
            const JobCtx ctx{ vfd, cmd_handle, syncpt, cfg_addr, dst_addr, self_addr, 0, 0, 0, 0, SelfSrc };
            const bool done = RunOneJob("ovl:check", VicJob::BlitStrip, true, ctx);
            g_ovl_slots = nullptr;
            g_ovl_nslots = 0;
            const u8 *o[4];
            for (u32 band = 0; band < 4; ++band) { o[band] = g_vic_dst_buf + (band * 8 + 4) * Stride * 4 + 32 * 4; }
            auto near = [](u32 v, u32 want, u32 tol) { return (v > want ? v - want : want - v) <= tol; };
            bool ok = done;
            for (u32 c = 0; c < 3; ++c) {
                ok = ok && near(o[0][c], Bg[c], 3)                                  /* transparent: the background */
                        && near(o[1][c], 0, 3)                                      /* opaque black */
                        && near(o[2][c], 255, 3)                                    /* opaque white */
                        && near(o[3][c], (255 * 128 + Bg[c] * 127) / 255, 4);       /* half white, straight alpha */
            }
            LogLine("   overlays: blend check %s - transparent %02x%02x%02x (bg %02x%02x%02x), black %02x%02x%02x, white %02x%02x%02x, half %02x%02x%02x",
                    !done ? "DID NOT COMPLETE" : (ok ? "passed" : "FAILED"),
                    o[0][0], o[0][1], o[0][2], Bg[0], Bg[1], Bg[2], o[1][0], o[1][1], o[1][2], o[2][0], o[2][1], o[2][2], o[3][0], o[3][1], o[3][2]);
            return ok;
        }

#if SFT_OVL_PROBE
        /* ---- v0.7.14 ovl2: the blend card ---------------------------------
         * Does the VIC blend a second slot over the first, and with which
         * factors? Slot 0: a 64x32 background card; slot 1: an overlay card of
         * eight 4-row bands with known alphas (the decoded overlays are
         * premultiplied, logs/v0714-ovl1-run1/decoded). One job per variant,
         * output RGBA (pitch), every variant's output saved for the PC
         * (tools/vic_blend.py). All buffers are ours, every address non-zero.
         * Once a boot with config/switch-frame-tap/probe-blend.txt; a marker
         * file stops it running again after a boot it did not finish. */
        constexpr u32 BcW = 64, BcH = 32, BcStridePx = 256;
        constexpr size_t BcSurfBytes = BcStridePx * 4 * BcH;          /* 32 KB */
        static_assert(2 * BcSurfBytes <= VicSrcSize, "both cards fit our source buffer");
        static_assert(BcSurfBytes <= DstSize, "the output fits what RunOneJob flushes");
        constexpr u8 BcBand[8][4] = {
            {   0,   0,   0,   0 },   /* transparent: the background */
            { 255, 255, 255, 255 },   /* opaque white */
            { 128, 128, 128, 128 },   /* 50 % white */
            {  64,   0,   0,  64 },   /* 25 % red */
            {   0,   0,   0, 128 },   /* 50 % black: the background halved */
            { 255,   0,   0,   0 },   /* colour with alpha 0: added (premultiplied) or dropped (straight) */
            {   0,   0,   0, 255 },   /* opaque black: is alpha byte 3? */
            { 192,  96,  48, 192 },   /* 75 % orange */
        };
        struct BcVariant { char name[16]; u32 k1; u8 src_c, dst_c, src_a, dst_a; bool enable, constant_alpha, slot0_factors, scaled, block_linear; };
        constexpr BcVariant BcVariants[] = {
            { "off",        0,    0, 0, 0, 0, false, false, false, false, false },
            { "zero",       0,    0, 0, 0, 0, true,  false, false, false, false },
            { "premul",     1023, vic::BLEND_SRCFACTC_K1, vic::BLEND_DSTFACTC_NEG_K1_TIMES_SRC,
                                  vic::BLEND_SRCFACTA_K1, vic::BLEND_DSTFACTA_NEG_K1_TIMES_SRC, true, false, false, false, false },
            { "straight",   1023, vic::BLEND_SRCFACTC_K1_TIMES_SRC, vic::BLEND_DSTFACTC_NEG_K1_TIMES_SRC,
                                  vic::BLEND_SRCFACTA_K1, vic::BLEND_DSTFACTA_NEG_K1_TIMES_SRC, true, false, false, false, false },
            { "premul_s0",  1023, vic::BLEND_SRCFACTC_K1, vic::BLEND_DSTFACTC_NEG_K1_TIMES_SRC,
                                  vic::BLEND_SRCFACTA_K1, vic::BLEND_DSTFACTA_NEG_K1_TIMES_SRC, true, false, true,  false, false },
            { "constant",   1023, vic::BLEND_SRCFACTC_K1, vic::BLEND_DSTFACTC_NEG_K1_TIMES_SRC,
                                  vic::BLEND_SRCFACTA_K1, vic::BLEND_DSTFACTA_NEG_K1_TIMES_SRC, true, true,  false, false, false },
            { "premul_x1.5", 1023, vic::BLEND_SRCFACTC_K1, vic::BLEND_DSTFACTC_NEG_K1_TIMES_SRC,
                                  vic::BLEND_SRCFACTA_K1, vic::BLEND_DSTFACTA_NEG_K1_TIMES_SRC, true, false, false, true,  false },
            { "premul_bl",  1023, vic::BLEND_SRCFACTC_K1, vic::BLEND_DSTFACTC_NEG_K1_TIMES_SRC,
                                  vic::BLEND_SRCFACTA_K1, vic::BLEND_DSTFACTA_NEG_K1_TIMES_SRC, true, false, false, false, true  },
        };
        constexpr u32 BcNumVariants = sizeof(BcVariants) / sizeof(BcVariants[0]);
        struct __attribute__((packed)) BcFileHdr { u32 magic, version, count, w, h, reserved[3]; };
        struct __attribute__((packed)) BcFileRec { char name[16]; u32 completed, enable, constant_alpha, slot0_factors, scaled, block_linear; u8 k1_lo, k1_hi, src_c, dst_c, src_a, dst_a, pad[2]; };
        static_assert(sizeof(BcFileHdr) == 32 && sizeof(BcFileRec) == 48, "must match tools/vic_blend.py");
        constexpr size_t BcFileBytes = sizeof(BcFileHdr) + 2 * BcW * BcH * 4 + BcNumVariants * (sizeof(BcFileRec) + BcW * BcH * 4);
        constexpr const char *BcTrigger = "sdmc:/config/switch-frame-tap/probe-blend.txt";
        constexpr const char *BcMarker  = "sdmc:/config/switch-frame-tap/probe/blend-running.txt";

        bool FileThere(const char *path) {
            fs::DirectoryEntryType t;
            return R_SUCCEEDED(fs::GetEntryType(std::addressof(t), path));
        }

        /* the block-linear offset of byte x of row y (64-byte x 8-row GOBs,
         * 2^blk_h GOBs to a block; rows < 8 << blk_h only, one block row) */
        u32 BcBlOffset(u32 xbyte, u32 y, u32 blk_h_log2) {
            const u32 block_sz = 512u << blk_h_log2;
            const u32 gob_y = y / 8, yy = y % 8, xb = xbyte % 64;
            return (xbyte / 64) * block_sz + gob_y * 512 + (xb / 32) * 256 + (yy / 2) * 64 + ((xb % 32) / 16) * 32 + (yy % 2) * 16 + (xb % 16);
        }

        void TryBlendCard(u32 vfd, u32 cmd_handle, u32 syncpt, u32 cfg_addr, u32 dst_addr, u32 self_addr) {
            if (!FileThere(BcTrigger)) { return; }
            static_cast<void>(fs::CreateDirectory("sdmc:/config/switch-frame-tap/probe"));
            if (FileThere(BcMarker)) {
                LogLine("   blend card: it did not finish on an earlier boot (%s) - not running it again", BcMarker);
                return;
            }
            /* the dmnt:cht bounce: free until the stream reads through it */
            u8 *const f = g_vic_heap != 0 ? reinterpret_cast<u8 *>(g_vic_heap + VicBounceOff) : nullptr;
            if (f == nullptr || 2 * ((BcFileBytes + 0xFFF) & ~static_cast<size_t>(0xFFF)) > VicBounceSize) {
                LogLine("   blend card: no room to assemble its file - not running");
                return;
            }
            static_cast<void>(fs::CreateFile(BcMarker, 0));
            VicStage("blend:1");
            LogLine("   ---- VIC BLEND CARD (ovl2): %u jobs, slot 1 over slot 0 on %ux%u cards we own ----", BcNumVariants, BcW, BcH);

            u8 *const s0 = g_vic_src_buf, *const s1 = g_vic_src_buf + BcSurfBytes;
            const u32 s1_addr = self_addr + static_cast<u32>(BcSurfBytes);
            /* background: R rises left to right, B falls, G rises downwards */
            std::memset(g_vic_src_buf, 0, 2 * BcSurfBytes);
            for (u32 y = 0; y < BcH; ++y) {
                for (u32 x = 0; x < BcW; ++x) {
                    u8 *p = s0 + y * BcStridePx * 4 + x * 4;
                    p[0] = static_cast<u8>(x * 4); p[1] = static_cast<u8>(64 + y * 4); p[2] = static_cast<u8>(255 - x * 4); p[3] = 255;
                }
            }
            auto paint_pitch = [&]() {
                std::memset(s1, 0, BcSurfBytes);
                for (u32 y = 0; y < BcH; ++y) {
                    for (u32 x = 0; x < BcW; ++x) { std::memcpy(s1 + y * BcStridePx * 4 + x * 4, BcBand[y / 4], 4); }
                }
            };
            paint_pitch();

            const BcFileHdr hdr = { 0x434C4256u /* "VBLC" */, 1, BcNumVariants, BcW, BcH, {} };
            std::memcpy(f, std::addressof(hdr), sizeof(hdr));
            size_t off = sizeof(hdr);
            for (u32 y = 0; y < BcH; ++y) { std::memcpy(f + off, s0 + y * BcStridePx * 4, BcW * 4); off += BcW * 4; }
            for (u32 y = 0; y < BcH; ++y) { std::memcpy(f + off, s1 + y * BcStridePx * 4, BcW * 4); off += BcW * 4; }

            const JobCtx ctx{ vfd, cmd_handle, syncpt, cfg_addr, dst_addr, self_addr, 0, 0, 0, 0, SelfSrc };
            const SrcDesc pitch_src{ BcW, BcH, BcStridePx, vic::BLK_KIND_PITCH, 0, vic::PIXFMT_A8B8G8R8, vic::CACHE_WIDTH_64Bx4, BcW, BcH };
            u32 n_ok = 0;
            for (u32 i = 0; i < BcNumVariants; ++i) {
                const BcVariant &v = BcVariants[i];
                if (v.block_linear) {
                    /* the overlay card again, block-linear with 16-GOB blocks, as
                     * the overlays' own buffers are */
                    std::memset(s1, 0, BcSurfBytes);
                    for (u32 y = 0; y < BcH; ++y) {
                        for (u32 x = 0; x < BcW; ++x) {
                            for (u32 b = 0; b < 4; ++b) { s1[BcBlOffset(x * 4 + b, y, 4)] = BcBand[y / 4][b]; }
                        }
                    }
                }
                armDCacheFlush(g_vic_src_buf, VicSrcSize);
                g_strip_src = pitch_src;
                g_strip_out = OutDesc{ BcW, BcH, BcStridePx, vic::PIXFMT_A8B8G8R8 };
                g_strip_flip = 0;
                BlendSlot b = {};
                b.addr = s1_addr;
                b.src = v.block_linear ? SrcDesc{ BcW, BcH, BcW, vic::BLK_KIND_GENERIC_16Bx2, 4, vic::PIXFMT_A8B8G8R8, vic::CACHE_WIDTH_64Bx4, BcW, BcH }
                                       : pitch_src;
                if (v.scaled) { b.src.rect_w = 32; b.src.rect_h = 16; b.dst_l = 0; b.dst_t = 0; b.dst_r = 47; b.dst_b = 23; }
                else          { b.dst_l = 0; b.dst_t = 0; b.dst_r = BcW - 1; b.dst_b = BcH - 1; }
                b.k1 = v.k1; b.src_c = v.src_c; b.dst_c = v.dst_c; b.src_a = v.src_a; b.dst_a = v.dst_a;
                b.constant_alpha = v.constant_alpha; b.slot0_factors = v.slot0_factors;
                g_ovl_slots = std::addressof(b);
                g_ovl_nslots = v.enable ? 1 : 0;
                std::memset(g_vic_dst_buf, 0xAB, DstSize);   /* poison: an untouched output stays 0xAB */
                char stage[32];
                std::snprintf(stage, sizeof(stage), "blend:%s", v.name);
                const bool ok = RunOneJob(stage, VicJob::BlitStrip, true, ctx);
                g_ovl_slots = nullptr;
                g_ovl_nslots = 0;
                if (ok) { ++n_ok; }

                BcFileRec rec = {};
                std::memcpy(rec.name, v.name, sizeof(rec.name));
                rec.completed = ok ? 1 : 0; rec.enable = v.enable; rec.constant_alpha = v.constant_alpha;
                rec.slot0_factors = v.slot0_factors; rec.scaled = v.scaled; rec.block_linear = v.block_linear;
                rec.k1_lo = static_cast<u8>(v.k1 & 0xFF); rec.k1_hi = static_cast<u8>(v.k1 >> 8);
                rec.src_c = v.src_c; rec.dst_c = v.dst_c; rec.src_a = v.src_a; rec.dst_a = v.dst_a;
                std::memcpy(f + off, std::addressof(rec), sizeof(rec)); off += sizeof(rec);
                /* g_vic_dst_buf is uncached: these reads see what the engine wrote */
                for (u32 y = 0; y < BcH; ++y) { std::memcpy(f + off, g_vic_dst_buf + y * BcStridePx * 4, BcW * 4); off += BcW * 4; }
                /* column 32 of each band's second row, for the log */
                char line[8 * 16 + 1];
                int k = 0;
                for (u32 band = 0; band < 8; ++band) {
                    const u8 *p = g_vic_dst_buf + (band * 4 + 1) * BcStridePx * 4 + 32 * 4;
                    k += std::snprintf(line + k, sizeof(line) - k, " %02x%02x%02x%02x", p[0], p[1], p[2], p[3]);
                }
                LogLine("   blend[%-11s] %s:%s", v.name, ok ? "done" : "DID NOT COMPLETE", line);
                if (!ok) { LogLine("   blend card: stopping at the first job that did not complete"); break; }
            }
            u32 fnv = 0;
            const size_t slot = (off + 0xFFF) & ~static_cast<size_t>(0xFFF);
            WriteSdVerified("sdmc:/config/switch-frame-tap/probe/vic-blend.bin", f, off, f + slot, std::addressof(fnv));
            LogLine("   blend card: %u of %u jobs completed; %zu B -> probe/vic-blend.bin (tools/vic_blend.py)", n_ok, BcNumVariants, off);
            static_cast<void>(fs::DeleteFile(BcMarker));
            VicStage("blend:done");
        }
#endif


    }

    /* the dmnt:cht read bounce (applet_mitm_dmdbg.cpp): nullptr until the VIC
     * heap exists - reads only happen while streaming, after it is taken */
    u8 *VicBounceBuffer() {
        return g_vic_heap != 0 ? reinterpret_cast<u8 *>(g_vic_heap + VicBounceOff) : nullptr;
    }
    /* v0.7.14: the lower 768 KB - the overlays work in the rest (cached
     * memory; the buffers the VIC reads are uncached, slow for the CPU) */
    size_t VicBounceBufferSize() { return VicBounceOverlayOff; }

    constinit bool g_dump_armed    = false;
    constinit u32  g_probe_delay_s = 120;

    constinit std::atomic<u32> g_queue_count{0};
    constinit std::atomic<s32> g_queue_slot{-1};
    constinit std::atomic<u64> g_queue_tick{0};
    constinit std::atomic<u32> g_queue_fence_n{0};
    constinit bool g_nvab_armed = false;
    constinit std::atomic<u32> g_early_collect{1};
    /* v0.7.14 ovl2: frames the live stream sent and their read and VIC time,
     * for research that measures its own cost per second (the overlay probe) */
    constinit std::atomic<u32> g_live_frames{0};
    constinit std::atomic<u64> g_live_read_ns{0}, g_live_vic_ns{0};
    namespace {
        inline void LiveFrameSent(u64 read_ns, u64 vic_ns) {
            g_live_frames.fetch_add(1, std::memory_order_relaxed);
            g_live_read_ns.fetch_add(read_ns, std::memory_order_relaxed);
            g_live_vic_ns.fetch_add(vic_ns, std::memory_order_relaxed);
        }
    }
    constinit std::atomic<u32> g_queue_transform{0};
    constinit std::atomic<u32> g_queue_crop_wh{0};
    constinit std::atomic<bool> g_queue_crop_origin0{true};
    constinit std::atomic<u32> g_queue_crop_c{0};
    constinit std::atomic<bool> g_viewer_key_request{false};
    constinit std::atomic<bool> g_viewer_can_ask{false};
    constinit std::atomic<bool> g_viewer_crop{false};
    /* v0.7.18: close the engines and set up again on the full donor (DonorCanGrow) */
    constinit std::atomic<bool> g_donor_regrow{false};
    constinit std::atomic<u32> g_size_lock{0};
    void ViewerLockSize(bool lock) {
        if (!lock) { g_size_lock.store(0, std::memory_order_relaxed); return; }
        if (g_size_lock.load(std::memory_order_relaxed) != 0) { return; }   /* already: a repeated request */
        const u32 w = g_live_w.load(std::memory_order_relaxed), h = g_live_h.load(std::memory_order_relaxed);
        g_size_lock.store(g_streaming_now.load(std::memory_order_relaxed) && w != 0 ? (w << 16 | h) : ~0u, std::memory_order_relaxed);
    }
    constinit std::atomic<u64> g_queue_fence[4] = {};
    constinit std::atomic<u32> g_queue_seq{0};
    constinit PresentRec g_present_ring[PresentRingSize] = {};
    constinit std::atomic<bool> g_stream_enabled{true};
    constinit std::atomic<bool> g_reconfig_request{false};
    constinit std::atomic<u64>  g_app_tid{0};
    constinit std::atomic<u64>  g_vim_program{0};
    constinit std::atomic<bool> g_app_excluded{false};
    constinit std::atomic<u32>  g_live_state{LiveState_Starting};
    constinit std::atomic<bool> g_streaming_now{false};
    constinit std::atomic<u32>  g_live_w{0}, g_live_h{0}, g_live_fps_x10{0}, g_live_game_fps_x10{0}, g_live_sessions{0};

    namespace {
    void CaptureInto(GameSurface &surf, const char *who, const NvGraphicBufferRaw *gb, s32 slot_arg) {
        if (gb == nullptr || gb->num_planes == 0) { return; }
        const NvSurfaceRaw *p0 = std::addressof(gb->planes[0]);

        /* M87: a new buffer object is a new game (or a new swapchain): forget
         * the old slots. Before M87 the slot was a running count of these
         * calls, so a relaunched game's buffers landed at slots 6 and 7. */
        /* v0.7.1: slot 0 starts a set. A different nvmap id alone no longer
         * does - Minecraft registers each slot from its own buffer object, and
         * the old rule restarted the set at every slot. */
        if (slot_arg == 0 || (slot_arg < 0 && static_cast<u32>(gb->nvmap_id) != surf.nvmap_id)) {
            for (auto &o : surf.slot_offset) { o = 0; }
            for (auto &o : surf.slot_nvmap) { o = 0; }
            surf.num_slots = 0;
            ++surf.generation;
        }
        const u32 slot = (slot_arg >= 0 && slot_arg < 8) ? static_cast<u32>(slot_arg) : surf.num_slots;
        if (slot < 8) {
            surf.slot_offset[slot] = p0->offset;
            surf.slot_nvmap[slot]  = static_cast<u32>(gb->nvmap_id);
            if (slot + 1 > surf.num_slots) { surf.num_slots = slot + 1; }
        }
        surf.buf_size     = gb->total_size;
        surf.color_format = p0->color_format;
        surf.layout       = p0->layout;
        surf.nvmap_id     = static_cast<u32>(gb->nvmap_id);
        surf.width        = p0->width;
        surf.height       = p0->height;
        /* v0.7.17: a 16-bit RGB565 surface (libnx's framebuffer: DBI, #42) has
         * 2 bytes a pixel - its stride was taken as half its width */
        const bool rgb565 = p0->color_format == 0x010A881210ULL;
        surf.stride_px    = p0->pitch / (rgb565 ? 2 : 4);
        surf.block_h_log2 = p0->block_height_log2;
        /* NvColorFormat -> VIC pixel format. A8B8G8R8 is the swapchain default. */
        surf.pix_format   = rgb565 ? vic::PIXFMT_B5G6R5
                          : (p0->color_format == 0x0100D12120ULL) ? vic::PIXFMT_A8R8G8B8 : vic::PIXFMT_A8B8G8R8;
        surf.armed        = true;

        LogLine("   captured %s slot %u: nvmap=%u %ux%u stride_px=%u blk_h_log2=%u off=0x%x pixfmt=%u (slots=%u)",
                who, slot, surf.nvmap_id, surf.width, surf.height,
                surf.stride_px, surf.block_h_log2, p0->offset,
                surf.pix_format, surf.num_slots);
    }
    }

    void CaptureGameSurface(const NvGraphicBufferRaw *gb, s32 slot_arg) { CaptureInto(g_game_surface, "game", gb, slot_arg); }
    void CaptureAppletSurface(const NvGraphicBufferRaw *gb, s32 slot_arg) { CaptureInto(g_applet_surface, "applet", gb, slot_arg); }

    namespace {

        constinit std::atomic<s32> g_vic_pending{-1};
        /* v0.7.10: RunVicBlit gave the donor's memory back - run it again (it
         * waits for the next viewer) */
        constinit std::atomic<bool> g_rearm{false};
        /* v0.7.14: the engine setup failed - wait before the worker re-arms,
         * so a lasting refusal does not launch and end the donor in a loop */
        constinit bool g_setup_failed = false;
        /* test: sdmc:/config/switch-frame-tap/fault-nv.txt fails the first
         * engine setup of the boot at the nvdrv transfer memory, to check the
         * recovery (the stream should start again ~5 s later) */
        bool FaultInjectNvTmemOnce() {
            static bool done = false;
            if (done) { return false; }
            done = true;
            fs::DirectoryEntryType t;
            if (R_FAILED(fs::GetEntryType(std::addressof(t), "sdmc:/config/switch-frame-tap/fault-nv.txt"))) { return false; }
            LogLine("   test: fault-nv.txt - failing this engine setup once");
            return true;
        }
        constinit s32 g_last_slot = 0;
        alignas(os::ThreadStackAlignment) constinit u8 g_vic_stack[24_KB];   /* v0.7.13: 32, 10 used streaming with screenshots */
        constinit os::ThreadType g_vic_thread;

    void RunVicBlit(s32 slot) {
        /* v0.7.14: with home_menu on, no game is needed (the HOME menu streams) */
        if (!g_vic_armed || (!g_game_surface.armed && !g_ui_stream_armed)) { return; }
        bool expected = false;
        if (!g_blit_done.compare_exchange_strong(expected, true)) { return; }

        const u32 sidx = (slot >= 0 && static_cast<u32>(slot) < g_game_surface.num_slots)
                       ? static_cast<u32>(slot) : 0;
        const u32 src_off = g_game_surface.slot_offset[sidx];

        /* declared before the first goto: a jump may not bypass an initialised
         * declaration, and close_vic reads these to decide what to unpin. */
        u32 cfg_addr = 0, dst_addr = 0, src_addr = 0, self_addr = 0;

        /* v0.7.10 measurement: in live mode, set up nothing - no heap, no
         * nvdrv, no channels - until a viewer reads. With no viewer the module
         * then holds only its image and static buffers in the Applet group,
         * and the web applet test shows whether that alone fits. */
        for (u32 heap_fails = 0;; ++heap_fails) {
        if (g_live_armed && g_release_mode) {
            if (heap_fails == 0) { LogLine("   engines: set up when a viewer connects (until then: no heap, no channels)"); }
            VicStage("vb:wait_viewer");
            /* a screenshot needs the engines too, viewer or not; neither while
             * an applet such as the browser holds the Applet pool - the donor
             * would take its memory and the applet abort (2210-1024) */
            for (bool said_busy = false;;) {
                /* a viewer, and (v0.7.11) a game that draws - not the HOME menu */
                for (;;) {
                    UsbFollowStreamSwitch();
                    if (ShotRequestPending()) { break; }
                    if ((g_ambi_armed && NetIp() != 0) || UsbViewerPresent(300)) {
                        /* v0.7.14: the HOME menu streams too (home_menu = 1) */
                        /* v0.7.17: not while a web applet's menu is over the game (WebMenuUp) */
                        if (WebMenuUp()) {
                            SetViewerReason(ViewerReason_AppletOpen);
                            /* v0.7.16 review: once - every 100 ms it wrote the log and last.txt (41 lines with the eShop open 6 s) */
                            static const char *const WebMenuStage = "vb:web_menu";
                            if (g_vic_stage.load(std::memory_order_relaxed) != WebMenuStage) { VicStage(WebMenuStage); }
                            os::SleepThread(TimeSpan::FromMilliSeconds(100));
                            continue;
                        }
                        if (g_ui_stream_armed) { break; }
                        const u32 q0 = g_queue_count.load(std::memory_order_relaxed);
                        os::SleepThread(TimeSpan::FromMilliSeconds(250));
                        if (g_queue_count.load(std::memory_order_relaxed) != q0) { break; }
                        ::ams::os::ProcessId app{};
                        const bool have_app = g_pmdmnt_rc == 0 && R_SUCCEEDED(::ams::pm::dmnt::GetApplicationProcessId(std::addressof(app)));
                        SetViewerReason(have_app ? ViewerReason_GameHidden : ViewerReason_NoGame);
                        VicStage("vb:wait_game");
                    }
                    os::SleepThread(TimeSpan::FromMilliSeconds(500));
                }
                u64 mb = 0;
                const bool hold = AppletHoldActive();
                if (!hold && !AppletBusy(std::addressof(mb))) { break; }
                if (TakeShotRequest()) {
                    LogLine("shot: an applet holds the Applet pool (%llu MB) - no screenshot now", static_cast<unsigned long long>(mb));
                    NotifyShot(KeyboardHoldsStream() ? "No screenshot while the keyboard is open" : "No screenshot while a manual, the eShop or another applet is open");
                }
                if (!said_busy) { LogLine("   engines: %s - waiting for it to close", hold ? "an applet is starting" : "an applet holds the Applet pool"); said_busy = true; }
                VicStage("vb:applet_busy");
                SetViewerReason(KeyboardHoldsStream() ? ViewerReason_Keyboard : ViewerReason_AppletOpen);
                os::SleepThread(TimeSpan::FromMilliSeconds(250));   /* v0.7.16: was 2 s - up to 2 s more before the picture after every applet */
            }
            g_applet_needs_memory.store(false, std::memory_order_relaxed);
            SetViewerReason(ViewerReason_Starting);
            LogLine("   engines: %s - setting up", ShotRequestPending() ? "a screenshot was asked for" : "a viewer is reading");
        }
        VicStage("vb:0_heap");
        if (AllocVicHeap()) { break; }
        VicStage("vb:0_FAILED");
        /* v0.7.10: no memory from the donor (an applet just closed, or the
         * donor is not installed): wait and try again, not give up for the boot */
        if (!(g_live_armed && g_release_mode)) { return; }
        if (TakeShotRequest()) { NotifyShot("Screenshot failed: not enough memory free"); }
        if (heap_fails % 12 == 0) { LogLine("   engines: no streaming memory - trying again in 5 s"); }
        SetViewerReason(ViewerReason_NoMemory);
        os::SleepThread(TimeSpan::FromSeconds(5));
        }
        /* the test paths (no live mode, or the colour sweep) use the capture
         * region from here on, for the whole boot, as before v0.7.10 */
        if ((!g_live_armed || g_csc_armed) && !CaptureAcquire("tests")) { VicStage("vb:0_FAILED"); return; }

        VicStage("vb:1_smGetService");
        /* Service name decides the NvDrvPermission mask, and phys=0 on the
         * game's handle is a permissions story, not a bug:
         *   nvdrv:s (sysmodules) 0x439E  - no bit 10 (SetAruidWithoutCheck),
         *                                  no bit 12 (import EXPORTED handles),
         *                                  no bit 15 (full VA range - and indeed
         *                                  our IOVAs sit in the restricted
         *                                  0xE0000000+ window)
         *   nvdrv:t (factory)    0xFFFFFFFF - everything
         * nvmap objects are bound to an AppletResourceUserId, so importing
         * another process's buffer plausibly needs the aruid bits we lack. */
        ::Result rc = 0;
        const char *nv_svc = "nvdrv:t";
        /* v0.7.14: up here so the failures below can jump to close_sess */
        u64 &nv_tmem_size = g_nv_tmem_size;
        {
            SmUse sm_nv;   /* v0.7.10: sm only for this (the live loop never returns) */
            rc = smGetService(std::addressof(g_nv_srv), "nvdrv:t");
            if (R_FAILED(rc)) {
                LogLine("   nvdrv:t unavailable rc=0x%x - falling back", rc);
                rc = smGetService(std::addressof(g_nv_srv), "nvdrv:s");
                nv_svc = "nvdrv:s";
            }
        }
        LogLine("   using %s rc=0x%x", nv_svc, rc);
        /* v0.7.14: a failed setup goes through close_sess like the later
         * ones - a bare return kept g_blit_done set and the donor's memory
         * held, so nothing streamed (and the browser applets had no memory)
         * until a reboot. close_sess frees the donor and re-arms the worker. */
        if (R_FAILED(rc)) { VicStage("vb:1_FAILED"); g_setup_failed = true; goto close_sess; }

        VicStage("vb:2_tmem");
        if (FaultInjectNvTmemOnce() || !DonorTakeNvTmem(std::addressof(g_nv_tmem_handle), std::addressof(nv_tmem_size))) {
            LogLine("   nvdrv transfer memory: none came with the donor's memory - no engines");
            VicStage("vb:2_FAILED"); g_setup_failed = true; goto close_sess;
        }

        VicStage("vb:3_Initialize");
        {
            const u32 tmem_size = static_cast<u32>(nv_tmem_size);
            rc = serviceDispatchIn(std::addressof(g_nv_srv), 3, tmem_size,
                .in_num_handles = 2,
                .in_handles     = { CUR_PROCESS_HANDLE, g_nv_tmem_handle });
            LogLine("   Initialize rc=0x%x", rc);
            if (R_FAILED(rc)) { VicStage("vb:3_FAILED"); goto close_sess; }
        }

        u32 nvmap_fd, nverr;

        /* --- aruid discovery on a THROWAWAY fd ----------------------------
         * M21 found the owner (aruid 142) and adopted it successfully, yet the
         * pin still returned 0 - because we adopted it *after* opening
         * /dev/nvmap. libnx sets the aruid during Initialize, before opening
         * any device node, so an fd evidently captures the client identity at
         * open time. Discover on a scratch fd, close it, adopt, and only then
         * open the fd we actually use.
         *
         * M86b: NOT in live mode. Adopting the game's aruid makes every
         * channel and buffer this session opens count as the GAME's with
         * nvservices; Run L's relaunch hung after the game exited while they
         * were all still open. Live mode touches only our own buffers, which
         * never needed the aruid (the VIC worked on them from M16, before
         * M21 adopted it). */
        if (g_live_armed) {
            LogLine("   live mode: our own nvdrv identity (no game aruid, no FROM_ID)");
        } else {
            VicStage("vb:4a_discover_aruid");
            u32 tfd = 0, terr = 0;
            if (R_SUCCEEDED(NvOpen("/dev/nvmap", std::addressof(tfd), std::addressof(terr))) && terr == 0) {
                struct { u32 id; u32 handle; } a = { g_game_surface.nvmap_id, 0 };
                u32 e = 0;
                if (R_SUCCEEDED(NvIoctl(tfd, NvmapIocFromId, std::addressof(a), sizeof(a), std::addressof(e))) && e == 0) {
                    for (u64 v = 1; v <= 256 && g_game_aruid == 0; v++) {
                        if (NvIsOwnedByAruid(tfd, v, a.handle)) { g_game_aruid = v; }
                    }
                }
                NvClose(tfd);
            }
            LogLine("   discovered game aruid = %llu (on a throwaway fd, now closed)",
                    static_cast<unsigned long long>(g_game_aruid));

            if (g_game_aruid != 0) {
                u32 aerr = 0;
                const ::Result ar = NvSetAruidWithoutCheck(g_game_aruid, std::addressof(aerr));
                LogLine("   SetAruidWithoutCheck(%llu) BEFORE any real Open: rc=0x%x err=%u",
                        static_cast<unsigned long long>(g_game_aruid), ar, aerr);
            }
        }

        VicStage("vb:4_open_nvmap");
        rc = NvOpen("/dev/nvmap", std::addressof(nvmap_fd), std::addressof(nverr));
        LogLine("   /dev/nvmap rc=0x%x fd=%u nverr=%u", rc, nvmap_fd, nverr);
        if (R_FAILED(rc) || nverr != 0) { VicStage("vb:4_FAILED"); goto close_sess; }

        {
            u32 gfd = 0, gerr = 0;
            const ::Result gr = NvOpen("/dev/nvhost-gpu", std::addressof(gfd), std::addressof(gerr));
            LogLine("   perm probe: /dev/nvhost-gpu rc=0x%x nverr=%u -> %s",
                    gr, gerr, (R_SUCCEEDED(gr) && gerr == 0) ? "OPEN (bit0 set: full mask)"
                                                             : "denied (restricted mask)");
            if (R_SUCCEEDED(gr) && gerr == 0) { NvClose(gfd); }
        }

        u32 src_handle;
        src_handle = 0;   /* M86b: stays 0 in live mode (no FROM_ID); assigned, not initialised - gotos above jump past */
        /* --- what did the full mask actually open? -------------------------
         * Reading another process's swapchain is structurally out (nvservices
         * maps client memory through the process handle we gave it at
         * Initialize, and the game's pages are not in our process). So survey
         * the nodes the full mask now reaches - particularly the DISPLAY group,
         * bit 8 - because a post-composition source needs no foreign handle at
         * all, and would capture the home menu and overlays too. */
        {
            VicStage("vb:4b_node_survey");
            static const char *const nodes[] = {
                "/dev/nvhost-display", "/dev/nvdisp-ctrl", "/dev/nvdisp-disp0",
                "/dev/nvdisp-disp1",   "/dev/nvdcutil-disp0", "/dev/nvcec-ctrl",
                "/dev/nvhost-as-gpu",  "/dev/nvhost-ctrl-gpu", "/dev/nvhost-msenc",
                "/dev/nvhost-nvdec",   "/dev/nvhost-tsec",     "/dev/nvhost-nvjpg",
            };
            for (const char *n : nodes) {
                u32 fd = 0, e = 0;
                const ::Result r = NvOpen(n, std::addressof(fd), std::addressof(e));
                const bool ok = R_SUCCEEDED(r) && e == 0;
                LogLine("   node %-22s %s (rc=0x%x nverr=%u)", n, ok ? "OPEN" : "denied", r, e);
                if (ok) { NvClose(fd); }   /* channels are exclusive - never leak one */
            }
        }

        VicStage("vb:5_FROM_ID");
        /* M86b: NOT in live mode. FROM_ID takes a reference on the game's
         * swapchain nvmap object, whose memory is the game's own (its
         * transfer memory to nvservices). Run L held it past the game's exit
         * - the one-shot path releases it at vb:released, live never gets
         * there - and the relaunched game hung on a black screen, needing a
         * forced power-off. Live mode reads frames with the debug SVCs and
         * never needs this handle. */
        if (!g_live_armed) {
            struct { u32 id; u32 handle; } a = { g_game_surface.nvmap_id, 0 };
            rc = NvIoctl(nvmap_fd, NvmapIocFromId, std::addressof(a), sizeof(a), std::addressof(nverr));
            LogLine("   FROM_ID(%u) under aruid %llu rc=0x%x nverr=%u -> handle=%u",
                    g_game_surface.nvmap_id, static_cast<unsigned long long>(g_game_aruid),
                    rc, nverr, a.handle);
            if (R_FAILED(rc) || nverr != 0) { VicStage("vb:5_FAILED"); goto close_sess; }
            src_handle = a.handle;
        }

        u32 cfg_handle, cfg_id, cmd_handle, cmd_id, dst_handle, dst_id, self_handle, self_id;
        VicStage("vb:6_alloc_bufs");
        rc = NvmapOwn(nvmap_fd, g_vic_cfg_buf, VicCfgSize, 0, std::addressof(cfg_handle), std::addressof(cfg_id));
        if (R_FAILED(rc)) { VicStage("vb:6_cfg_FAILED"); goto close_sess; }
        rc = NvmapOwn(nvmap_fd, g_vic_cmd_buf, VicCmdSize, 0, std::addressof(cmd_handle), std::addressof(cmd_id));
        if (R_FAILED(rc)) { VicStage("vb:6_cmd_FAILED"); goto close_sess; }
        rc = NvmapOwn(nvmap_fd, g_vic_dst_buf, VicDstSize, 0, std::addressof(dst_handle), std::addressof(dst_id));
        if (R_FAILED(rc)) { VicStage("vb:6_dst_FAILED"); goto close_sess; }
        rc = NvmapOwn(nvmap_fd, g_vic_src_buf, VicSrcSize, 0, std::addressof(self_handle), std::addressof(self_id));
        if (R_FAILED(rc)) { VicStage("vb:6_self_FAILED"); goto close_sess; }
        AMS_UNUSED(cfg_id, cmd_id, dst_id, self_id);
        AMS_UNUSED(cfg_handle, dst_handle, src_handle, src_off);   /* Phase A: relocs disabled */

        u32 vfd, syncpt;
        VicStage("vb:7_open_vic");
        rc = NvOpen("/dev/nvhost-vic", std::addressof(vfd), std::addressof(nverr));
        LogLine("   /dev/nvhost-vic rc=0x%x fd=%u nverr=%u", rc, vfd, nverr);
        if (R_FAILED(rc) || nverr != 0) { VicStage("vb:7_FAILED"); goto close_sess; }
        {
            struct { u32 module_id; u32 syncpt; } gs = { 0, 0 };
            rc = NvIoctl(vfd, NvHostIocChannelGetSyncpoint, std::addressof(gs), sizeof(gs), std::addressof(nverr));
            LogLine("   GET_SYNCPOINT rc=0x%x nverr=%u -> syncpt=%u", rc, nverr, gs.syncpt);
            if (R_FAILED(rc) || nverr != 0) { VicStage("vb:7_syncpt_FAILED"); goto close_vic; }
            syncpt = gs.syncpt;
        }

        VicStage("vb:8_set_nvmap_fd");
        {
            struct { u32 fd; } a = { nvmap_fd };
            rc = NvIoctl(vfd, NvHostIocChannelSetNvmapFd, std::addressof(a), sizeof(a), std::addressof(nverr));
            LogLine("   SET_NVMAP_FD(%u) rc=0x%x nverr=%u", nvmap_fd, rc, nverr);
        }
        {
            struct { u32 timeout; } a = { 1000 };
            rc = NvIoctl(vfd, NvHostIocChannelSetSubmitTo, std::addressof(a), sizeof(a), std::addressof(nverr));
            LogLine("   SET_SUBMIT_TIMEOUT rc=0x%x nverr=%u", rc, nverr);
        }

        if (g_vic_execute) {
            /* v0.7.1: device address 0 is a real address the channel hands out
             * when it is free - and to every check here it means "not pinned"
             * (a zero address hangs the VIC). One boot gave it to cfg, and every
             * job of that boot was refused: no stream at all. A page of our own,
             * mapped first and never unmapped, takes it instead. It sits in the
             * unused gap between the command buffer and dst. */
            {
                VicStage("vb:8_guard");
                g_guard_nvmap_fd = nvmap_fd;
                u32 gh = 0, gid = 0, gaddr = 0;
                if (R_SUCCEEDED(NvmapOwn(nvmap_fd, reinterpret_cast<u8 *>(g_vic_heap + 0x8000), 0x1000, 0, std::addressof(gh), std::addressof(gid)))) {
                    MapCmdBuffer(vfd, gh, std::addressof(gaddr), "guard (holds device address 0)");
                }
            }
            VicStage("vb:8a_map_cfg");
            if (!MapCmdBuffer(vfd, cfg_handle, std::addressof(cfg_addr), "cfg")) { VicStage("vb:8a_FAILED"); goto close_vic; }
            VicStage("vb:8b_map_dst");
            if (!MapCmdBuffer(vfd, dst_handle, std::addressof(dst_addr), "dst")) { VicStage("vb:8b_FAILED"); goto close_vic; }
            /* The game's imported handle pins with nverr=0 but phys=0. Try every
             * variant before giving up; none of these submit anything.
             * M86b: never in live mode - there is no imported handle. */
            VicStage("vb:8c_map_src");
            if (src_handle != 0) {
                MapCmdBuffer(vfd, src_handle, std::addressof(src_addr), "src(game)", 0);
                if (src_addr == 0) { MapCmdBuffer(vfd, src_handle, std::addressof(src_addr), "src(game)", 1); }
                if (src_addr == 0) { MapCmdBuffer(vfd, src_handle, std::addressof(src_addr), "src(game)", 0, NvHostIocChannelMapCmdBufEx); }
            }
            VicStage("vb:8d_map_self");
            MapCmdBuffer(vfd, self_handle, std::addressof(self_addr), "src(ours)", 0);
            LogLine("   pinned: cfg=0x%x dst=0x%x self=0x%x game_src=0x%x (+slot off 0x%x)",
                    cfg_addr, dst_addr, self_addr, src_addr, src_off);
            /* v0.7.17: one of ours at device address 0 - the hole opened after
             * the guard was mapped (Super Mario 3D All-Stars: Galaxy quit to
             * the title list between the two). Every job of the setup is
             * refused then ("cfg 0 - not running"), until a reboot. Close it
             * all and set up again at once: the address is ours to free, and
             * the next guard takes it. */
            if (cfg_addr == 0 || dst_addr == 0 || self_addr == 0) {
                LogLine("   engines: a buffer of ours got device address 0 (cfg=0x%x dst=0x%x self=0x%x) - setting up again", cfg_addr, dst_addr, self_addr);
                VicStage("vb:8_zero_retry");
                goto close_vic;
            }

            /* Paint a recognisable pattern into the source we own, so a correct
             * blit is provable byte-for-byte rather than just "non-zero". */
            for (u32 y = 0; y < DstH; y++) {
                u8 *row = g_vic_src_buf + y * DstPitch;
                for (u32 x = 0; x < DstW; x++) {
                    row[x * 4 + 0] = 0xFF;                      /* A */
                    row[x * 4 + 1] = static_cast<u8>(x * 4);    /* R ramps across */
                    row[x * 4 + 2] = static_cast<u8>(y * 4);    /* G ramps down   */
                    row[x * 4 + 3] = 0x11;                      /* B constant     */
                }
            }
            armDCacheFlush(g_vic_src_buf, VicSrcSize);
            LogLine("   self-src pattern: row0[0..8]=%02x %02x %02x %02x %02x %02x %02x %02x",
                    g_vic_src_buf[0], g_vic_src_buf[1], g_vic_src_buf[2], g_vic_src_buf[3],
                    g_vic_src_buf[4], g_vic_src_buf[5], g_vic_src_buf[6], g_vic_src_buf[7]);
        }

        {
            const SrcDesc game_src{ g_game_surface.width, g_game_surface.height,
                                    g_game_surface.stride_px, vic::BLK_KIND_GENERIC_16Bx2,
                                    g_game_surface.block_h_log2, g_game_surface.pix_format,
                                    vic::CACHE_WIDTH_64Bx4,
                                    g_game_surface.width, g_game_surface.height };
            const JobCtx ctx{ vfd, cmd_handle, syncpt, cfg_addr, dst_addr,
                              (self_addr != 0) ? self_addr : src_addr, 0,
                              cfg_handle, dst_handle, src_handle, game_src };
            /* Once the VIC stops completing it is wedged; further submits achieve
             * nothing and keep the compositor starved. Stop at the first failure. */

            /* 1. Control: known good since M16. */
            if (!RunOneJob("vb:job_fill", VicJob::Fill, true, ctx)) {
                LogLine("   fill did not complete - engine wedged, skipping the rest");
                VicStage("vb:ABORTED_after_fill");
            } else if (self_addr == 0) {
                LogLine("   no address for our own source buffer; nothing further to try");
                VicStage("vb:no_self_addr");
            } else {
                /* 2. A genuine blit from a buffer WE own, so every address the
                 *    engine touches is one MAP_CMD_BUFFER actually gave us. */
                RunOneJob("vb:job_blit_self", VicJob::BlitSelf, true, ctx);
            }

            /* The game's buffer stays out of reach: its handle pins to phys=0
             * three different ways, and RunOneJob now refuses zero addresses. */
            if (src_addr == 0) {
                LogLine("   game blit still not possible: handle pins to phys=0");
                VicStage("vb:game_blit_SKIPPED");
            }
        }
        VicStage("vb:ALL_JOBS_DONE");

#if SFT_OVL_PROBE
        /* v0.7.14 ovl2: the blend card, at the first setup of the boot (it
         * returns at once without probe-blend.txt) */
        {
            static bool s_blend_tried = false;
            if (!s_blend_tried && self_addr != 0 && cfg_addr != 0 && dst_addr != 0 && !g_engine_wedged) {
                s_blend_tried = true;
                TryBlendCard(vfd, cmd_handle, syncpt, cfg_addr, dst_addr, self_addr);
            }
        }
#endif

        /* v0.7.14: the overlays' blend check, at the first setup of the boot */
        if (OverlaysArmed()) {
            static bool s_checked = false;
            if (!s_checked && self_addr != 0 && cfg_addr != 0 && dst_addr != 0 && !g_engine_wedged) {
                s_checked = true;
                OverlaySetCalibrated(OverlayBlendCheck(vfd, cmd_handle, syncpt, cfg_addr, dst_addr, self_addr));
            }
        }

        /* M82: VIC arithmetic on our own buffers, before anything touches the
         * game or another engine */
        if (g_csc_armed && self_addr != 0 && cfg_addr != 0 && dst_addr != 0 && !g_engine_wedged) {
            TryCscSweep(vfd, cmd_handle, syncpt, cfg_addr, dst_addr, self_addr);
        }

        /* M86b: the vi:m indirect-layer probe is a closed route (M47) and
         * touches the display service; live mode skips it */
        if (!g_live_armed) { TryIndirectCapture(); }
        /* v0.7.14: the overlays' buffers live past the self-tests' 64 KB of our
         * output buffer, which streaming does not use */
        if (dst_addr != 0) {
            OverlayAreaUp(g_vic_dst_buf + DstSize, dst_addr + static_cast<u32>(DstSize), VicDstSize - DstSize,
                          reinterpret_cast<u8 *>(g_vic_heap + VicBounceOff + VicBounceOverlayOff), VicBounceSize - VicBounceOverlayOff);
        }
        TryDebugCapture(vfd, nvmap_fd, cmd_handle, syncpt, cfg_addr, dst_addr);
        /* M75: gated. This has completed cleanly in every run (its cmdbuf is an
         * IMMEDIATE increment with no engine op), but the point of an observer
         * run is to touch no engine channel at all. */
        /* M81: the observer goes first. It touches no engine, and this way it
         * reads grc's status blocks before any job of ours has shared NVENC
         * with grc. */
        if (g_grcscan_armed) { TryGrcScan(); }
        /* M76: the positive control goes first among the engine jobs. If it
         * wedges the engine, nothing after it would be informative anyway. */
        if (g_jpgdec_armed) { TryNvjpgDecodeControl(nvmap_fd, cmd_handle, dst_handle); }
        if (g_nvjpg_armed) { TryNvjpgProbe(nvmap_fd, cmd_handle, dst_handle); }

    close_vic:
        OverlayAreaDown();   /* waits for the overlays' thread to leave the buffer */
        if (cfg_addr != 0) { UnmapCmdBuffer(vfd, cfg_handle); }
        if (dst_addr != 0) { UnmapCmdBuffer(vfd, dst_handle); }
        if (src_addr != 0)  { UnmapCmdBuffer(vfd, src_handle); }
        if (self_addr != 0) { UnmapCmdBuffer(vfd, self_handle); }
        NvClose(vfd);
    close_sess:
        serviceClose(std::addressof(g_nv_srv));
        /* back with the donor's memory: closed when the donor goes, or used
         * again if that memory is kept for the next stream (an unmap refused) */
        if (g_nv_tmem_handle != os::InvalidNativeHandle) { DonorAcceptNv(g_nv_tmem_size, g_nv_tmem_handle); g_nv_tmem_handle = os::InvalidNativeHandle; }
        VicStage("vb:released");
        /* v0.7.10: closing the nvdrv session freed every fd and nvmap handle
         * on the donor's memory; unmap it, end the donor, and let the worker
         * set up again for the next viewer */
        if (g_donor_mode) {
            DonorRelease();
            g_donor_mode = false;
            g_vic_heap = 0;
            g_vic_heap_size = 0;
            g_vic_cfg_buf = g_vic_cmd_buf = g_vic_dst_buf = g_vic_src_buf = nullptr;
            g_ind_buf = nullptr;
            SetCaptureTail(nullptr);
            g_ind_size = 0;
            g_stage_buf = nullptr;
            g_stream_stage[0] = g_stream_stage[1] = nullptr;
            g_ctrl_fd = 0;
            g_guard_nvmap_fd = 0;
            g_guard_next = 0x9000;
            g_blit_done.store(false);
            g_rearm.store(true);
            LogLine("   engines closed, the donor's memory given back - set up again when a viewer connects");
            /* 5 s, doubling to a minute while it keeps failing: each try
             * starts the donor and takes its memory */
            static u32 retry_s = 5;
            if (g_setup_failed) {
                LogLine("   engines: the setup failed - trying again in %u s", retry_s);
                os::SleepThread(TimeSpan::FromSeconds(retry_s));
                retry_s = retry_s >= 30 ? 60 : retry_s * 2;
            } else {
                retry_s = 5;
            }
        }
        g_setup_failed = false;
    }

        /* ---- vi indirect-layer capture ------------------------------------
         * The only mechanism that can satisfy our constraint: 2450 writes the
         * layer image into a type-0x46 buffer, i.e. memory WE supply. Exact
         * marshalling taken from libnx viGetIndirectLayerImageMap, and our 2460
         * call already matched libnx byte-for-byte and returned rc=0.
         *
         * 102 was denied earlier only because the game's session is vi:u with
         * mode 0. Per switchbrew, "passing 1 to vi:s/vi:m results in the
         * IApplicationDisplayService having greater privileges" - so we open
         * vi:m ourselves and ask for mode 1. */
        void TryIndirectCapture() {
            /* GetDisplayService's COMMAND ID is the service type, not 0.
             * libnx: _viCmdGetSession(root, out, inval, g_viServiceType) - the
             * service type is passed as the cmd_id, so vi:u=0, vi:s=1, vi:m=2,
             * with inval 1 for system/manager. Sending cmd 0 to
             * IManagerRootService is an unknown command, which is why vi closed
             * the session on us (0xF601 SessionClosed). A closed session cannot
             * be reused, so each attempt re-opens the root service. */
            VicStage("ind:1_open_vi_root");
            ::Service vi_root = {};
            ::Service disp    = {};
            bool got_disp = false;
            for (const auto &v : { std::tuple<const char *, u32, u32>{ "vi:m", 2, 1 },
                                   std::tuple<const char *, u32, u32>{ "vi:s", 1, 1 } }) {
                const char *name = std::get<0>(v);
                const u32 cmd = std::get<1>(v), mode = std::get<2>(v);

                SmUse sm_vi;
                ::Result r = smGetService(std::addressof(vi_root), name);
                if (R_FAILED(r)) { LogLine("   smGetService(%s) rc=0x%x", name, r); continue; }

                r = serviceDispatchIn(std::addressof(vi_root), cmd, mode,
                    .out_num_objects = 1, .out_objects = std::addressof(disp));
                LogLine("   %s GetDisplayService cmd=%u mode=%u rc=0x%x", name, cmd, mode, r);
                if (R_SUCCEEDED(r)) { got_disp = true; break; }
                serviceClose(std::addressof(vi_root));
            }
            if (!got_disp) { VicStage("ind:2_FAILED"); return; }
            ::Result rc = 0;
            AMS_UNUSED(rc);

            /* what does the privileged session unlock? */
            ::Service mgr_keep = {};
            for (const auto &p : { std::pair<u32, const char *>{ 101, "GetSystemDisplayService" },
                                   std::pair<u32, const char *>{ 102, "GetManagerDisplayService" } }) {
                ::Service sub = {};
                const ::Result r = serviceDispatch(std::addressof(disp), p.first,
                    .out_num_objects = 1, .out_objects = std::addressof(sub));
                LogLine("   %u %-26s rc=0x%x %s", p.first, p.second, r,
                        R_SUCCEEDED(r) ? "GOT IT" : "denied");
                if (R_SUCCEEDED(r)) { serviceClose(std::addressof(sub)); }
            }

            VicStage("ind:3_required_memory");
            for (const auto &d : { std::pair<s32, s32>{ 1280, 720 }, std::pair<s32, s32>{ 1920, 1080 } }) {
                const struct { s64 w; s64 h; } in = { d.first, d.second };
                struct { s64 size; s64 align; } out = {};
                const ::Result r = serviceDispatchInOut(std::addressof(disp), 2460, in, out);
                LogLine("   2460(%dx%d) rc=0x%x -> size=%lld align=%lld",
                        d.first, d.second, r,
                        static_cast<long long>(out.size), static_cast<long long>(out.align));
            }

            /* Open a display on OUR privileged session, then make a real
             * indirect layer. 0x60A is ams::sf::PreconditionViolation - the
             * marshalling was accepted and only the consumer handle was bogus,
             * so a genuine handle is the whole remaining gap.
             *
             * 2050 CreateIndirectLayer is undocumented, but its sibling
             * viCreateManagedLayer(2010) is { u32 flags; u32 pad; u64 display_id;
             * u64 aruid; } -> u64. Probing shapes is safe: a wrong raw size
             * answers InvalidCmifHeaderSize(0x1940A) and a missing command
             * answers UnknownMethodId(0x1BA0A), so the errors discriminate. */
            u64 display_id = 0;
            {
                struct { char data[0x40]; } name = {};
                std::strncpy(name.data, "Default", sizeof(name.data) - 1);
                const ::Result r = serviceDispatchInOut(std::addressof(disp), 1010, name, display_id);
                LogLine("   1010 OpenDisplay(\"Default\") rc=0x%x -> display_id=%llu",
                        r, static_cast<unsigned long long>(display_id));
            }

            u64 consumer = 0;
            if (R_SUCCEEDED(serviceDispatch(std::addressof(disp), 102,
                                            .out_num_objects = 1, .out_objects = std::addressof(mgr_keep)))) {
                VicStage("ind:3b_create_indirect_layer");
                /* shape A: { u64 display_id; u64 aruid } -> u64 */
                {
                    const struct { u64 display_id; u64 aruid; } in = { display_id, g_game_aruid };
                    u64 out = 0;
                    const ::Result r = serviceDispatchInOut(std::addressof(mgr_keep), 2050, in, out);
                    LogLine("   2050 shapeA{disp,aruid} rc=0x%x -> handle=%llu", r,
                            static_cast<unsigned long long>(out));
                    if (R_SUCCEEDED(r)) { consumer = out; }
                }
                /* shape B: { u32 flags; u32 pad; u64 display_id; u64 aruid } -> u64 */
                if (consumer == 0) {
                    const struct { u32 flags; u32 pad; u64 display_id; u64 aruid; } in =
                        { 0, 0, display_id, g_game_aruid };
                    u64 out = 0;
                    const ::Result r = serviceDispatchInOut(std::addressof(mgr_keep), 2050, in, out);
                    LogLine("   2050 shapeB{flags,disp,aruid} rc=0x%x -> handle=%llu", r,
                            static_cast<unsigned long long>(out));
                    if (R_SUCCEEDED(r)) { consumer = out; }
                }
                /* shape C: { u64 aruid } -> u64 */
                if (consumer == 0) {
                    const u64 in = g_game_aruid;
                    u64 out = 0;
                    const ::Result r = serviceDispatchInOut(std::addressof(mgr_keep), 2050, in, out);
                    LogLine("   2050 shapeC{aruid} rc=0x%x -> handle=%llu", r,
                            static_cast<unsigned long long>(out));
                    if (R_SUCCEEDED(r)) { consumer = out; }
                }
                LogLine("   consumer handle = %llu", static_cast<unsigned long long>(consumer));

                /* A bare indirect layer has no producer, so there is no image to
                 * return - hence the same PreconditionViolation. The command
                 * names spell out the intended sequence: layer, then a producer
                 * endpoint, then a consumer endpoint. 2050's ABI was guessable
                 * from its documented sibling, so try the same shape here. */
                if (consumer != 0) {
                    VicStage("ind:3c_endpoints");
                    for (const auto &e : { std::pair<u32, const char *>{ 2052, "CreateIndirectProducerEndPoint" },
                                           std::pair<u32, const char *>{ 2054, "CreateIndirectConsumerEndPoint" } }) {
                        {   /* shape {u64 handle; u64 aruid} */
                            const struct { u64 h; u64 aruid; } in = { consumer, g_game_aruid };
                            u64 out = 0;
                            const ::Result r = serviceDispatchInOut(std::addressof(mgr_keep), e.first, in, out);
                            LogLine("   %u %-32s {h,aruid} rc=0x%x -> %llu", e.first, e.second, r,
                                    static_cast<unsigned long long>(out));
                            if (R_SUCCEEDED(r)) { continue; }
                        }
                        {   /* shape {u64 handle} */
                            const u64 in = consumer;
                            u64 out = 0;
                            const ::Result r = serviceDispatchInOut(std::addressof(mgr_keep), e.first, in, out);
                            LogLine("   %u %-32s {h}       rc=0x%x -> %llu", e.first, e.second, r,
                                    static_cast<unsigned long long>(out));
                        }
                    }
                }
            }

            /* 2450 itself. 0x60A on every made-up handle told us the request
             * shape is right and only the handle was wrong, so try the real one
             * first and keep the guesses as a fallback. Each call only writes
             * into our own buffer, so a rejection costs nothing. */
            VicStage("ind:4_image_map");
            /* Pick the largest resolution whose required size fits the heap we
             * were actually granted, rather than assuming 720p fits. */
            s32 cw = 0, ch = 0;
            for (const auto &d : { std::pair<s32, s32>{ 1920, 1080 }, std::pair<s32, s32>{ 1280, 720 },
                                   std::pair<s32, s32>{ 960, 540 },  std::pair<s32, s32>{ 640, 360 } }) {
                const struct { s64 w; s64 h; } q = { d.first, d.second };
                struct { s64 size; s64 align; } a = {};
                if (R_SUCCEEDED(serviceDispatchInOut(std::addressof(disp), 2460, q, a)) &&
                    a.size > 0 && static_cast<size_t>(a.size) <= g_ind_size) {
                    cw = d.first; ch = d.second;
                    LogLine("   capture size %dx%d needs %lld B, buffer is %zu B - OK",
                            cw, ch, static_cast<long long>(a.size), g_ind_size);
                    break;
                }
            }
            if (cw == 0) { LogLine("   no resolution fits the %zu B buffer", g_ind_size); }

            std::memset(g_ind_buf, 0xAB, 0x1000);
            armDCacheFlush(g_ind_buf, g_ind_size);
            const u64 handles[] = { consumer, 0, 1, 2, g_game_aruid };
            for (u64 h : handles) {
                if (cw == 0) { break; }
                const struct { s64 w; s64 h; u64 handle; u64 aruid; } in =
                    { cw, ch, h, g_game_aruid };
                struct { s64 size; s64 stride; } out = {};
                const ::Result r = serviceDispatchInOut(std::addressof(disp), 2450, in, out,
                    .buffer_attrs = { SfBufferAttr_Out | SfBufferAttr_HipcMapAlias |
                                      SfBufferAttr_HipcMapTransferAllowsNonSecure },
                    .buffers      = { { g_ind_buf, g_ind_size } },
                    .in_send_pid  = true);
                LogLine("   2450(%dx%d, handle=%llu, aruid=%llu) rc=0x%x -> size=%lld stride=%lld", cw, ch,
                        static_cast<unsigned long long>(h),
                        static_cast<unsigned long long>(g_game_aruid), r,
                        static_cast<long long>(out.size), static_cast<long long>(out.stride));
                if (R_SUCCEEDED(r)) {
                    armDCacheFlush(g_ind_buf, g_ind_size);
                    u32 changed = 0;
                    for (u32 i = 0; i < 0x1000; i++) { if (g_ind_buf[i] != 0xAB) { changed++; } }
                    LogLine("   *** 2450 SUCCEEDED *** changed=%u/4096  first16: "
                            "%02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                            changed, g_ind_buf[0], g_ind_buf[1], g_ind_buf[2], g_ind_buf[3],
                            g_ind_buf[4], g_ind_buf[5], g_ind_buf[6], g_ind_buf[7],
                            g_ind_buf[8], g_ind_buf[9], g_ind_buf[10], g_ind_buf[11],
                            g_ind_buf[12], g_ind_buf[13], g_ind_buf[14], g_ind_buf[15]);
                    VicStage("ind:CAPTURED");
                    break;
                }
            }

            if (serviceIsActive(std::addressof(mgr_keep))) { serviceClose(std::addressof(mgr_keep)); }
            serviceClose(std::addressof(disp));
            serviceClose(std::addressof(vi_root));
            VicStage("ind:done");
        }

        /* ---- the debugger route -------------------------------------------
         * nvservices will not map another process's pages for us, and vi will
         * not populate an indirect layer without AM. But the kernel has one
         * mechanism designed for exactly this, and Atmosphere's own cheat
         * engine dmnt uses it against running games at 60 Hz:
         *
         *     svcDebugActiveProcess -> svcQueryDebugProcessMemory
         *                           -> svcReadDebugProcessMemory
         *
         * DebugActiveProcess STOPS the target. Our logger does an SD write per
         * line, which would freeze the game for hundreds of ms, so everything
         * is collected silently while attached and logged only after detaching.
         * Closing the debug handle resumes the process. Read-only: the NPDM
         * deliberately omits WriteDebugProcessMemory and TerminateDebugProcess.
         *
         * The three kernel gates, read straight out of mesosphere (M34):
         *
         *   kern_svc_debug.cpp:28   DebugActiveProcess needs
         *                             IsDebugMode() || CanForceDebugProd()
         *   kern_svc_debug.cpp:38   ...and
         *                             target->IsPermittedDebug()
         *                             || CanForceDebug() || CanForceDebugProd()
         *   kern_svc_debug.cpp:232/276  Query/ReadDebugProcessMemory need
         *                             IsDebugMode() || CanForceDebugProd()
         *
         * M33 declared none of those NPDM debug flags, so its attach could only
         * ever have worked if the game itself were marked permitted-debug.
         * M34 adds "force_debug": true - the same flag creport and dmnt.gen2
         * declare - which satisfies the second gate unconditionally.
         *
         * And the read itself is NOT blocked by the framebuffer's attributes:
         * kern_k_page_table_base.cpp:2743 checks state/permission with an
         * attribute mask of None, so MemoryAttribute_DeviceShared - which every
         * nvmap-pinned page carries - does not disqualify the range. That is the
         * whole reason this route can work where nvmap FROM_ID could not.
         *
         * Not done here, deliberately: after attaching, draining GetDebugEvent
         * and calling ContinueDebugEvent(ExceptionHandled | ContinueAll) would
         * let the game keep running while we stay attached - which is how dmnt
         * reads cheat addresses at 60 Hz, and is what a streaming capture would
         * need. Both SVCs are already in our NPDM. This one-shot probe keeps the
         * game stopped instead, because a frozen frame is the cleaner sample. */
        struct RegionHit { u64 base; u64 size; u32 state; u32 perm; u32 attr; u32 devices; };

        /* The swapchain is ONE nvmap object: three slots of 8,847,360 B, laid out
         * contiguously at the offsets the binder parcels already handed us. */
        constexpr u64 FbSlotSize   = 8847360ull;
        constexpr u64 FbSwapSize   = 3ull * FbSlotSize;               /* 26,542,080 */
        constexpr u64 FbSlotOff[3] = { 0ull, 0x870000ull, 0x10E0000ull };

        struct FbCandidate { u64 addr; u64 region_base; u32 lane; u32 varied; u8 head[3][16]; };

        /* One block-row: 120 blocks x 8192 B, covering the full 1920 px width by
         * 128 rows, contiguous. Nine of them are exactly one 8,847,360 B slot. */
        constexpr u64 FbBlockRow = 983040ull;

        /* M35 found it. Region base 0x10851e6000, size EXACTLY 26,542,080 - three
         * slots, and the same size nvmap PARAM reported for handle 1268 - with the
         * opaque-pixel signature at all three slot offsets:
         *
         *   slot0 ff fb ff ff | slot1 ff f7 ff ff | slot2 ff f4 ff ff
         *
         * Same lane constant, the varying lane differing per slot: three
         * successive frames of a near-white top-left corner. That is the
         * swapchain. Exactly one candidate survived ~24,000 probed offsets.
         *
         * Two things M35 got wrong, fixed here:
         *   - it reported the offset against big[0] rather than the region the
         *     candidate was actually found in (cosmetic, but it read as 0x11d79000
         *     when the true answer is offset 0 of an exactly-sized region);
         *   - it scanned 24,000 offsets and burned its whole read budget when an
         *     exactly-26,542,080-byte region is a dead giveaway. Check those
         *     first: the freeze drops from ~340 ms to ~1 ms.
         *
         * ADDRESSES ARE NOT STABLE. The same region was at 0x14f5e6d000 one boot
         * and 0x107346d000 the next. It must be located at runtime, every time. */
        /* M36 matched on an all-0xFF corner, so every lane looked "opaque" and
         * lane 0 won by being first. The real alpha lane is 3: the captured
         * strip reads aa aa af ff / ab ab af ff, i.e. A8B8G8R8 stored
         * little-endian as R,G,B,A. Hence "opaque pixels 0/245760" - the stat
         * counted lane 0, which is red. Two fixes: the fallback scan now really
         * does reject a uniform block (M36 computed `varied` and never acted on
         * it), and the alpha lane is measured from the strip rather than trusted
         * from 16 bytes of white. */
        s32 FbPixelLane(const u8 *p, u32 *out_varied) {
            for (u32 lane = 0; lane < 4; ++lane) {
                bool opaque = true;
                for (u32 i = 0; i < 16; ++i) {
                    if (p[i * 4 + lane] != 0xFF) { opaque = false; break; }
                }
                if (!opaque) { continue; }

                u32 varied = 0;
                for (u32 o = 0; o < 4; ++o) {
                    if (o == lane) { continue; }
                    for (u32 i = 1; i < 16; ++i) {
                        if (p[i * 4 + o] != p[o]) { ++varied; break; }
                    }
                }
                if (varied == 0) { continue; }          /* uniform block - not a frame */
                if (out_varied != nullptr) { *out_varied = varied; }
                return static_cast<s32>(lane);
            }
            return -1;
        }

        /* Stream one whole slot out to the SD card, 64 KB at a time, while the
         * game runs. 8,847,360 B = 120 blocks x 8192 B x 9 block-rows = a
         * 1920x1152 block-linear surface, of which the top 1080 rows are the
         * frame. De-swizzling happens on the PC (tools/deswizzle.py). */
        constexpr const char *FrameBinPath = "sdmc:/applet-mitm-frame.bin";
        constexpr const char *FrameTxtPath = "sdmc:/applet-mitm-frame.txt";

        bool WriteBufToSd(const char *path, const void *buf, size_t len) {
            fs::DeleteFile(path);
            if (R_FAILED(fs::CreateFile(path, static_cast<s64>(len)))) { return false; }
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), path, fs::OpenMode_Write))) { return false; }
            const auto wrc = fs::WriteFile(f, 0, buf, len, fs::WriteOption::Flush);
            fs::CloseFile(f);
            if (R_FAILED(wrc)) { LogLine("   WriteBufToSd(%s) rc=0x%x", path, wrc.GetValue()); }
            return R_SUCCEEDED(wrc);
        }

        /* ---- M78: SD writes that can be trusted ---------------------------
         *
         * M77 Run C decoded correctly on the console, and the nvjpg-dec.rgba it
         * left on the card held recycled FAT clusters - a fragment of this
         * run's own log, Mario Kart asset names - instead of the output. The
         * write handed fs::WriteFile the UNCACHED engine buffer, and
         * mesosphere refuses any IPC buffer with the Uncached attribute
         * (kern_k_page_table_base.cpp, SetupForIpcClient's test_attr_mask
         * for Ipc, NonSecureIpc and NonDeviceIpc alike). CreateFile had
         * already allocated the clusters, so a file of the right size existed
         * and nothing said the write had failed, because its Result was
         * discarded. M41/M42 hit exactly this and left the lesson in the
         * strip-dump comment below; M76 repeated it anyway.
         *
         * So: the source must be cached memory, every fs Result is checked and
         * logged, the file is read back and compared byte for byte, and the
         * FNV-1a of what was written goes in the log, where the PC can compare
         * it with the file it copied off the card. */
        constexpr u32 Fnv1a32(const u8 *p, size_t n) {
            u32 h = 0x811C9DC5u;
            for (size_t i = 0; i < n; ++i) { h = (h ^ p[i]) * 0x01000193u; }
            return h;
        }

        /* `src` must be normal cached memory; `verify` is scratch of at least
         * `len` bytes, also cached. One retry on any failure. */
        bool WriteSdVerified(const char *path, const u8 *src, size_t len, u8 *verify, u32 *out_fnv) {
            const u32 fnv = Fnv1a32(src, len);
            if (out_fnv != nullptr) { *out_fnv = fnv; }
            for (u32 attempt = 1; attempt <= 2; ++attempt) {
                static_cast<void>(fs::DeleteFile(path));   /* absent is fine */
                Result rc = fs::CreateFile(path, static_cast<s64>(len));
                if (R_FAILED(rc)) { LogLine("   sd(%s) #%u CreateFile rc=0x%x", path, attempt, rc.GetValue()); continue; }
                fs::FileHandle f;
                rc = fs::OpenFile(std::addressof(f), path, fs::OpenMode_Write);
                if (R_FAILED(rc)) { LogLine("   sd(%s) #%u OpenFile(write) rc=0x%x", path, attempt, rc.GetValue()); continue; }
                rc = fs::WriteFile(f, 0, src, len, fs::WriteOption::Flush);
                fs::CloseFile(f);
                if (R_FAILED(rc)) { LogLine("   sd(%s) #%u WriteFile(%zu B) rc=0x%x", path, attempt, len, rc.GetValue()); continue; }

                rc = fs::OpenFile(std::addressof(f), path, fs::OpenMode_Read);
                if (R_FAILED(rc)) { LogLine("   sd(%s) #%u OpenFile(read-back) rc=0x%x", path, attempt, rc.GetValue()); continue; }
                size_t got = 0;
                rc = fs::ReadFile(std::addressof(got), f, 0, verify, len);
                fs::CloseFile(f);
                if (R_FAILED(rc) || got != len) {
                    LogLine("   sd(%s) #%u read-back rc=0x%x got %zu of %zu B", path, attempt, rc.GetValue(), got, len);
                    continue;
                }
                size_t bad = 0;
                while (bad < len && verify[bad] == src[bad]) { ++bad; }
                if (bad != len) {
                    LogLine("   sd(%s) #%u read-back DIFFERS at byte %zu (wrote %02x, read %02x)", path, attempt, bad, src[bad], verify[bad]);
                    continue;
                }
                LogLine("   sd(%s): %zu B written, read back identical, fnv1a32=%08x", path, len, fnv);
                return true;
            }
            LogLine("   sd(%s): FAILED after 2 attempts - the file on the card is NOT this data (fnv1a32 would be %08x)", path, fnv);
            return false;
        }

        /* For engine output: copy out of the uncached nvmap buffer into the
         * cached stage buffer first, then write. */
        bool WriteEngineOutputToSd(const char *path, const u8 *uncached_src, size_t len, u32 *out_fnv) {
            const size_t slot = (len + 0xFFF) & ~static_cast<size_t>(0xFFF);
            if (g_stage_buf == nullptr || 2 * slot > StreamStageSize || g_ind_size < FbBlockRowStage + 2 * slot) {
                LogLine("   sd(%s): no cached stage buffer for %zu B - not writing", path, len);
                return false;
            }
            std::memcpy(g_stage_buf, uncached_src, len);
            return WriteSdVerified(path, g_stage_buf, len, g_stage_buf + slot, out_fnv);
        }

        constexpr const char *StripBinPath = "sdmc:/applet-mitm-strip.bin";
        constexpr const char *VicBinPath   = "sdmc:/applet-mitm-vic.bin";

        bool DumpSlotToSd(::ams::svc::Handle dbg, u64 src, u64 *out_ns, u32 *out_chunks) {
            fs::DeleteFile(FrameBinPath);
            if (R_FAILED(fs::CreateFile(FrameBinPath, static_cast<s64>(FbSlotSize)))) { return false; }
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), FrameBinPath, fs::OpenMode_Write))) { return false; }

            const u64 t0 = armTicksToNs(armGetSystemTick());
            u32 chunks = 0;
            bool ok = true;
            for (u64 done = 0; done < FbSlotSize; done += 0x10000) {
                const u64 n = (FbSlotSize - done < 0x10000) ? (FbSlotSize - done) : 0x10000;
                if (!DbgRead(dbg, g_ind_buf, src + done, n)) { ok = false; break; }
                const auto opt = (done + n >= FbSlotSize) ? fs::WriteOption::Flush : fs::WriteOption::None;
                if (R_FAILED(fs::WriteFile(f, static_cast<s64>(done), g_ind_buf, n, opt))) { ok = false; break; }
                ++chunks;
            }
            *out_ns     = armTicksToNs(armGetSystemTick()) - t0;
            *out_chunks = chunks;
            fs::CloseFile(f);
            return ok;
        }

        /* ---- M59: the streaming prototype --------------------------------
         * A composition of proven pieces, not new engine work: the debug-SVC
         * slot read (5.6 ms at 1571 MB/s, M56), the VIC scaled blit (the BEST
         * variant already does a uniform 4x downscale, M43), and the USB bulk
         * transport (M57, byte-exact).
         *
         * LATENCY is the design goal, so the loop never queues. Each iteration
         * takes the slot the game has JUST presented and discards whatever
         * arrived while we were busy - a backlog would only add delay, and a
         * dropped frame is cheaper than a late one.
         *
         * THROUGHPUT comes from overlap. The post is asynchronous, so frame N's
         * transfer runs concurrently with frame N+1's capture. Serially 480x270
         * costs 5.6 + vic + copy + 12.4 ms and caps near 43 fps; overlapped the
         * transfer alone is the bottleneck and 60 fps fits.
         *
         * g_vic_dst_buf is UNCACHED nvmap memory - handing that to any I/O path
         * is what made every VIC dump fail in M41/M42 - so each frame is staged
         * into cached memory that already carries its header, giving one URB
         * per frame rather than two. */
        struct __attribute__((packed)) SftHdrWire {
            u32 magic; u16 version; u16 flags;
            u32 width, height, stride, length, block_h_log2, kind;
        };
        static_assert(sizeof(SftHdrWire) == 32, "must match sft_hdr_t in tools/raw-recv/raw-recv.c");

        /* ---- M60: CPU point-sample downscale, no VIC ---------------------
         * M59c measured the VIC at 119,023 us per full-frame blit - 7x the
         * whole 60 fps budget - and that is the lesser problem. nvnflinger
         * composites on the SAME engine and the SAME syncpoint 12 we submit to,
         * so a tight blit loop starved the compositor: the game fell to 3.8 fps
         * during the run and never recovered. The VIC is fine for a one-shot
         * blit and unusable as a per-frame stage.
         *
         * Reading straight out of the block-linear capture costs no engine time
         * and cannot contend with the compositor. At an exact 4x reduction the
         * arithmetic is unusually kind: output pixel x lands on byte 16*x, which
         * is always a 16-byte group boundary, so each output pixel is one
         * aligned 4-byte read. No filtering - this is a point sample, so it
         * aliases; correctness and frame rate first. */
        inline u32 BlockLinearOffset(u32 byte_x, u32 y) {
            const u32 block_y  = y / 128;          /* block = 16 gobs x 8 rows */
            const u32 y_in_blk = y % 128;
            const u32 gob_y    = y_in_blk / 8;
            const u32 yy       = y_in_blk % 8;
            const u32 block_x  = byte_x / 64;      /* gob is 64 B wide         */
            const u32 xg       = byte_x % 64;
            const u32 base     = (block_y * 120u + block_x) * 8192u;   /* 120 blocks/row */
            const u32 in_gob   = ((xg / 32u) * 256u) + ((yy / 2u) * 64u)
                               + (((xg % 32u) / 16u) * 32u) + ((yy % 2u) * 16u);
            return base + gob_y * 512u + in_gob;
        }

        /* Retained as a fallback: it needs no engine, no pin and no syncpoint,
         * so it still works if the VIC is ever unavailable. M67 streams through
         * the VIC instead - 1.5 bytes/pixel against this one's 4, and 0.8 ms of
         * engine time against ~4.3 ms of CPU. */
        [[maybe_unused]] void DownscalePoint(const u8 *src, u8 *dst, u32 out_w, u32 out_h, u32 step) {
            for (u32 oy = 0; oy < out_h; ++oy) {
                const u32 sy = oy * step;
                u8 *drow = dst + static_cast<size_t>(oy) * out_w * 4;
                for (u32 ox = 0; ox < out_w; ++ox) {
                    std::memcpy(drow + ox * 4, src + BlockLinearOffset(ox * step * 4u, sy), 4);
                }
            }
        }

        void StreamFrames(::ams::svc::Handle dbg, u64 slot_base,
                          u32 vfd, u32 cmd_handle, u32 syncpt,
                          u32 cfg_addr, u32 dst_addr, u32 src_pin,
                          u32 wantW = 0, u32 wantH = 0, u32 wantFrames = 0) {
            const u32 W = (wantW != 0) ? wantW : g_stream_w;
            const u32 H = (wantH != 0) ? wantH : g_stream_h;
            const u32 nframes = (wantFrames != 0) ? wantFrames : g_stream_frames;
            /* M67: 1.5 bytes/pixel. The VIC writes the NV12 plane layout but
             * performs no colour conversion - it packs raw channels (luma
             * plane = B, chroma even = R, chroma odd = G), i.e. 4:2:0
             * subsampled RGB. The host undoes the permutation for free, so we
             * get the 2.67x reduction without the colour matrix that three
             * hardware runs failed to program. */
            const size_t out_bytes  = static_cast<size_t>(W) * static_cast<size_t>(H) * 3 / 2;
            const size_t wire_bytes = sizeof(SftHdrWire) + out_bytes;

            if (out_bytes > StreamStageSize) {
                LogLine("   stream: %ux%u needs %zu B but a stage buffer is %zu - refusing",
                        W, H, out_bytes, StreamStageSize);
                return;
            }
            if (g_stream_stage[0] == nullptr || g_stream_stage[1] == nullptr) {
                LogLine("   stream: stage buffers absent - heap too small"); return;
            }
            /* The VIC scales to arbitrary sizes; only the retired CPU
             * point-sampler needed exact uniform divisors. Even dimensions are
             * still required because chroma is half-resolution in both axes. */
            if (W == 0 || H == 0 || (W & 1) != 0 || (H & 1) != 0) {
                LogLine("   stream: %ux%u must have even dimensions (4:2:0 chroma)", W, H);
                return;
            }
            if (src_pin == 0 || cfg_addr == 0 || dst_addr == 0) {
                LogLine("   stream: engine resources missing (pin=%x cfg=%x dst=%x)", src_pin, cfg_addr, dst_addr);
                return;
            }
            if (!UsbReady()) {
                LogLine("   stream: USB not Configured - nothing to stream to"); return;
            }

            g_vic_quiet = true;
            LogLine("   ---- STREAMING %ux%u packed-420 via VIC, up to %u frames (%zu B/frame) ----",
                    W, H, nframes, wire_bytes);

            /* the header never changes, so write it into both stages once */
            SftHdrWire h = {};
            h.magic  = 0x52544653u;          /* "SFTR" */
            h.version = 1;
            h.width  = W;  h.height = H;  h.stride = W * 4;
            h.length = static_cast<u32>(out_bytes);
            h.block_h_log2 = 0;              /* the VIC output is LINEAR */
            h.kind         = 0;
            h.flags        = 1;              /* 1 = 4:2:0 packed RGB (B / R / G) */
            h.stride       = W;              /* luma-plane pitch in bytes */
            std::memcpy(g_usb_hdr, std::addressof(h), sizeof(h));

            /* source: the captured slot, exactly as the binder parcels describe it */

            /* source: the captured slot, exactly as the binder parcels
             * describe it - block-linear, kind 0xFE, block_height_log2 4 */
            const SrcDesc stream_src{ 1920, 1080, 1920,
                                      vic::BLK_KIND_GENERIC_16Bx2, 4,
                                      vic::PIXFMT_A8R8G8B8, vic::CACHE_WIDTH_64Bx4,
                                      1920, 1080 };

            u32 seen   = g_queue_count.load(std::memory_order_relaxed);
            const u32 q_at_start = seen;
            u32 parity = 0, urb = 0, sent = 0, dropped = 0;
            u32 stale_run = 0, stale_total = 0;
            bool pending = false;
            u64 t_read = 0, t_vic = 0, t_copy = 0, t_wait = 0;
            u64 worst = 0, best = ~UINT64_C(0);
            u32 timed = 0;
            const u64 loop_t0 = armTicksToNs(armGetSystemTick());

            for (u32 i = 0; i < nframes; ++i) {
                /* M59b: capture is NOT gated on the counter any more.
                 * Both M59 runs bailed here with "game stopped presenting"
                 * while the heartbeat showed the game emitting ~109 binder
                 * txns/s across the same window - so g_queue_count was not
                 * reporting what I assumed, and the run died on a diagnostic
                 * rather than on a real failure. Wait briefly for a fresh
                 * frame, then capture regardless: a duplicate frame costs one
                 * transfer and is visible in the output, whereas bailing costs
                 * the entire run. The counter is now only used to pick the slot
                 * and to account for drops. */
                u32 spins = 0;
                while (g_queue_count.load(std::memory_order_relaxed) == seen && spins < 20) {
                    os::SleepThread(TimeSpan::FromMilliSeconds(1));
                    ++spins;
                }
                const u32 now = g_queue_count.load(std::memory_order_relaxed);
                if (now != seen) {
                    dropped += (now - seen) - 1;    /* presented while we were busy */
                    stale_run = 0;
                } else {
                    ++stale_run; ++stale_total;
                    /* M67b: 240 (~5 s) ended a run on a MK8 loading screen,
                     * where the game legitimately presents almost nothing - the
                     * queue counter moved 14 frames in 7.6 s. A loading screen
                     * is not a dead game, so tolerate far more of it. */
                    if (stale_run >= 1200) {        /* ~25 s of genuinely nothing */
                        LogLine("   stream: no new frame for %u iterations - stopping at frame %u", stale_run, i);
                        break;
                    }
                }
                seen = now;

                const s32 slot = g_queue_slot.load(std::memory_order_relaxed);
                const u64 off  = (slot >= 0 && slot < 3) ? FbSlotOff[slot] : 0;

                const u64 f0 = armTicksToNs(armGetSystemTick());
                if (!DbgRead(dbg, g_ind_buf, slot_base + off, FbSlotSize)) {
                    LogLine("   stream: slot read failed at frame %u", i); break;
                }
                /* M60b: NO cache flush. It existed so the VIC could see our
                 * writes through the SMMU; the CPU point-sampler reads its own
                 * cached writes coherently. Flushing 8.8 MB per frame was
                 * 138,240 cache-line ops 60x a second, for nothing. */
                const u64 f1 = armTicksToNs(armGetSystemTick());

                /* The VIC reads our capture through the SMMU, so the cached
                 * writes ReadDebugProcessMemory just made must reach memory
                 * first. M60b dropped this flush because the CPU sampler read
                 * its own cache coherently; the engine does not. */
                armDCacheFlush(g_ind_buf, FbSlotSize);

                g_strip_src = stream_src;
                g_strip_out = OutDesc{ W, H, W, vic::PIXFMT_Y8_U8V8_N420 };
                const JobCtx vc{ vfd, cmd_handle, syncpt, cfg_addr, dst_addr, src_pin, 0, 0, 0, 0, SelfSrc };
                if (!RunOneJob("st:vic", VicJob::BlitStrip, true, vc)) {
                    LogLine("   stream: VIC job failed at frame %u", i); break;
                }
                const u64 f2 = armTicksToNs(armGetSystemTick());

                /* g_vic_dst_buf is UNCACHED nvmap; stage through cached memory
                 * before it goes anywhere (the M41/M42 lesson). */
                std::memcpy(g_stream_stage[parity], g_vic_dst_buf, out_bytes);
                const u64 f3 = armTicksToNs(armGetSystemTick());

                /* the previous transfer has had the whole capture above to
                 * finish in; only now is it allowed to cost us anything */
                if (pending) {
                    size_t got = 0;
                    if (!UsbWaitAsync(urb, std::addressof(got))) {
                        LogLine("   stream: transfer stalled at frame %u", i); pending = false; break;
                    }
                    ++sent;
                }
                const u64 f4 = armTicksToNs(armGetSystemTick());

                size_t hsent = 0;
                if (!UsbSendBuffer(g_usb_hdr, sizeof(SftHdrWire), std::addressof(hsent))) {
                    LogLine("   stream: header send failed at frame %u", i); pending = false; break;
                }
                if (!UsbPostAsync(g_stream_stage[parity], out_bytes, std::addressof(urb))) {
                    LogLine("   stream: post failed at frame %u", i); pending = false; break;
                }
                pending = true;
                parity ^= 1;

                /* M60b: hand the core back. Every thread in this process is
                 * pinned to core 3 (kernel_flags lowest/highest_cpu_id 3),
                 * INCLUDING the mitm IPC thread that answers the game's vi:u
                 * calls. M60's first long run burned ~10 ms of solid CPU per
                 * iteration and starved it: the game stopped presenting for the
                 * entire 8.3 s stream (txn frozen at 3511) and resumed the
                 * instant we stopped. Sleeping is not politeness here, it is
                 * what keeps the game alive. */
                os::SleepThread(TimeSpan::FromMilliSeconds(2));

                t_read += f1 - f0; t_vic += f2 - f1; t_copy += f3 - f2; t_wait += f4 - f3;
                const u64 dt = f4 - f0;
                if (dt > worst) { worst = dt; }
                if (dt < best)  { best  = dt; }
                ++timed;
            }

            if (pending) {
                size_t got = 0;
                if (UsbWaitAsync(urb, std::addressof(got))) { ++sent; }
            }
            g_vic_quiet = false;

            const u64 loop_ns = armTicksToNs(armGetSystemTick()) - loop_t0;
            const u32 n = (timed > 0) ? timed : 1;
            const u64 fps_x10 = (loop_ns > 0) ? (UINT64_C(10000000000) * sent / loop_ns) : 0;
            LogLine("   stream: %u frames sent in %llu ms -> %llu.%llu fps  (%u presented frames dropped)",
                    sent, static_cast<unsigned long long>(loop_ns / 1000000),
                    static_cast<unsigned long long>(fps_x10 / 10),
                    static_cast<unsigned long long>(fps_x10 % 10), dropped);
            LogLine("   stream: avg per frame - read %llu us, vic %llu us, copy %llu us, usb wait %llu us",
                    static_cast<unsigned long long>(t_read / 1000 / n),
                    static_cast<unsigned long long>(t_vic  / 1000 / n),
                    static_cast<unsigned long long>(t_copy / 1000 / n),
                    static_cast<unsigned long long>(t_wait / 1000 / n));
            LogLine("   stream: queue counter %u -> %u over the run; %u stale iterations",
                    q_at_start, g_queue_count.load(std::memory_order_relaxed), stale_total);
            LogLine("   stream: frame time best %llu us, worst %llu us  (60 fps budget = 16667 us)",
                    static_cast<unsigned long long>((best == ~UINT64_C(0)) ? 0 : best / 1000),
                    static_cast<unsigned long long>(worst / 1000));
        }

        /* ---- M86: live mode ---------------------------------------------------
         *
         * The runs so far were one-shot probes: at wait=N seconds attach once,
         * stream a fixed number of frames, detach. "live" turns that into a
         * service that runs for the rest of the boot:
         *
         *   wait for a viewer (UsbViewerPresent: someone is reading the
         *   endpoint) and a game (pm:dmnt has an application, and it is
         *   presenting) -> attach -> find the swapchain -> drain + continue ->
         *   pump -> stream until the viewer or the game goes -> pump off,
         *   detach -> wait again.
         *
         * Closing the viewer, closing the game, or starting another game each
         * end one session and the next one starts on its own. Only an NVENC
         * stall ends it for good (the channel is left open, M74), and so does
         * a swapchain that is never found for the same game (no retry storm:
         * the next game gets a fresh try). The swapchain finder here is the
         * exact-size one - three 1920x1080 A8B8G8R8 slots, what MK8 uses; a
         * game with another layout is reported and skipped. */
        bool FindSwapchainBySize(::ams::svc::Handle dbg, u64 want, u64 *out) {
            u64 addr = 0, best = 0, best_size = ~UINT64_C(0);
            /* v0.7.14: runs of adjacent device-mapped regions. Pokemon Violet
             * keeps its buffers in one 280 MB object; after the stream dropped
             * on a loading screen the re-attach found no region that size or
             * larger (#20, "swapchain (280363008 B) NOT FOUND") - the game had
             * changed part of the mapping, splitting the region's entry in two
             * or more that still sit end to end. */
            u64 run_base = 0, run_size = 0, run_exact = 0, run_best = 0, run_best_size = ~UINT64_C(0);
            u32 run_parts = 0, exact_parts = 0;
            auto end_run = [&]() {
                if (run_parts > 1 && run_size == want && run_exact == 0) { run_exact = run_base; exact_parts = run_parts; }
                if (run_parts > 1 && run_size > want && run_size < run_best_size) { run_best = run_base; run_best_size = run_size; }
                run_size = 0; run_parts = 0;
            };
            for (u32 steps = 0; steps < 4000; ++steps) {
                ::ams::svc::MemoryInfo mi = {};
                ::ams::svc::PageInfo   pi = {};
                if (!DbgQuery(dbg, std::addressof(mi), std::addressof(pi), addr) || mi.size == 0) { break; }
                const u32  attr = static_cast<u32>(mi.attribute);
                const bool dev  = (attr & ::ams::svc::MemoryAttribute_DeviceShared) != 0 || mi.device_count > 0;
                if (dev && mi.size == want) { *out = mi.base_address; return true; }
                /* v0.1.1b: some homebrew keeps its framebuffers inside a larger
                 * device-mapped block (Run AE: two 1280x720 slots at +0xF0000
                 * and +0x4B0000 of one allocation) - the smallest such block
                 * that can hold them, if no region is exactly their size */
                if (dev && mi.size > want && mi.size < best_size) { best = mi.base_address; best_size = mi.size; }
                if (dev) {
                    if (run_parts == 0 || run_base + run_size != mi.base_address) { end_run(); run_base = mi.base_address; }
                    run_size += mi.size; ++run_parts;
                } else {
                    end_run();
                }
                const u64 next = mi.base_address + mi.size;
                if (next <= addr) { break; }
                addr = next;
            }
            end_run();
            if (run_exact != 0) {
                LogLine("   swapchain: %llu B in %u adjacent device-mapped regions at 0x%010llx", static_cast<unsigned long long>(want), exact_parts,
                        static_cast<unsigned long long>(run_exact));
                *out = run_exact;
                return true;
            }
            if (best_size != ~UINT64_C(0)) {
                LogLine("   swapchain: no region of exactly %llu B; using the smallest device-mapped one that holds it (%llu B)",
                        static_cast<unsigned long long>(want), static_cast<unsigned long long>(best_size));
                *out = best;
                return true;
            }
            if (run_best_size != ~UINT64_C(0)) {
                LogLine("   swapchain: no region holds %llu B; using the smallest run of adjacent device-mapped ones that does (%llu B at 0x%010llx)",
                        static_cast<unsigned long long>(want), static_cast<unsigned long long>(run_best_size), static_cast<unsigned long long>(run_best));
                *out = run_best;
                return true;
            }
            return false;
        }

        /* Where each slot's buffer may be, when no region is exactly the
         * swapchain (FindSwapchainBySize fell back to "the smallest region
         * that holds it", which in a homebrew app is as likely its texture
         * memory - Moonlight streamed its box art). Which candidate is which
         * slot is worked out while streaming (VicCaptureSession::Calibrate).
         *
         * One nvmap object per slot (GameSurface::separate - Smash Ultimate,
         * Moonlight): every device-mapped region that is a whole number of
         * buffers, each rounded up to a power-of-two alignment (Moonlight
         * keeps a 3.75 MB 720p buffer in a 4 MB block), gives one candidate
         * per buffer - adjacent buffers can share one memory block.
         * One object for every slot (DuckStation: two slots at 0 and
         * 0x3C0000): every device-mapped region that holds the whole set
         * gives one candidate per slot, its base plus that slot's offset. */
        u32 FindSlotCandidates(::ams::svc::Handle dbg, u64 buf, const u32 *slot_off, u32 nslots, bool separate, u64 *out, u32 max, u32 *devc = nullptr) {
            if (buf == 0 || nslots == 0) { return 0; }
            static constexpr u64 Aligns[] = { 0x1000, 0x10000, 0x20000, 0x40000, 0x100000, 0x200000, 0x400000 };
            u64 span = buf;
            for (u32 k = 0; k < nslots; ++k) { if (slot_off[k] + buf > span) { span = slot_off[k] + buf; } }
            u64 addr = 0;
            u32 n = 0;
            for (u32 steps = 0; steps < 4000 && n < max; ++steps) {
                ::ams::svc::MemoryInfo mi = {};
                ::ams::svc::PageInfo   pi = {};
                if (!DbgQuery(dbg, std::addressof(mi), std::addressof(pi), addr) || mi.size == 0) { break; }
                const u32  attr = static_cast<u32>(mi.attribute);
                const bool dev  = (attr & ::ams::svc::MemoryAttribute_DeviceShared) != 0 || mi.device_count > 0;
                if (dev && separate) {
                    for (const u64 a : Aligns) {
                        const u64 step = (buf + a - 1) & ~(a - 1);
                        if (mi.size % step == 0 && mi.size / step <= 8) {
                            for (u64 o = 0; o < mi.size && n < max; o += step) { if (devc) { devc[n] = mi.device_count; } out[n++] = mi.base_address + o; }
                            break;
                        }
                    }
                } else if (dev && mi.size >= span) {
                    /* v0.7.3: what kind of region - Sonic Frontiers has two 640 MB
                     * pools laid out alike, and the matcher once took the wrong one */
                    LogLine("     region 0x%010llx, %llu MB: state 0x%x attr 0x%x perm 0x%x, device mappings %u, ipc %u",
                            static_cast<unsigned long long>(mi.base_address), static_cast<unsigned long long>(mi.size >> 20),
                            static_cast<u32>(mi.state), attr, static_cast<u32>(mi.permission), mi.device_count, mi.ipc_count);
                    for (u32 k = 0; k < nslots && n < max; ++k) { if (devc) { devc[n] = mi.device_count; } out[n++] = mi.base_address + slot_off[k]; }
                }
                const u64 next = mi.base_address + mi.size;
                if (next <= addr) { break; }
                addr = next;
            }
            return n;
        }

        /* the region the swapchain is in: exactly the set's size (MK8, Smash's
         * first slot), or the smallest that holds it. When that is not the
         * whole story (one object per slot, homebrew in larger blocks) the
         * stream's VicCaptureSession::PrepareSlots finds the slot buffers and matches
         * them while streaming (v0.7.3, from WillMidia's PR #4 - it replaces
         * v0.7.1's calibration before the stream, which held the picture back
         * 2-6 s and never started on a still screen). */
        bool LocateSwapchain(::ams::svc::Handle dbg, GameSurface &geo, u64 *base, const char *who) {
            AMS_UNUSED(who);
            u64 want = FbSwapSize;
            if (geo.num_slots != 0 && geo.buf_size != 0) {
                u32 top = 0;
                for (u32 k = 0; k < geo.num_slots && k < 8; ++k) { if (geo.slot_offset[k] > top) { top = geo.slot_offset[k]; } }
                want = static_cast<u64>(top) + geo.buf_size;
            }
            /* v0.7.14: the same buffer set as the last session (a re-attach
             * after a drop): where it was, if that is still device-mapped */
            static u64 s_base = 0, s_want = 0;
            static u32 s_gen = 0;
            if (FindSwapchainBySize(dbg, want, base)) { s_base = *base; s_want = want; s_gen = geo.generation; return true; }
            if (s_base != 0 && s_want == want && s_gen == geo.generation) {
                ::ams::svc::MemoryInfo mi = {};
                ::ams::svc::PageInfo   pi = {};
                if (DbgQuery(dbg, std::addressof(mi), std::addressof(pi), s_base) &&
                    ((static_cast<u32>(mi.attribute) & ::ams::svc::MemoryAttribute_DeviceShared) != 0 || mi.device_count > 0)) {
                    LogLine("   swapchain: not found by size - using the last session's address 0x%010llx (the same buffers)", static_cast<unsigned long long>(s_base));
                    *base = s_base;
                    return true;
                }
            }
            return false;
        }

        /* v0.7.14: the HOME menu or an applet, read out of vi (applet_mitm_uisource.hpp).
         * false: nothing was streamed (no shared buffer, no memory, no attach) */
        bool RunUiSession(u32 vfd, u32 nvmap_fd, u32 cmd_handle, u32 syncpt, u32 cfg_addr, u32 &sessions) {
            /* v0.7.16: not while a web applet is open - Super Mario 3D
             * All-Stars runs its games with OfflineWeb open; pressing "-"
             * moved its menu onto a layer of its own, this session followed
             * it (upside down) and the console went down a second later */
            if (WebAppletOpen()) {
                static bool said = false;
                if (!said) { LogLine("   live-ui: a web applet is open - the HOME menu is not streamed meanwhile"); said = true; }
                return false;
            }
            u64 vpid = 0, base = 0;
            GameSurface ug = {};
            if (!UiSourceFind(std::addressof(vpid), std::addressof(base), std::addressof(ug))) {
                static bool said = false;
                if (!said) { LogLine("   live-ui: vi's shared buffer not found - the HOME menu is not streamed"); said = true; }
                return false;
            }
            if (!CaptureAcquire("live-ui")) {
                VicStage("live-ui:no_memory");
                SetViewerReason(ViewerReason_NoMemory);
                os::SleepThread(TimeSpan::FromSeconds(2));
                return false;
            }
            SetViewerReason(ViewerReason_Starting);
            ::ams::svc::Handle dbg = ::ams::svc::InvalidHandle;
            const u64 f0 = armTicksToNs(armGetSystemTick());
            if (const Result ra = ::ams::svc::DebugActiveProcess(std::addressof(dbg), vpid); R_FAILED(ra)) {
                LogLine("   live-ui: attach to vi rc=0x%x", ra.GetValue());
                os::SleepThread(TimeSpan::FromSeconds(5));
                return false;
            }
            u32 nev = 0;
            const Result rr = DbgResume(dbg, std::addressof(nev));
            const u64 held_us = (armTicksToNs(armGetSystemTick()) - f0) / 1000;
            if (R_FAILED(rr)) { DbgDetach(dbg); os::SleepThread(TimeSpan::FromSeconds(2)); return false; }
            DebugPumpStart(dbg);
            ++sessions;
            g_live_sessions.store(sessions, std::memory_order_relaxed);
            LogLine("   live[%u]: the HOME menu / applets - attached to vi (pid %llu), held %llu us, %u attach events", sessions,
                    static_cast<unsigned long long>(vpid), static_cast<unsigned long long>(held_us), nev);
            g_game_presented_in_ui.store(false, std::memory_order_relaxed);
            g_session_surface = std::addressof(g_game_surface);   /* ug's generation is the game's */
            g_ui_session_active.store(true, std::memory_order_relaxed);
            StreamEnd end = StreamEnd::Failed;
            const bool ui_started = UiSourceStart(dbg, vpid, base);
            if (!ui_started && UiSourceIdle()) {
                /* v0.7.16: nothing drawing (a game's loading pause) - let go of
                 * vi at once and wait for the game, no 5 s pause */
                g_ui_session_active.store(false, std::memory_order_relaxed);
                DebugPumpStop();
                DbgDetach(dbg);
                return false;
            }
            if (ui_started) {
                g_reconfig_request.store(false, std::memory_order_relaxed);
                end = g_uvc_raw ? TryUvcRawStream(dbg, base, vfd, nvmap_fd, cmd_handle, syncpt, cfg_addr, std::addressof(ug))
                                : TryAmbilightStream(dbg, base, vfd, nvmap_fd, cmd_handle, syncpt, cfg_addr, std::addressof(ug));
            }
            UiSourceStop();
            g_ui_session_active.store(false, std::memory_order_relaxed);
            DebugPumpStop();
            DbgDetach(dbg);
            UiSourceLogStats("live-ui");
            g_live_fps_x10.store(0, std::memory_order_relaxed);
            g_live_game_fps_x10.store(0, std::memory_order_relaxed);
            static const char *const names[] = { "done", "a game is on screen", "the viewer went away", "NVENC stalled", "failed",
                                                 "restarting (new settings or a game's new buffers)", "turned off, or an applet needs the memory",
                                                 "a 1080p picture - setting up again on the full memory loan" };
            LogLine("   live[%u]: HOME-menu session ended - %s; detached from vi", sessions, names[static_cast<int>(end)]);
            if (end == StreamEnd::Failed) { os::SleepThread(TimeSpan::FromSeconds(5)); }
            return true;
        }

        /* v0.7.16: a game rebuilding its graphics (Kingdom Hearts II's
         * native build, switched docked/handheld from an overlay: four new
         * buffer sets in 30 s, then it was gone and the screen black) is not
         * attached to again in the middle of it - every attach stops the game
         * for a moment, and while it rebuilds it starts and ends threads, each
         * one an event it waits on us for (33 in one session, up to 7.8 ms).
         * Wait until its buffers stay the same and it presents again: 1 s,
         * or 3 s when it rebuilt three times within 20 s; at most 8 s. */
        void SettleAfterRebuild() {
            static constinit u64 s_rebuild_ns[3] = {};
            static constinit u32 s_rebuild_i = 0;
            const u64 now = armTicksToNs(armGetSystemTick());
            s_rebuild_ns[s_rebuild_i++ % 3] = now;
            const u64 oldest = s_rebuild_ns[s_rebuild_i % 3];
            const bool churning = s_rebuild_i >= 3 && now - oldest < UINT64_C(20000000000);
            const u64 quiet_ns = churning ? UINT64_C(3000000000) : UINT64_C(1000000000);
            const u64 pid = g_app_pid.load(std::memory_order_relaxed);
            u32 gen = g_game_surface.generation;
            u64 t_gen = now;
            for (;;) {
                os::SleepThread(TimeSpan::FromMilliSeconds(100));
                ViewerKeepAlive();   /* v0.7.17: the viewer hears from us meanwhile */
                const u64 t = armTicksToNs(armGetSystemTick());
                if (g_app_pid.load(std::memory_order_relaxed) != pid) { return; }   /* the game went away */
                if (g_game_surface.generation != gen) { gen = g_game_surface.generation; t_gen = t; }
                const u64 last_present = armTicksToNs(g_app_present_tick.load(std::memory_order_relaxed));
                const bool presenting = last_present != 0 && t > last_present && t - last_present < UINT64_C(200000000);
                if (presenting && t - t_gen >= quiet_ns) { break; }
                if (t - now >= UINT64_C(8000000000)) { break; }
            }
            const u64 waited = armTicksToNs(armGetSystemTick()) - now;
            if (churning || waited > UINT64_C(1500000000)) {
                LogLine("   live: the game %s its frame buffers - waited %llu ms for it to settle before attaching again",
                        churning ? "keeps rebuilding" : "rebuilt", static_cast<unsigned long long>(waited / 1000000));
            }
        }

        /* v0.7.19: an app that closes itself within a few seconds of the stream
         * attaching - TiCo's protected builds do (anti-debug, #31: its 33
         * threads gone 2.6 s after the attach, no fault) - goes on the
         * never-attach list, as "Stream this app" off in the overlay does, so
         * its next start runs untouched. Not the Homebrew Menu over a game
         * (hbl runs under the game's program id, which would then be skipped)
         * nor the Album's homebrew. */
        constexpr u64 AutoSkipNs = UINT64_C(6000000000);
        struct AttachRecord { u64 tid; u64 ns; bool candidate; };

        AttachRecord NoteAttach(u64 pid, bool applet_target) {
            AttachRecord r = { g_app_tid.load(std::memory_order_relaxed), armTicksToNs(armGetSystemTick()), false };
            if (applet_target || r.tid == 0 || g_pmdmnt_rc != 0) { return r; }
            os::NativeHandle h = os::InvalidNativeHandle;
            ncm::ProgramLocation loc = {};
            cfg::OverrideStatus st = {};
            if (R_SUCCEEDED(::ams::pm::dmnt::AtmosphereGetProcessInfo(std::addressof(h), std::addressof(loc), std::addressof(st), os::ProcessId{pid}))) {
                os::CloseNativeHandle(h);
                r.candidate = !st.IsHbl() && loc.program_id.value == r.tid;
            }
            return r;
        }

        void AutoSkipIfClosedAtOnce(const AttachRecord &a) {
            const u64 ms = (armTicksToNs(armGetSystemTick()) - a.ns) / UINT64_C(1000000);
            if (!a.candidate || ms * UINT64_C(1000000) > AutoSkipNs) { return; }
            SetExcluded(a.tid, true);
            LogLine("   live: %016llx closed itself %llu ms after the stream attached (anti-debug, as TiCo) - skipped from now on; \"Stream this app\" in the overlay brings it back",
                    static_cast<unsigned long long>(a.tid), static_cast<unsigned long long>(ms));
            NotifyShot("This app closed when streamed - it is skipped from now on");
        }

        void RunLive(u32 vfd, u32 nvmap_fd, u32 cmd_handle, u32 syncpt, u32 cfg_addr) {
            LogLine("   ==== LIVE MODE (M86): streaming whenever a viewer is reading and a game is running ====");
            /* M98: a release install captures only on the firmware it was
             * tested on (22.x; v0.7.8: 21.x too, a user's 21.0.1; v0.7.11:
             * 23.x, a user's 23.0.0); the debug reads and engine jobs assume it */
            {
                const u32 v = hosversionGet();
                if (g_release_mode && !g_anyfw_armed && (HOSVER_MAJOR(v) < 21 || HOSVER_MAJOR(v) > 23)) {
                    g_live_state.store(LiveState_Unsupported, std::memory_order_relaxed);
                    LogLine("   live: firmware %u.%u.%u is untested (this build: 21.x-23.x) - not streaming. allow_untested_firmware = 1 in config.ini overrides.",
                            HOSVER_MAJOR(v), HOSVER_MINOR(v), HOSVER_MICRO(v));
                    VicStage("live:untested_fw");
                    SetViewerReason(ViewerReason_Unsupported);
                    for (;;) { static_cast<void>(UsbViewerPresent(300)); os::SleepThread(TimeSpan::FromSeconds(2)); }
                }
            }
            u64 skip_pid = 0;          /* a game whose swapchain was not found: wait for another */
            bool skip_big = false;     /* v0.7.19: ...or that draws above 1080p (#48) */
            u32 skip_gen = 0;          /* v0.7.17: ...or for its next buffer set (PokeMMO: a splash first, its game buffers 7 s later) */
            bool said_wait_viewer = false, said_wait_game = false;
            u64 hidden_since_ns = 0;   /* app alive but not presenting - load vs HOME */
            bool said_hidden_wait = false;
            u32 sessions = 0;
            bool said_off = false;
            /* v0.7.10: with no viewer for this long, close the engines and give
             * the donor's memory back (RunVicBlit tears down and sets up again
             * for the next viewer) - the browser applets need that pool */
            constexpr u64 IdleTeardownNs = 15'000'000'000ull;
            u64 idle_since = 0;
            auto idle_long = [&]() -> bool {
                const u64 now = armTicksToNs(armGetSystemTick());
                if (idle_since == 0) { idle_since = now; }
                return g_donor_mode && now - idle_since >= IdleTeardownNs && g_ind_buf == nullptr && g_capture_handles == 0;
            };
            for (;;) {
                if (g_engine_wedged) {
                    g_live_state.store(LiveState_Wedged, std::memory_order_relaxed);
                    VicStage("live:engine_wedged");
                    LogLine("   live: the engine is wedged - no more streaming this boot");
                    SetViewerReason(ViewerReason_Wedged);
                    for (;;) { static_cast<void>(UsbViewerPresent(300)); os::SleepThread(TimeSpan::FromSeconds(2)); }
                }
                /* v0.7.10: an applet is starting - give the donor's memory back now */
                if (g_applet_needs_memory.load(std::memory_order_relaxed) && g_donor_mode) {
                    CaptureRelease("live");
                    if (g_ind_buf == nullptr && g_capture_handles == 0) { LogLine("   live: an applet needs the memory - closing the engines now"); return; }
                }
                /* v0.7.18: the full donor for a 1080p picture (DonorCanGrow) */
                if (g_donor_regrow.load(std::memory_order_relaxed) && g_donor_mode) {
                    CaptureRelease("live");
                    if (g_ind_buf == nullptr && g_capture_handles == 0) {
                        g_donor_regrow.store(false, std::memory_order_relaxed);
                        LogLine("   live: setting up again on the full memory loan - closing the engines now");
                        return;
                    }
                }
                UsbFollowStreamSwitch();
                /* M99: a screenshot while not streaming: attach just for it */
                if (TakeShotRequest()) { static_cast<void>(ShotStandalone()); }
                /* M97: switched off from the overlay - not even a USB probe */
                if (!g_stream_enabled.load(std::memory_order_relaxed)) {
                    g_live_state.store(LiveState_Off, std::memory_order_relaxed);
                    if (!said_off) { LogLine("   live: turned off (overlay)"); VicStage("live:off"); said_off = true; }
                    SetViewerReason(ViewerReason_Off);
                    CaptureRelease("live");
                    if (idle_long()) { LogLine("   live: off for 15 s - closing the engines, memory back to the system"); return; }
                    os::SleepThread(TimeSpan::FromMilliSeconds(250));
                    continue;
                }
                if (said_off) { LogLine("   live: turned on (overlay)"); said_off = false; said_wait_viewer = said_wait_game = false; }
                if ((!g_ambi_armed && !UsbViewerPresent(300)) || (g_ambi_armed && NetIp() == 0)) {
                    g_live_state.store(LiveState_WaitViewer, std::memory_order_relaxed);
                    if (!said_wait_viewer) { LogLine("   live: waiting for a viewer (raw-view) to read the stream"); VicStage("live:no_viewer"); said_wait_viewer = true; }
                    CaptureRelease("live");
                    if (idle_long()) { LogLine("   live: no viewer for 15 s - closing the engines, memory back to the system"); return; }
                    os::SleepThread(TimeSpan::FromMilliSeconds(700));
                    continue;
                }
                said_wait_viewer = false;
                idle_since = 0;

                ::ams::os::ProcessId pid{};
                const u32 q_before = g_queue_count.load(std::memory_order_relaxed);
                const bool have_app = g_pmdmnt_rc == 0 && R_SUCCEEDED(::ams::pm::dmnt::GetApplicationProcessId(std::addressof(pid)));
                /* v0.7.14: the Album / hbmenu draws with buffers of its own, like a game */
                const u64 applet_pid = g_ui_stream_armed ? g_applet_target_pid.load(std::memory_order_relaxed) : 0;
                if (skip_pid != 0 && have_app && pid.value == skip_pid && g_game_surface.generation != skip_gen) {
                    LogLine("   live: the game made new frame buffers - looking for its swapchain again");
                    skip_pid = 0;
                    said_wait_game = false;
                }
                if ((have_app && pid.value != skip_pid) || applet_pid != 0) {
                    /* presenting? a suspended or loading-from-boot game is not */
                    os::SleepThread(TimeSpan::FromMilliSeconds(200));
                }
                bool presenting = g_queue_count.load(std::memory_order_relaxed) != q_before;
                /* v0.7.14: whose frames they were - the Album in front of a
                 * suspended game presents, the game does not */
                bool applet_target = false;
                if (presenting && applet_pid != 0) {
                    const u64 presenter = g_present_pid.load(std::memory_order_relaxed);
                    if (presenter == applet_pid && (!have_app || presenter != pid.value)) {
                        pid.value = applet_pid;
                        applet_target = true;
                    } else if (!have_app || presenter != pid.value) {
                        presenting = false;
                    }
                }
                const bool have_target = have_app || applet_target;
                /* v0.1.1b: an app on the never-attach list (anti-debug builds
                 * such as TiCo refuse to run under a debugger) */
                if (have_app && !applet_target && g_app_excluded.load(std::memory_order_relaxed)) {
                    g_live_state.store(LiveState_Excluded, std::memory_order_relaxed);
                    SetViewerReason(ViewerReason_Excluded);
                    if (!said_wait_game) { LogLine("   live: %016llx is excluded - not attaching", static_cast<unsigned long long>(g_app_tid.load())); VicStage("live:excluded"); said_wait_game = true; }
                    CaptureRelease("live");
                    os::SleepThread(TimeSpan::FromMilliSeconds(500));
                    continue;
                }
                if (!have_target || pid.value == skip_pid || !presenting) {
                    /* v0.7.14: the HOME menu or an applet instead (home_menu = 1) */
                    /* v0.7.17: a game that is loading - vi was just looked at and
                     * nothing of the HOME menu drew: not again for a second */
                    static u64 s_ui_look_ns = 0;
                    const u64 look_now = armTicksToNs(armGetSystemTick());
                    const bool look = !have_app || !UiSourceIdle() || look_now - s_ui_look_ns >= UINT64_C(1000000000);
                    if (g_ui_stream_armed && look) {
                        s_ui_look_ns = look_now;
                        if (RunUiSession(vfd, nvmap_fd, cmd_handle, syncpt, cfg_addr, sessions)) {
                            said_wait_game = false;
                            continue;
                        }
                        /* home_menu on but UI could not start: free now on confirm
                         * so SwitchU / Album is not racing a mapped donor. */
                        if (g_donor_mode && g_detach_on_home) {
                            const char *ui_why = nullptr;
                            if (HomeDetachShouldFire(50, g_home_detach_ms, g_home_fallback_ms, std::addressof(ui_why))) {
                                PrepareForHomeMenu();
                                CaptureRelease("live");
                                if (g_ind_buf == nullptr && g_capture_handles == 0) {
                                    LogLine("   live: home_menu UI unavailable (%s) - closing the engines, memory back for HOME/Album",
                                            ui_why != nullptr ? ui_why : "?");
                                    return;
                                }
                            }
                        }
                    }
                    g_live_state.store(LiveState_WaitGame, std::memory_order_relaxed);
                    SetViewerReason(!have_app ? ViewerReason_NoGame : (pid.value == skip_pid ? (skip_big ? ViewerReason_TooBig : ViewerReason_NotFound) : ViewerReason_GameHidden));
                    /* App alive but not presenting: often a load screen, not HOME.
                     * Tear the donor down when HomeDetachShouldFire confirms
                     * (HOME button / Album / applet) or the fallback stall elapses.
                     * No game / web menu over the game still frees immediately.
                     * v0.7.17: loading with home_menu idle keeps engines through
                     * title-list handoffs (Duke Nukem, Super Mario 3D All-Stars). */
                    const bool web_menu = have_app && WebMenuUp();
                    const bool loading = have_app && g_ui_stream_armed && UiSourceIdle();
                    const bool app_hidden = have_app && pid.value != skip_pid && !presenting;
                    if (g_donor_mode && g_detach_on_home && app_hidden && !loading) {
                        const u64 now_ns = armTicksToNs(armGetSystemTick());
                        if (hidden_since_ns == 0) { hidden_since_ns = now_ns; said_hidden_wait = false; }
                        const u64 age_ms = (now_ns - hidden_since_ns) / UINT64_C(1000000);
                        const char *home_why = nullptr;
                        if (HomeDetachShouldFire(age_ms, g_home_detach_ms, g_home_fallback_ms, std::addressof(home_why))) {
                            PrepareForHomeMenu();
                            CaptureRelease("live");
                            if (g_ind_buf == nullptr && g_capture_handles == 0) {
                                LogLine("   live: home confirmed (%s) after %llu ms not presenting - closing the engines, memory back to the system",
                                        home_why != nullptr ? home_why : "?", static_cast<unsigned long long>(age_ms));
                                return;
                            }
                        } else {
                            if (!said_hidden_wait) {
                                LogLine("   live: game not presenting (%llu ms) - holding capture through load gaps (detach needs HOME button / Album / applet or %u ms fallback)",
                                        static_cast<unsigned long long>(age_ms), g_home_fallback_ms);
                                VicStage("live:no_game");
                                said_hidden_wait = true;
                                said_wait_game = true;
                            }
                            os::SleepThread(TimeSpan::FromMilliSeconds(200));
                            continue;
                        }
                    }
                    if (g_donor_mode && g_detach_on_home && pid.value != skip_pid && (!have_app || web_menu)) {
                        CaptureRelease("live");
                        if (g_ind_buf == nullptr && g_capture_handles == 0) {
                            LogLine("   live: %s - closing the engines, memory back to the system",
                                    !have_app ? "no game" : "a web applet's menu is over the game");
                            return;
                        }
                    }
                    hidden_since_ns = 0;
                    said_hidden_wait = false;
                    if (!said_wait_game) {
                        LogLine("   live: viewer present; waiting for a game (%s)",
                                !have_app ? "no application" : (pid.value == skip_pid ? (skip_big ? "this game draws above 1080p" : "this game's swapchain was not found") : "not presenting"));
                        VicStage("live:no_game");
                        said_wait_game = true;
                    }
                    CaptureRelease("live");
                    /* v0.7.17: a game that will draw again is looked for sooner
                     * (plus the 200 ms present check above) */
                    os::SleepThread(TimeSpan::FromMilliSeconds(have_app ? 100 : 800));
                    continue;
                }
                said_wait_game = false;
                hidden_since_ns = 0;
                said_hidden_wait = false;

                /* v0.7.10: the capture region only now - a viewer reads and a
                 * game presents. Short of memory (an applet such as the web
                 * browser holds the Applet pool): wait and try again rather
                 * than take what the applet needs. */
                if (!CaptureAcquire("live")) {
                    static u32 short_n = 0;
                    if ((short_n++ % 15) == 0) { LogLine("   live: not enough memory free to stream right now - trying again"); }
                    VicStage("live:no_memory");
                    SetViewerReason(ViewerReason_NoMemory);
                    os::SleepThread(TimeSpan::FromSeconds(2));
                    continue;
                }

                SetViewerReason(ViewerReason_Starting);
                ++sessions;
                g_live_sessions.store(sessions, std::memory_order_relaxed);
                ::ams::svc::Handle dbg = ::ams::svc::InvalidHandle;
                if (!DmAttach(pid.value, std::addressof(dbg))) {
                    LogLine("   live[%u]: no attach to pid %llu (direct failed, dmnt not holding it) - retrying in 5 s", sessions,
                            static_cast<unsigned long long>(pid.value));
                    os::SleepThread(TimeSpan::FromSeconds(5));
                    continue;
                }
                /* M87: this game's geometry, as its setPreallocatedBuffer
                 * parcels described it; a consistent copy (the binder thread
                 * writes it) */
                GameSurface geo;
                const GameSurface &src_surface = applet_target ? g_applet_surface : g_game_surface;
                g_session_surface = std::addressof(src_surface);
                for (u32 tries = 0; tries < 10; ++tries) {
                    const u32 gen = src_surface.generation;
                    geo = src_surface;
                    if (gen == src_surface.generation) { break; }
                }
                if (applet_target) { LogLine("   live[%u]: the Album / hbmenu is in front (pid %llu)", sessions, static_cast<unsigned long long>(pid.value)); }
                u64 slot_base = 0;
                const u64 f0 = armTicksToNs(armGetSystemTick());
                const bool found = LocateSwapchain(dbg, geo, std::addressof(slot_base), "live");
                u64 want = FbSwapSize;
                if (geo.num_slots != 0 && geo.buf_size != 0) {
                    u32 top = 0;
                    for (u32 k = 0; k < geo.num_slots && k < 8; ++k) { if (geo.slot_offset[k] > top) { top = geo.slot_offset[k]; } }
                    want = static_cast<u64>(top) + geo.buf_size;
                }
                /* A direct attach stopped the game: drain the attach burst and
                 * continue. Through dmnt:cht the game never stopped (dmnt pumps
                 * its own handle), so DbgResume is a no-op there. */
                u32 nev = 0;
                const Result rc = DbgResume(dbg, std::addressof(nev));
                const u64 frozen_us = (armTicksToNs(armGetSystemTick()) - f0) / 1000;
                LogLine("   live[%u]: attached to pid %llu; %ux%u, %u slot(s); swapchain (%llu B) %s%010llx; game held %llu us; %u attach events, continue rc=0x%x",
                        sessions, static_cast<unsigned long long>(pid.value), geo.width, geo.height, geo.num_slots,
                        static_cast<unsigned long long>(want), found ? "at 0x" : "NOT FOUND ",
                        static_cast<unsigned long long>(slot_base), static_cast<unsigned long long>(frozen_us), nev, rc.GetValue());
                if (R_FAILED(rc)) {
                    DbgDetach(dbg);
                    os::SleepThread(TimeSpan::FromSeconds(2));
                    continue;
                }
                if (!found) {
                    DbgDetach(dbg);
                    skip_pid = pid.value;
                    skip_gen = geo.generation;
                    skip_big = false;
                    continue;
                }
                /* v0.7.19: above 1080p (a 2560x1440 mod, #48) the frame does not fit
                 * the capture region: say so once and wait for new buffers, instead
                 * of attaching and failing every 5 s */
                if (geo.width > 1920 || geo.height > 1088) {
                    DbgDetach(dbg);
                    LogLine("   live: this game draws at %ux%u - above 1080p, not streamed (until it makes new frame buffers)", geo.width, geo.height);
                    skip_pid = pid.value;
                    skip_gen = geo.generation;
                    skip_big = true;
                    SetViewerReason(ViewerReason_TooBig);
                    continue;
                }
                if (!DbgIsDmnt(dbg)) { DebugPumpStart(dbg); }
                g_reconfig_request.store(false, std::memory_order_relaxed);   /* this session starts with the current settings */
                const GameSurface *gp = (geo.num_slots != 0 && geo.buf_size != 0) ? std::addressof(geo) : nullptr;
                const AttachRecord attached = NoteAttach(pid.value, applet_target);   /* v0.7.19: auto-skip */
                const StreamEnd end = g_uvc_raw ? TryUvcRawStream(dbg, slot_base, vfd, nvmap_fd, cmd_handle, syncpt, cfg_addr, gp)
                                                : TryAmbilightStream(dbg, slot_base, vfd, nvmap_fd, cmd_handle, syncpt, cfg_addr, gp);
                const bool target_exited = !DbgIsDmnt(dbg) && DebugPumpTargetGone();
                if (!DbgIsDmnt(dbg)) { DebugPumpStop(); }
                DbgDetach(dbg);
                g_live_fps_x10.store(0, std::memory_order_relaxed);
                g_live_game_fps_x10.store(0, std::memory_order_relaxed);
                static const char *const names[] = { "done", "the game went away", "the viewer went away", "NVENC stalled", "failed",
                                                     "the picture size changed - restarting at the new size", "turned off from the overlay",
                                                     "a 1080p picture - setting up again on the full memory loan" };
                LogLine("   live[%u]: session ended - %s; detached", sessions, names[static_cast<int>(end)]);
                if (end == StreamEnd::GameGone && target_exited) { AutoSkipIfClosedAtOnce(attached); }
                /* Viewer closed while capturing: CaptureRelease alone leaves the
                 * donor mapped. A later HOME into SwitchU Album then races that
                 * ~42 MB. Free it now when home_menu is not about to reuse it. */
                if (end == StreamEnd::ViewerGone && g_donor_mode && !g_ui_stream_armed) {
                    PrepareForHomeMenu();
                    CaptureRelease("live");
                    if (g_ind_buf == nullptr && g_capture_handles == 0) {
                        LogLine("   live: viewer closed - closing the engines, memory back for HOME/Album");
                        return;
                    }
                }
                if (end == StreamEnd::Failed) { os::SleepThread(TimeSpan::FromSeconds(5)); }
                /* v0.7.14: with the HOME menu streaming, its session follows a
                 * game's at once - the 1 s pause was most of the 1.35 s the
                 * viewer stood still when a game was quit (ovl3d) */
                else if (end == StreamEnd::GameGone && g_ui_stream_armed) { os::SleepThread(TimeSpan::FromMilliSeconds(300)); }
                else if (end != StreamEnd::Reconfigure && end != StreamEnd::Disabled && end != StreamEnd::Regrow) { os::SleepThread(TimeSpan::FromSeconds(1)); }
                else if (end == StreamEnd::Reconfigure) {
                    os::SleepThread(TimeSpan::FromMilliSeconds(300));   /* a new buffer set registers slot by slot */
                    SettleAfterRebuild();
                }
            }
        }

        void TryDebugCapture(u32 vfd, u32 nvmap_fd, u32 cmd_handle, u32 syncpt, u32 cfg_addr, u32 dst_addr) {
            if (!g_dbg_armed) { return; }
            if (g_live_armed) { RunLive(vfd, nvmap_fd, cmd_handle, syncpt, cfg_addr); return; }

            VicStage("dbg:1_find_pid");
            if (g_pmdmnt_rc != 0) {
                LogLine("   pmdmntInitialize() failed at boot (rc=0x%x) - skipping", g_pmdmnt_rc);
                VicStage("dbg:1_no_pmdmnt");
                return;
            }
            ::ams::os::ProcessId pid{};
            {
                const Result r = ::ams::pm::dmnt::GetApplicationProcessId(std::addressof(pid));
                LogLine("   pm:dmnt GetApplicationProcessId rc=0x%x -> pid=%llu",
                        r.GetValue(), static_cast<unsigned long long>(pid.value));
                if (R_FAILED(r)) { VicStage("dbg:1_FAILED"); return; }
            }

            /* ---- attached from here. The game is STOPPED until we Continue. -- */
            RegionHit   big[8]  = {};   u32 nbig  = 0;
            FbCandidate cand    = {};   bool found = false, by_size = false;
            u32  steps = 0, reads = 0, budget = 24000, nexact = 0;
            bool walk_complete = false;

            u32  nev = 0;
            ::ams::Result r_attach{}, r_cont = ::ams::svc::ResultInvalidHandle();
            bool attached = false, resumed = false;

            u64  strip_ns = 0, dump_ns = 0, full_ns = 0;
            u32  strip_chunks_ok = 0, dump_chunks = 0, full_strips = 0;
            u32  cap_done = 0, cap_missed = 0, cap_distinct = 0;
            u64  cap_min = ~UINT64_C(0), cap_max = 0, cap_sum = 0, cap_ns = 0;
            u32  fps_before = 0, fps_during = 0, fps_after = 0;
            u32  cap_over = 0, slot_hits[4] = {};
            u32  strip_own_rc = 0, strip_pin = 0;
            u64  frozen_ns = 0;
            bool strip_read_ok = false, strip_blit_ok = false, strip_dumped = false, vic_dumped = false;
            u32  strip_ok_count = 0, strip_dump_count = 0, variant_sum[8] = {};
            bool dumped = false;
            u32  nonzero = 0, distinct = 0, lane_ff[4] = {}, total_px = 0;
            u8   live_a[16] = {}, live_b[16] = {};
            bool live_ok = false, live_changed = false;

            ::ams::svc::Handle dbg = ::ams::svc::InvalidHandle;
            attached = DmAttach(pid.value, std::addressof(dbg));
            if (attached) { r_attach = ResultSuccess(); } else { r_attach = ::ams::svc::ResultInvalidHandle(); }
            if (attached) {

                u64 addr = 0;
                for (; steps < 4000; ++steps) {
                    ::ams::svc::MemoryInfo mi = {};
                    ::ams::svc::PageInfo   pi = {};
                    if (!DbgQuery(dbg, std::addressof(mi), std::addressof(pi), addr)) { walk_complete = true; break; }
                    if (mi.size == 0) { walk_complete = true; break; }

                    const u32  attr = static_cast<u32>(mi.attribute);
                    const bool dev  = (attr & ::ams::svc::MemoryAttribute_DeviceShared) != 0 || mi.device_count > 0;

                    if (dev && mi.size >= FbSwapSize && nbig < 8) {
                        const RegionHit r = { mi.base_address, mi.size, static_cast<u32>(mi.state),
                                              static_cast<u32>(mi.permission), attr, mi.device_count };
                        if (mi.size == FbSwapSize) {
                            for (u32 k = nbig; k > nexact; --k) { big[k] = big[k - 1]; }
                            big[nexact++] = r;
                        } else {
                            big[nbig] = r;
                        }
                        ++nbig;
                    }
                    const u64 next = mi.base_address + mi.size;
                    if (next <= addr) { walk_complete = true; break; }
                    addr = next;
                }

                /* A device-shared region of EXACTLY three slots is the swapchain:
                 * that is nvmap 1268's size, and it held at offset 0 on both runs
                 * that found it. Take it without demanding a pixel pattern - the
                 * corner can legitimately be flat white, which is what confused
                 * M36's lane detection. The signature scan stays as the fallback
                 * for a console whose layout does not produce an exact region. */
                u8 s[3][64];
                if (nexact > 0) {
                    cand.addr        = big[0].base;
                    cand.region_base = big[0].base;
                    cand.lane        = 3;                  /* A8B8G8R8 -> R,G,B,A */
                    found            = true;
                    by_size          = true;
                    for (u32 k = 0; k < 3; ++k) {
                        ++reads;
                        if (DbgRead(dbg, s[k], cand.addr + FbSlotOff[k], 64)) {
                            std::memcpy(cand.head[k], s[k], 16);
                        }
                    }
                }
                for (u32 r = 0; r < nbig && !found && budget > 0; ++r) {
                    const u64 last = big[r].size - FbSwapSize;
                    for (u64 off = 0; off <= last && !found && budget > 0; off += 0x1000) {
                        --budget; ++reads;
                        if (!DbgRead(dbg, s[0], big[r].base + off, 64)) { continue; }
                        u32 varied = 0;
                        const s32 lane = FbPixelLane(s[0], std::addressof(varied));
                        if (lane < 0) { continue; }

                        bool all_slots = true;
                        for (u32 k = 1; k < 3; ++k) {
                            --budget; ++reads;
                            if (!DbgRead(dbg, s[k], big[r].base + off + FbSlotOff[k], 64)
                                || FbPixelLane(s[k], nullptr) != lane) { all_slots = false; break; }
                        }
                        if (!all_slots) { continue; }

                        cand.addr        = big[r].base + off;
                        cand.region_base = big[r].base;
                        cand.lane        = static_cast<u32>(lane);
                        cand.varied      = varied;
                        for (u32 k = 0; k < 3; ++k) { std::memcpy(cand.head[k], s[k], 16); }
                        found = true;
                    }
                }

                /* Resuming is now done as early as possible, so it is a guarded
                 * lambda rather than a fixed point in the sequence. */
                const u64 t_frozen0 = armTicksToNs(armGetSystemTick());
                auto resume_game = [&]() {
                    if (resumed) { return; }
                    r_cont = DbgResume(dbg, std::addressof(nev));
                    resumed = R_SUCCEEDED(r_cont);
                    frozen_ns = armTicksToNs(armGetSystemTick()) - t_frozen0;
                    /* M84: from here on, every later thread start/exit in the
                     * game raises an event that stops it until continued. Run I
                     * froze at a loading screen for want of this. No pump on a
                     * dmnt session: dmnt already pumps its own handle. */
                    if (resumed && !DbgIsDmnt(dbg)) { DebugPumpStart(dbg); }
                };

                /* ---- THE VIC, ON REAL GAME PIXELS -------------------------
                 * Everything here happens while the game is stopped, so the raw
                 * strip and the VIC's output are the SAME pixels and can be
                 * compared byte-for-byte on the PC. If they came from different
                 * moments, any mismatch would be ambiguous between "wrong VIC
                 * config" and "the frame moved", which is untestable.
                 *
                 * The capture buffer is nvmap'd CACHEABLE: ReadDebugProcessMemory
                 * writes 983,040 B into it and an uncached destination would cost
                 * far more than the explicit flush below. */
                /* M82: an nvframe run skips the older one-shot steps (strip dump,
                 * 120-frame read loop); its own loop measures what they did */
                if (found && !g_stream_armed && !g_nvframe_armed && !g_nvstream_armed && !g_nvp_armed && !g_nv1080_armed && g_ind_buf != nullptr && g_ind_size >= FbBlockRow && cfg_addr != 0 && dst_addr != 0) {
                    VicStage("vs:1_own_capture_buf");
                    u32 cap_handle = 0, cap_id = 0, cap_addr = 0;
                    const auto orc = NvmapOwn(nvmap_fd, g_ind_buf, static_cast<u32>(FbBlockRow), 0,
                                              std::addressof(cap_handle), std::addressof(cap_id), true);
                    if (R_SUCCEEDED(orc)) {
                        VicStage("vs:2_pin");
                        MapCmdBuffer(vfd, cap_handle, std::addressof(cap_addr), "capture", 0);
                        if (cap_addr != 0) {
                            VicStage("vs:3_read_block_row");
                            strip_read_ok = DbgRead(dbg, g_ind_buf, cand.addr, FbBlockRow);
                            if (strip_read_ok) {
                                /* the engine reads this through the SMMU; our
                                 * cached writes must reach memory first */
                                armDCacheFlush(g_ind_buf, FbBlockRow);

                                /* THE STUTTER FIX. Only the 983 KB read above needs
                                 * the game stopped - everything below works on our
                                 * own buffer. M45 kept the game frozen for 4.78 s
                                 * (181.195 -> 185.971) doing blits and 1.3 MB of SD
                                 * writes with the target halted, which is the
                                 * stutter felt on the console. Resume here. */
                                resume_game();

                                VicStage("vs:4_dump_strip");
                                strip_dumped = WriteBufToSd(StripBinPath, g_ind_buf, FbBlockRow);

                                const JobCtx sctx{ vfd, cmd_handle, syncpt, cfg_addr, dst_addr,
                                                   cap_addr, 0, 0, 0, 0, SelfSrc };
                                for (u32 v = 0; v < StripVariantCount; ++v) {
                                    const StripVariant &sv = StripVariants[v];
                                    /* layout fixed at what M43 proved correct;
                                     * only the source pixel format varies */
                                    g_strip_src = SrcDesc{ StripW, StripH, StripW,
                                                           vic::BLK_KIND_GENERIC_16Bx2, 4,
                                                           sv.src_fmt, vic::CACHE_WIDTH_64Bx4,
                                                           sv.rect_w, sv.rect_h };
                                    g_strip_out = OutDesc{ StripOut.w, StripOut.h,
                                                           StripOut.stride_px, sv.out_fmt };
                                    char stage[48];
                                    std::snprintf(stage, sizeof(stage), "vb:strip_%s", sv.name);
                                    VicStage("vs:5_sweep");
                                    if (RunOneJob(stage, VicJob::BlitStrip, true, sctx)) { ++strip_ok_count; }

                                    /* g_vic_dst_buf is UNCACHED nvmap memory, and handing that
                                     * straight to fs::WriteFile is what made every vic dump report
                                     * failure in M41 and M42 while strip.bin - which comes from the
                                     * CACHEABLE capture buffer - succeeded every time. The files it
                                     * left behind did not even match the engine's own checksum, so
                                     * they were never the VIC's output. Stage through normal cached
                                     * heap past the nvmap'd region first. */
                                    std::memcpy(g_stage_buf, g_vic_dst_buf, StripOutSize);
                                    variant_sum[v] = 0;
                                    for (u32 k = 0; k < StripOutSize; ++k) { variant_sum[v] += g_stage_buf[k]; }
                                    char path[64];
                                    std::snprintf(path, sizeof(path), "sdmc:/applet-mitm-vic-%s.bin", sv.name);
                                    if (WriteBufToSd(path, g_stage_buf, StripOutSize)) { ++strip_dump_count; }
                                }
                                strip_blit_ok = strip_ok_count > 0;
                                vic_dumped    = strip_dump_count > 0;
                            }
                            UnmapCmdBuffer(vfd, cap_handle);
                        }
                    }
                    strip_own_rc = orc;   /* ::Result is libnx's u32, not ams::Result */
                    strip_pin    = cap_addr;
                }

                /* Dump the frame BEFORE resuming. M37 continued the game first
                 * and then streamed 8.8 MB to the SD at 24 MB/s, so the game
                 * redrew that slot ~21 times across the 358 ms write. The decoded
                 * PNG showed it exactly: a monotonic brightness ramp, one step per
                 * 128-row block-row - mean luma 191, 223, 237, 249, 254, 251 - as
                 * MK8's fade-to-white advanced down the image in the same order we
                 * read it. Tearing, not a decode fault.
                 *
                 * With the game still stopped nothing can write to the slot, so the
                 * frame comes out coherent. It costs ~400 ms frozen, once, which is
                 * a fine trade for a screenshot. A streaming capture would instead
                 * read into RAM at 1129 MB/s and never go near the SD card. */
                if (found && g_dump_armed && g_ind_buf != nullptr && g_ind_size >= 0x10000) {
                    dumped = DumpSlotToSd(dbg, cand.addr, std::addressof(dump_ns), std::addressof(dump_chunks));
                }

                /* fallback: if the strip section was skipped, resume here */
                resume_game();

                /* ---- M63: VIC benchmark, instrumentation OFF ---------------
                 * The number M59 reported (119,023 us/blit) was dominated by
                 * its own diagnostics. This times the same engine with
                 * g_vic_quiet set, and then does it again with an NV12 output
                 * surface so we learn the cost of the format conversion that
                 * NVENC will need. Runs after resume_game(), so the game is
                 * live throughout and any starvation would be visible. */
                if (found && g_bench_armed && g_ind_buf != nullptr
                    && g_ind_size >= FbSlotSize && cfg_addr != 0 && dst_addr != 0) {
                    VicStage("bn:1_pin");
                    u32 bh = 0, bi = 0, ba = 0;
                    const auto brc = NvmapOwn(nvmap_fd, g_ind_buf, static_cast<u32>(FbSlotSize), 0,
                                              std::addressof(bh), std::addressof(bi), true);
                    if (R_FAILED(brc)) {
                        LogLine("   bench: NvmapOwn rc=0x%x", brc);
                    } else {
                        MapCmdBuffer(vfd, bh, std::addressof(ba), "bench-src", 0);
                        if (ba == 0) {
                            LogLine("   bench: pin returned 0 - refusing to submit");
                        } else {
                            VicStage("bn:2_read");
                            if (!DbgRead(dbg, g_ind_buf, cand.addr, FbSlotSize)) {
                                LogLine("   bench: slot read failed");
                            } else {
                                armDCacheFlush(g_ind_buf, FbSlotSize);

                                const SrcDesc full_src{ 1920, 1080, 1920,
                                                        vic::BLK_KIND_GENERIC_16Bx2, 4,
                                                        vic::PIXFMT_A8R8G8B8, vic::CACHE_WIDTH_64Bx4,
                                                        1920, 1080 };
                                const JobCtx bc{ vfd, cmd_handle, syncpt, cfg_addr, dst_addr, ba, 0, 0, 0, 0, SelfSrc };
                                constexpr u32 BenchW = 640, BenchH = 360, BenchN = 60;

                                struct BenchCase { const char *name; u32 fmt; };
                                const BenchCase cases[2] = {
                                    { "RGBA", vic::PIXFMT_A8B8G8R8 },
                                    { "NV12", vic::PIXFMT_Y8_U8V8_N420 },
                                };

                                for (u32 ci = 0; ci < 2; ++ci) {
                                    g_strip_src = full_src;
                                    g_strip_out = OutDesc{ BenchW, BenchH, BenchW, cases[ci].fmt };

                                    /* one warm-up outside the timed region, loud,
                                     * so a failure is diagnosable */
                                    g_vic_quiet = false;
                                    const bool warm = RunOneJob("bn:warmup", VicJob::BlitStrip, true, bc);
                                    LogLine("   bench %s: warm-up %s", cases[ci].name, warm ? "OK" : "FAILED");
                                    if (!warm) { continue; }

                                    g_vic_quiet    = true;
                                    g_vic_ns_total = 0; g_vic_ns_worst = 0; g_vic_jobs = 0; g_vic_timeouts = 0;
                                    u32 ok = 0;
                                    const u64 t0 = armTicksToNs(armGetSystemTick());
                                    for (u32 i = 0; i < BenchN; ++i) {
                                        if (RunOneJob("bn:job", VicJob::BlitStrip, true, bc)) { ++ok; }
                                    }
                                    const u64 wall = armTicksToNs(armGetSystemTick()) - t0;
                                    g_vic_quiet = false;

                                    const u32 n = (g_vic_jobs > 0) ? g_vic_jobs : 1;
                                    LogLine("   bench %s: %u timeouts (syncpt never reached the fence)",
                                            cases[ci].name, g_vic_timeouts);
                                    LogLine("   bench %s %ux%u: %u/%u jobs, submit+wait avg %llu us, worst %llu us",
                                            cases[ci].name, BenchW, BenchH, ok, BenchN,
                                            static_cast<unsigned long long>(g_vic_ns_total / 1000 / n),
                                            static_cast<unsigned long long>(g_vic_ns_worst / 1000));
                                    LogLine("   bench %s: wall %llu us for %u jobs = %llu us/frame  (60 fps budget 16667)",
                                            cases[ci].name,
                                            static_cast<unsigned long long>(wall / 1000), BenchN,
                                            static_cast<unsigned long long>(wall / 1000 / BenchN));
                                }

                                /* dump the NV12 result so correctness is checked
                                 * on the PC, not asserted here. g_vic_dst_buf is
                                 * UNCACHED nvmap - stage through cached memory
                                 * first, the M41/M42 lesson. */
                                {
                                    const size_t nv12_sz = static_cast<size_t>(BenchW) * BenchH * 3 / 2;
                                    if (g_stage_buf != nullptr && nv12_sz <= StreamStageSize) {
                                        std::memcpy(g_stage_buf, g_vic_dst_buf, nv12_sz);
                                        /* Does the engine write the UV plane at
                                         * all? Luma and chroma reported apart,
                                         * so "green image" stops being ambiguous
                                         * between a bad matrix and an unwritten
                                         * second plane. */
                                        const size_t ysz = static_cast<size_t>(BenchW) * BenchH;
                                        u8 ymin = 255, ymax = 0, cmin = 255, cmax = 0;
                                        for (size_t i = 0; i < ysz; ++i) {
                                            const u8 v = g_stage_buf[i];
                                            if (v < ymin) { ymin = v; }
                                            if (v > ymax) { ymax = v; }
                                        }
                                        for (size_t i = ysz; i < nv12_sz; ++i) {
                                            const u8 v = g_stage_buf[i];
                                            if (v < cmin) { cmin = v; }
                                            if (v > cmax) { cmax = v; }
                                        }
                                        LogLine("   bench: luma %u..%u   chroma %u..%u   %s",
                                                ymin, ymax, cmin, cmax,
                                                (cmin == cmax) ? "*** CHROMA PLANE NOT WRITTEN ***" : "chroma varies");
                                        const bool w = WriteBufToSd("sdmc:/applet-mitm-nv12.bin", g_stage_buf, nv12_sz);
                                        LogLine("   bench: nv12 dump %s (%zu B, %ux%u)",
                                                w ? "OK -> sdmc:/applet-mitm-nv12.bin" : "FAILED", nv12_sz, BenchW, BenchH);
                                    }
                                }
                            }
                            UnmapCmdBuffer(vfd, bh);
                        }
                    }
                    VicStage("bn:done");
                }

                /* ---- M67: stream, through the VIC -------------------------
                 * MUST STAY BELOW resume_game(): M59's first run put this above
                 * it, where DebugActiveProcess still has the game halted, and
                 * the loop waited for a frame that could not arrive.
                 *
                 * The engine is back. M60 used a CPU point-sampler because M59
                 * measured the VIC at 119 ms/blit; M63-M65 showed that figure
                 * was ~7 SD-flushing log calls plus a 65,536-iteration checksum
                 * inside the timed region. Measured quiet, it is 0.8 ms - and
                 * it emits 1.5 bytes/pixel instead of 4. */
                if (found && g_stream_armed && g_ind_buf != nullptr
                    && g_ind_size >= FbSlotSize && cfg_addr != 0 && dst_addr != 0) {
                    VicStage("st:1_pin");
                    u32 sh = 0, si = 0, sa = 0;
                    const auto srrc = NvmapOwn(nvmap_fd, g_ind_buf, static_cast<u32>(FbSlotSize), 0,
                                               std::addressof(sh), std::addressof(si), true);
                    if (R_FAILED(srrc)) {
                        LogLine("   stream: NvmapOwn(slot) rc=0x%x", srrc);
                    } else {
                        MapCmdBuffer(vfd, sh, std::addressof(sa), "stream-src", 0);
                        if (sa == 0) {
                            LogLine("   stream: pin returned 0 - refusing to submit");
                        } else {
                            VicStage("st:2_loop");
                            if (g_sweep_armed) {
                                /* M70: the whole resolution/fps curve in one
                                 * hardware cycle. M67 showed usb wait averaging
                                 * 120 us, i.e. each 497,664 B frame had already
                                 * landed during the preceding ~11.5 ms of CPU
                                 * work - so the link does >= 43 MB/s and the
                                 * 29.9 MB/s we were sending was our own ceiling,
                                 * not its. Measure where it actually runs out
                                 * instead of guessing one size per run. */
                                struct Step { u32 w, h; };
                                const Step steps[] = {
                                    {  768, 432 }, {  896, 504 }, {  960, 540 },
                                    { 1152, 648 }, { 1280, 720 },
                                };
                                for (u32 si = 0; si < sizeof(steps)/sizeof(steps[0]); ++si) {
                                    StreamFrames(dbg, cand.addr, vfd, cmd_handle, syncpt,
                                                 cfg_addr, dst_addr, sa,
                                                 steps[si].w, steps[si].h, 300);
                                }
                            } else {
                                /* M71: let the negotiated link choose the size.
                                 *
                                 * M70 measured the High-Speed wall at ~37 MB/s,
                                 * which puts 60 fps at ~800x450 and nothing
                                 * larger. If the BOS descriptor added this
                                 * build actually gets us SuperSpeed, that wall
                                 * moves by roughly an order of magnitude and
                                 * the right answer is completely different -
                                 * so ask, do not assume.
                                 *
                                 * 1600x900 rather than 1920x1080 on purpose:
                                 * StreamStageSize is 0x220000, which holds
                                 * 1,485,482 packed-420 pixels. 1600x900 is
                                 * 1,440,000 and fits; 1080p is 2,073,600 and
                                 * does not. Going native needs the staging
                                 * re-layout, and that is a change worth making
                                 * only once SuperSpeed is confirmed. */
                                const bool ss = UsbIsSuperSpeed();
                                u32 sW = g_stream_w, sH = g_stream_h;
                                if (g_stream_auto) {
                                    if (ss) { sW = 1600; sH = 900; }
                                    else    { sW =  800; sH = 450; }
                                }
                                LogLine("   stream: link=%s -> %ux%u (%u B/frame, %u MB/s at 60 fps)",
                                        ss ? "SUPERSPEED" : "high-speed", sW, sH,
                                        sW * sH * 3 / 2, (sW * sH * 3 / 2) * 60 / 1000000);
                                StreamFrames(dbg, cand.addr, vfd, cmd_handle, syncpt,
                                             cfg_addr, dst_addr, sa, sW, sH);
                            }
                            VicStage("st:3_done");
                        }
                        UnmapCmdBuffer(vfd, sh);
                    }
                }


                if (found) {
                    if (DbgRead(dbg, live_a, cand.addr, 16)) {
                        os::SleepThread(TimeSpan::FromMilliSeconds(120));
                        if (DbgRead(dbg, live_b, cand.addr, 16)) {
                            live_ok = true;
                            live_changed = std::memcmp(live_a, live_b, 16) != 0;
                        }
                    }

                    if (g_ind_buf != nullptr && g_ind_size >= FbBlockRow) {
                        const u64 t0 = armTicksToNs(armGetSystemTick());
                        for (u64 done = 0; done < FbBlockRow; done += 0x10000) {
                            const u64 n = (FbBlockRow - done < 0x10000) ? (FbBlockRow - done) : 0x10000;
                            if (!DbgRead(dbg, g_ind_buf + done, cand.addr + done, n)) { break; }
                            ++strip_chunks_ok;
                        }
                        strip_ns = armTicksToNs(armGetSystemTick()) - t0;

                        u32 hist[256] = {};
                        const u64 got = static_cast<u64>(strip_chunks_ok) * 0x10000;
                        const u64 lim = (got > FbBlockRow) ? FbBlockRow : got;
                        for (u64 i = 0; i < lim; ++i) {
                            const u8 v = g_ind_buf[i];
                            if (v != 0) { ++nonzero; }
                            ++hist[v];
                        }
                        for (u32 i = 0; i < 256; ++i) { if (hist[i] != 0) { ++distinct; } }
                        for (u64 i = 0; i + 3 < lim; i += 4) {
                            ++total_px;
                            for (u32 l = 0; l < 4; ++l) { if (g_ind_buf[i + l] == 0xFF) { ++lane_ff[l]; } }
                        }

                        /* The real per-frame cost: read a whole slot into RAM in
                         * block-row strips with no SD in the loop. This is what a
                         * streaming capture actually does every frame, so it is the
                         * number that decides whether 60 fps holds up. */
                        {
                            const u64 t1 = armTicksToNs(armGetSystemTick());
                            for (u64 done = 0; done < FbSlotSize; done += FbBlockRow) {
                                const u64 n = (FbSlotSize - done < FbBlockRow) ? (FbSlotSize - done) : FbBlockRow;
                                if (!DbgRead(dbg, g_ind_buf, cand.addr + done, n)) { break; }
                                ++full_strips;
                            }
                            full_ns = armTicksToNs(armGetSystemTick()) - t1;
                        }

                        /* ---- M57: THE TRANSPORT ---------------------------
                         * Every piece below is already proven separately; this
                         * is the first time they are joined end to end.
                         *
                         * The loop above reuses g_ind_buf at offset 0 and keeps
                         * nothing - it only times the read. This one reads into
                         * SUCCESSIVE offsets so the whole slot stays resident,
                         * which only became possible once M56 demonstrated a
                         * 16 MB heap (g_ind_size 16,580,608 vs FbSlotSize
                         * 8,847,360).
                         *
                         * resume_game() ran at the top of this block, so the
                         * game is already running: the send costs it nothing and
                         * there is no freeze to budget for.
                         *
                         * g_ind_buf is normal CACHED memory - the uncached
                         * SetMemoryAttribute calls cover only the four VIC
                         * buffers below VicBufsEnd - and sits at addr+0x30000,
                         * so it satisfies both of usbDs's requirements. Sending
                         * from the uncached VIC buffers would fail the way
                         * fs::WriteFile did with 0xd401. */
                        /* M83: not after an H.264 stream - its receiver would get a raw frame */
                        if (!g_stream_armed && !g_nvstream_armed && g_ind_size >= FbSlotSize) {
                            if (!UsbReady()) {
                                LogLine("   usb: not Configured (no host, or \"usb\" absent from the arm file) - skipping transport");
                            } else {
                                const u64 u0 = armTicksToNs(armGetSystemTick());
                                bool read_ok = true;
                                for (u64 done = 0; done < FbSlotSize; done += 0x10000) {
                                    const u64 n = (FbSlotSize - done < 0x10000) ? (FbSlotSize - done) : 0x10000;
                                    if (!DbgRead(dbg, g_ind_buf + done, cand.addr + done, n)) { read_ok = false; break; }
                                }
                                const u64 read_ns = armTicksToNs(armGetSystemTick()) - u0;
                                LogLine("   usb: resident slot read %s in %llu ms (%llu B)",
                                        read_ok ? "OK" : "FAILED",
                                        static_cast<unsigned long long>(read_ns / 1000000),
                                        static_cast<unsigned long long>(FbSlotSize));

                                if (read_ok) {
                                    /* must match sft_hdr_t in tools/raw-recv/raw-recv.c */
                                    struct __attribute__((packed)) SftHdr {
                                        u32 magic; u16 version; u16 flags;
                                        u32 width, height, stride, length, block_h_log2, kind;
                                    };
                                    static_assert(sizeof(SftHdr) == 32, "header must match the PC receiver");

                                    SftHdr h = {};
                                    h.magic        = 0x52544653u;   /* "SFTR" */
                                    h.version      = 1;
                                    h.flags        = 0;
                                    h.width        = 1920;
                                    h.height       = 1080;
                                    h.stride       = 7680;
                                    h.length       = static_cast<u32>(FbSlotSize);
                                    h.block_h_log2 = 4;
                                    h.kind         = 0xFE;
                                    std::memcpy(g_usb_hdr, std::addressof(h), sizeof(h));

                                    const u64 s0 = armTicksToNs(armGetSystemTick());
                                    size_t hdr_sent = 0, body_sent = 0;
                                    const bool hdr_ok = UsbSendBuffer(g_usb_hdr, sizeof(SftHdr), std::addressof(hdr_sent));
                                    const bool body_ok = hdr_ok && UsbSendBuffer(g_ind_buf, FbSlotSize, std::addressof(body_sent));
                                    const u64 send_ns = armTicksToNs(armGetSystemTick()) - s0;

                                    const u64 mbps = (send_ns > 0)
                                        ? (static_cast<u64>(body_sent) * UINT64_C(1000)) / (send_ns / UINT64_C(1000) + 1) / UINT64_C(1024)
                                        : 0;
                                    LogLine("   usb: header %s (%zu B), body %s (%zu / %llu B) in %llu ms ~ %llu KB/s",
                                            hdr_ok ? "sent" : "FAILED", hdr_sent,
                                            body_ok ? "sent" : "FAILED", body_sent,
                                            static_cast<unsigned long long>(FbSlotSize),
                                            static_cast<unsigned long long>(send_ns / 1000000),
                                            static_cast<unsigned long long>(mbps));
                                    LogLine("   usb: %s", (hdr_ok && body_ok)
                                            ? "*** A 1080p FRAME LEFT THE CONSOLE OVER USB ***"
                                            : "transport incomplete - see the rc above");
                                }
                            }
                        }

                        /* ---- sustained per-frame capture ------------------
                         * One timing on a title screen is not a streaming
                         * capture. This is the loop a real implementation runs:
                         * wait until the game presents, read the slot it just
                         * presented into, repeat. It measures the per-frame cost
                         * and - just as important - what the GAME's own frame
                         * rate does while we are doing it. */
                        if (!g_stream_armed && !g_nvframe_armed && !g_nvstream_armed && !g_nvp_armed && !g_nv1080_armed) {
                            constexpr u32 CapFrames = 120;

                            const u32 c0 = g_queue_count.load(std::memory_order_relaxed);
                            os::SleepThread(TimeSpan::FromMilliSeconds(1000));
                            fps_before = g_queue_count.load(std::memory_order_relaxed) - c0;

                            const u32 during0 = g_queue_count.load(std::memory_order_relaxed);
                            u32 seen = during0, last_sig = 0;
                            const u64 cap_t0 = armTicksToNs(armGetSystemTick());

                            for (u32 i = 0; i < CapFrames; ++i) {
                                u32 spins = 0;
                                while (g_queue_count.load(std::memory_order_relaxed) == seen && spins < 200) {
                                    os::SleepThread(TimeSpan::FromMilliSeconds(1));
                                    ++spins;
                                }
                                if (g_queue_count.load(std::memory_order_relaxed) == seen) { ++cap_missed; break; }
                                seen = g_queue_count.load(std::memory_order_relaxed);

                                /* read the slot the game just PRESENTED, not the
                                 * one it is drawing into - that is the whole
                                 * point of having the binder intercept. */
                                const s32 slot = g_queue_slot.load(std::memory_order_relaxed);
                                const u64 off  = (slot >= 0 && static_cast<u32>(slot) < 3) ? FbSlotOff[slot] : 0;

                                if (slot >= 0 && slot < 4) { ++slot_hits[slot]; }

                                const u64 t2 = armTicksToNs(armGetSystemTick());
                                bool ok = true;
                                u32  sig = 0;
                                for (u64 done = 0; done < FbSlotSize; done += FbBlockRow) {
                                    const u64 n = (FbSlotSize - done < FbBlockRow) ? (FbSlotSize - done) : FbBlockRow;
                                    if (!DbgRead(dbg, g_ind_buf, cand.addr + off + done, n)) { ok = false; break; }
                                    if (done == 0) {
                                        /* Hash block-row 0 - the TOP of the picture.
                                         * M39 hashed the buffer AFTER the loop, which
                                         * holds the last strip: rows 1024-1151, almost
                                         * all padding below the 1080-row image. It
                                         * barely changes, which is why only 31 of 120
                                         * frames looked distinct. */
                                        for (u32 k = 0; k < 8192; k += 8) { sig = sig * 31u + g_ind_buf[k]; }
                                    }
                                }
                                const u64 dt = armTicksToNs(armGetSystemTick()) - t2;
                                if (!ok) { ++cap_missed; continue; }

                                ++cap_done;
                                cap_sum += dt;
                                if (dt < cap_min) { cap_min = dt; }
                                if (dt > cap_max) { cap_max = dt; }
                                if (dt > UINT64_C(16667000)) { ++cap_over; }
                                if (sig != last_sig) { ++cap_distinct; last_sig = sig; }
                            }
                            cap_ns = armTicksToNs(armGetSystemTick()) - cap_t0;
                            if (cap_ns > 0) {
                                fps_during = static_cast<u32>((static_cast<u64>(seen - during0) * UINT64_C(1000000000)) / cap_ns);
                            }

                            const u32 c1 = g_queue_count.load(std::memory_order_relaxed);
                            os::SleepThread(TimeSpan::FromMilliSeconds(1000));
                            fps_after = g_queue_count.load(std::memory_order_relaxed) - c1;
                        }
                    }
                }

                /* the pump must be off the handle before it is closed */
                if (!DbgIsDmnt(dbg)) { DebugPumpStop(); }
                DbgDetach(dbg);
            }
            /* ---- detached; log everything --------------------------------- */

            VicStage("dbg:2_attached");
            LogLine("   attach(pid=%llu) rc=0x%x via %s: %s",
                    static_cast<unsigned long long>(pid.value), r_attach.GetValue(),
                    attached ? (DbgIsDmnt(dbg) ? "dmnt:cht" : "direct") : "-",
                    attached ? "ATTACHED" : "failed");
            if (!attached) { VicStage("dbg:2_FAILED"); return; }

            LogLine("   walked %u regions (%s); %u big enough (%u exactly %llu B); %u probe reads",
                    steps, walk_complete ? "complete" : "HIT THE STEP CAP", nbig, nexact,
                    static_cast<unsigned long long>(FbSwapSize), reads);
            for (u32 i = 0; i < nbig; ++i) {
                LogLine("     region %u: base=0x%010llx size=%llu%s", i,
                        static_cast<unsigned long long>(big[i].base),
                        static_cast<unsigned long long>(big[i].size),
                        (big[i].size == FbSwapSize) ? "   <== EXACT SWAPCHAIN SIZE" : "");
            }

            if (!found) {
                LogLine("   no exactly-sized region and no signature match");
                VicStage("dbg:no_signature");
                return;
            }

            LogLine("   *** SWAPCHAIN AT 0x%010llx (offset %llu into its region) - matched by %s ***",
                    static_cast<unsigned long long>(cand.addr),
                    static_cast<unsigned long long>(cand.addr - cand.region_base),
                    by_size ? "EXACT SIZE" : "pixel signature");
            for (u32 k = 0; k < 3; ++k) {
                const u8 *h = cand.head[k];
                LogLine("       slot%u +0x%07llx: %02x %02x %02x %02x  %02x %02x %02x %02x  %02x %02x %02x %02x  %02x %02x %02x %02x",
                        k, static_cast<unsigned long long>(FbSlotOff[k]),
                        h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7],
                        h[8], h[9], h[10], h[11], h[12], h[13], h[14], h[15]);
            }

            LogLine("   GAME FROZEN FOR %llu ms (M45 was 4780 ms - only the strip read needs a halt)",
                    static_cast<unsigned long long>(frozen_ns / 1000000));
            LogLine("   drained %u debug events; ContinueDebugEvent rc=0x%x -> %s",
                    nev, r_cont.GetValue(),
                    resumed ? "GAME RUNNING WHILE WE STAY ATTACHED" : "still frozen");
            if (resumed) { DebugPumpLogStats("attached, in total"); }

            if (live_ok) {
                LogLine("   live sample over 120 ms: %s", live_changed
                        ? "*** PIXELS CHANGED - the game is presenting while attached ***"
                        : "identical (game may be paused, or the corner is static)");
                LogLine("       t0: %02x %02x %02x %02x %02x %02x %02x %02x",
                        live_a[0], live_a[1], live_a[2], live_a[3], live_a[4], live_a[5], live_a[6], live_a[7]);
                LogLine("       t1: %02x %02x %02x %02x %02x %02x %02x %02x",
                        live_b[0], live_b[1], live_b[2], live_b[3], live_b[4], live_b[5], live_b[6], live_b[7]);
            }

            if (strip_chunks_ok > 0 && strip_ns > 0) {
                const u64 got     = static_cast<u64>(strip_chunks_ok) * 0x10000;
                const u64 kbps    = (got * UINT64_C(1000000)) / strip_ns;
                const u64 slot_us = (strip_ns * (FbSlotSize / 1024)) / (got / 1024) / 1000;
                LogLine("   read %llu B of block-row in %llu us -> %llu MB/s",
                        static_cast<unsigned long long>(got),
                        static_cast<unsigned long long>(strip_ns / 1000),
                        static_cast<unsigned long long>(kbps / 1000));
                LogLine("   => one full 8,847,360 B slot ~%llu us; 60 fps needs <= 16667 us  [%s]",
                        static_cast<unsigned long long>(slot_us),
                        (slot_us <= 16667) ? "FEASIBLE" : "too slow for 60 fps at full res");
                LogLine("   block-row: nonzero=%u/%llu  distinct=%u  pixels=%u", nonzero,
                        static_cast<unsigned long long>(got), distinct, total_px);
                LogLine("   0xFF per lane: [0]=%u [1]=%u [2]=%u [3]=%u  (the alpha lane is the big one)",
                        lane_ff[0], lane_ff[1], lane_ff[2], lane_ff[3]);
                LogLine("   first 16 B: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                        g_ind_buf[0], g_ind_buf[1], g_ind_buf[2], g_ind_buf[3],
                        g_ind_buf[4], g_ind_buf[5], g_ind_buf[6], g_ind_buf[7],
                        g_ind_buf[8], g_ind_buf[9], g_ind_buf[10], g_ind_buf[11],
                        g_ind_buf[12], g_ind_buf[13], g_ind_buf[14], g_ind_buf[15]);
            }

            if (full_strips > 0 && full_ns > 0) {
                const u64 mbps = (FbSlotSize * UINT64_C(1000000)) / full_ns / 1000;
                LogLine("   FULL SLOT into RAM: %u/9 strips, %llu us total, %llu MB/s -> %s",
                        full_strips,
                        static_cast<unsigned long long>(full_ns / 1000),
                        static_cast<unsigned long long>(mbps),
                        (full_ns / 1000 <= 16667) ? "*** FITS IN A 60 fps FRAME BUDGET ***"
                                                  : "over budget for 60 fps");
            }

            LogLine("   ---- VIC on real game pixels ----");
            LogLine("   nvmapOwn(capture, cacheable) rc=0x%x -> pinned at 0x%08x", strip_own_rc, strip_pin);
            LogLine("   block-row read %s; strip blit %s", strip_read_ok ? "OK" : "FAILED",
                    strip_blit_ok ? "*** COMPLETED ***" : "did not complete");
            LogLine("   swept %u variants: %u completed, %u dumped", StripVariantCount, strip_ok_count, strip_dump_count);
            for (u32 v = 0; v < StripVariantCount && v < 8; ++v) {
                /* No "identical - field inert" claim here. A byte sum is blind to
                 * reordering, which is exactly what these variants do, and that
                 * label sent three runs chasing a layout bug that did not exist.
                 * Sameness is decided on the PC by md5, not here by a sum. */
                LogLine("     %-12s bytesum=%u", StripVariants[v].name, variant_sum[v]);
            }
            LogLine("   dumps: %s %s   (compare with tools/compare_vic.py)",
                    strip_dumped ? "strip.bin OK" : "strip.bin FAILED",
                    vic_dumped   ? "vic.bin OK"   : "vic.bin FAILED");

            if (cap_done > 0) {
                const u64 avg     = cap_sum / cap_done;
                const u32 cap_fps = (cap_ns > 0) ? static_cast<u32>((static_cast<u64>(cap_done) * UINT64_C(1000000000)) / cap_ns) : 0;
                LogLine("   ---- sustained per-frame capture ----");
                LogLine("   captured %u full frames in %llu ms -> %u fps  (%u distinct, %u missed)",
                        cap_done, static_cast<unsigned long long>(cap_ns / 1000000),
                        cap_fps, cap_distinct, cap_missed);
                LogLine("   per-frame read: min %llu us  avg %llu us  max %llu us  (%u of %u over the 16667 us budget)",
                        static_cast<unsigned long long>(cap_min / 1000),
                        static_cast<unsigned long long>(avg / 1000),
                        static_cast<unsigned long long>(cap_max / 1000),
                        cap_over, cap_done);
                LogLine("   slots read: [0]=%u [1]=%u [2]=%u  (a rotating swapchain should hit all three)",
                        slot_hits[0], slot_hits[1], slot_hits[2]);
                /* Compare against the average of before and after, with a tight
                 * tolerance. M39 used "before - 6" and called 56 vs 59/61 fps
                 * unaffected; that hid a real ~4 fps dip. */
                const u32 base = (fps_before + fps_after) / 2;
                LogLine("   game presented: %u fps before, %u during, %u after -> %d fps delta  [%s]",
                        fps_before, fps_during, fps_after,
                        static_cast<int>(fps_during) - static_cast<int>(base),
                        (fps_during + 3 >= base) ? "*** GAME UNAFFECTED ***" : "GAME SLOWED while capturing");
            } else if (cap_missed > 0) {
                LogLine("   capture loop never got a frame (%u misses) - was the game presenting?", cap_missed);
            }

            if (dumped && dump_ns > 0) {
                const u64 mbps = (FbSlotSize * UINT64_C(1000000)) / dump_ns / 1000;
                LogLine("   *** WROTE A WHOLE FRAME (game stopped - no tearing): %s (%llu B, %u chunks, %llu ms, %llu MB/s incl. SD) ***",
                        FrameBinPath, static_cast<unsigned long long>(FbSlotSize), dump_chunks,
                        static_cast<unsigned long long>(dump_ns / 1000000),
                        static_cast<unsigned long long>(mbps));
                char meta[256];
                const int n = std::snprintf(meta, sizeof(meta),
                        "addr=0x%llx\nslot_bytes=%llu\nwidth=1920\nheight=1080\npadded_height=1152\n"
                        "pitch=7680\nbpp=4\nblock_height_log2=4\nkind=0xfe\nformat=A8B8G8R8 (bytes R,G,B,A)\n",
                        static_cast<unsigned long long>(cand.addr),
                        static_cast<unsigned long long>(FbSlotSize));
                fs::DeleteFile(FrameTxtPath);
                if (n > 0 && R_SUCCEEDED(fs::CreateFile(FrameTxtPath, n))) {
                    fs::FileHandle mf;
                    if (R_SUCCEEDED(fs::OpenFile(std::addressof(mf), FrameTxtPath, fs::OpenMode_Write))) {
                        static_cast<void>(fs::WriteFile(mf, 0, meta, n, fs::WriteOption::Flush));
                        fs::CloseFile(mf);
                    }
                }
                VicStage("dbg:FRAME_ON_SD");
            } else {
                LogLine("   frame dump did not complete (%u chunks)", dump_chunks);
                VicStage("dbg:dump_failed");
            }
        }

        // Legacy NVENC ladder/magic probe removed: Ambilight uses VIC only.

        /* ---- M73: NVJPG, the hardware JPEG encoder -----------------------
         *
         * The compression path. See applet_mitm_nvjpg.cpp for why this engine
         * rather than NVENC.
         *
         * Every attempt gets a FRESH CHANNEL. That is the fix for the flaw
         * that invalidated M71: one channel shared across attempts means the
         * first job to hang the engine stalls every later job identically, so
         * nothing after the first is informative. Open, submit, wait, close.
         *
         * input_type is not documented in NVIDIA's public header, so it is
         * swept rather than guessed. memory_mode 0 is semi-planar, which is
         * what the VIC already produces. */
        bool NvjpgOneAttempt(u32 nvmap_fd, u32 cmd_handle, u32 dst_handle,
                             const NvjpgLayout &L, u32 memory_mode, u32 input_type,
                             u32 *out_scan_len, u32 *out_err) {
            *out_scan_len = 0; *out_err = 0xFFFFFFFFu;

            u32 jfd = 0, nverr = 0;
            if (R_FAILED(NvOpen("/dev/nvhost-nvjpg", std::addressof(jfd), std::addressof(nverr))) || nverr != 0) {
                LogLine("   nvjpg: /dev/nvhost-nvjpg open FAILED nverr=%u", nverr);
                return false;
            }

            u32 syncpt = 0;
            {
                struct { u32 module_id; u32 syncpt; } gs = { 0, 0 };
                nverr = 0;
                NvIoctl(jfd, NvHostIocChannelGetSyncpoint, std::addressof(gs), sizeof(gs), std::addressof(nverr));
                syncpt = gs.syncpt;
            }
            /* M73 never logged this. If nvjpg were handed the VIC's syncpoint
             * (12), our increments would corrupt nvnflinger's - the M63
             * RESCUE_INCR failure - so it needs to be on the record. */
            if (syncpt == 12) {
                LogLine("   nvjpg: syncpt is 12, the SAME one nvnflinger composites on - REFUSING");
                NvClose(jfd);
                return false;
            }
            { struct { u32 fd; } sn = { nvmap_fd }; nverr = 0;
              NvIoctl(jfd, NvHostIocChannelSetNvmapFd, std::addressof(sn), sizeof(sn), std::addressof(nverr)); }
            /* M74: deliberately NOT setting SET_SUBMIT_TIMEOUT. M73 set 1000 ms,
             * so every stalled attempt ended in a channel abort, and repeated
             * engine resets are a plausible contributor to the compositor
             * corruption that ended that run. nvservices' own default is
             * gentler, and we close the channel ourselves anyway. */

            u32 buf_addr = 0, cmd_addr = 0;
            MapCmdBuffer(jfd, dst_handle, std::addressof(buf_addr), "nvjpg-buf", 0);
            MapCmdBuffer(jfd, cmd_handle, std::addressof(cmd_addr), "nvjpg-cmd", 0);
            if (buf_addr == 0) {
                LogLine("   nvjpg: buffer pin failed - refusing to submit");
                NvClose(jfd);
                return false;
            }

            /* M74: M27's lesson, applied to this engine at last. M73's fourth
             * case ran a PLANAR encode with chroma_v_addr = 0, i.e. handed the
             * engine a null surface - and a zero address does not fail
             * politely, it wedges the pipe. It took the compositor with it and
             * cost a forced power-off. Every address the engine will
             * dereference is checked here, once. */
            const u32 a_param = buf_addr + L.param_off,  a_status = buf_addr + L.status_off;
            const u32 a_bits  = buf_addr + L.bits_off,   a_luma   = buf_addr + L.luma_off;
            const u32 a_chroma = buf_addr + L.chroma_off;
            const u32 a_chroma_v = (memory_mode == 1) ? (a_chroma + L.w * L.h / 4) : 0;
            if (a_param == 0 || a_status == 0 || a_bits == 0 || a_luma == 0 || a_chroma == 0 ||
                (memory_mode == 1 && a_chroma_v == 0)) {
                LogLine("   nvjpg: REFUSING to submit, a surface address is zero "
                        "(param=%#x status=%#x bits=%#x luma=%#x cu=%#x cv=%#x)",
                        a_param, a_status, a_bits, a_luma, a_chroma, a_chroma_v);
                UnmapCmdBuffer(jfd, dst_handle);
                NvClose(jfd);
                return false;
            }

            NvjpgFillParams(g_vic_dst_buf, L, memory_mode, input_type);
            std::memset(g_vic_dst_buf + L.status_off, 0, sizeof(nvjpg_enc_status));
            std::memset(g_vic_dst_buf + L.bits_off, 0, 0x1000);
            armDCacheFlush(g_vic_dst_buf, VicDstSize);

            auto *w = reinterpret_cast<u32 *>(g_vic_cmd_buf);
            u32 n = NvjpgBuildCmdbuf(w, a_param, a_status, a_bits, a_luma, a_chroma, a_chroma_v);
            n = AppendIncrSyncpt(w, n, syncpt, true);
            armDCacheFlush(g_vic_cmd_buf, VicCmdSize);

            alignas(8) u8 sb[16 + 12 + 20 + 4] = {};
            u32 off = 0;
            auto put = [&](u32 v) { std::memcpy(sb + off, std::addressof(v), 4); off += 4; };
            put(1); put(0); put(1); put(1);
            put(cmd_handle); put(0); put(n);
            put(syncpt); put(1); put(0); put(0); put(0);
            const u32 fence_off = off; put(0);
            const u32 sz = off;
            const u32 req = (UINT32_C(3) << 30) | (sz << 16) | (0x00u << 8) | 0x01u;
            u32 fence_val = 0; nverr = 0;
            const auto rc = NvIoctl(jfd, req, sb, sz, std::addressof(nverr));
            std::memcpy(std::addressof(fence_val), sb + fence_off, 4);

            bool done = false;
            u32 seen = 0;
            if (R_SUCCEEDED(rc) && nverr == 0) {
                const u32 cfd = CtrlFd();
                if (cfd != 0) {
                    struct { u32 id; u32 thresh; u32 timeout; } a = { syncpt, fence_val, 300 };
                    u32 we = 0;
                    NvIoctl(cfd, NvHostIocCtrlSyncptWait, std::addressof(a), sizeof(a), std::addressof(we));
                    struct { u32 id; u32 value; } r = { syncpt, 0 };
                    for (u32 spin = 0; spin < 400 && !done; ++spin) {
                        u32 e2 = 0; r.value = 0;
                        NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e2));
                        seen = r.value;
                        done = (seen >= fence_val);
                        if (!done) { os::SleepThread(TimeSpan::FromMicroSeconds(200)); }
                    }
                }
            }
            armDCacheFlush(g_vic_dst_buf, VicDstSize);

            const auto *st = reinterpret_cast<const nvjpg_enc_status *>(g_vic_dst_buf + L.status_off);
            *out_err      = st->error_status;
            *out_scan_len = st->bitstream_size;

            LogLine("   nvjpg mem_mode=%u input_type=%u words=%u rc=0x%x nverr=%u fence %u/%u %s"
                    "  err=%u bytes=%u mcu=%ux%u cycles=%u",
                    memory_mode, input_type, n, rc, nverr, seen, fence_val,
                    done ? "REACHED" : "STALLED",
                    st->error_status, st->bitstream_size, st->mcu_x, st->mcu_y, st->cycle_count);

            UnmapCmdBuffer(jfd, dst_handle);
            NvClose(jfd);
            return done && st->error_status == 0 && st->bitstream_size > 0;
        }

        void TryNvjpgProbe(u32 nvmap_fd, u32 cmd_handle, u32 dst_handle) {
            if (g_engine_wedged) { LogLine("   nvjpg-enc: skipped - an engine job is stuck this boot"); return; }
            if (g_clk_armed) { ClocksHoldForEngines("nvjpg-enc"); }
            VicStage("jpg:1");

            NvjpgLayout L = {};
            L.param_off  = 0x000000;
            L.status_off = 0x002000;
            L.bits_off   = 0x003000;
            L.bits_size  = 0x080000;
            L.luma_off   = 0x090000;
            L.w = 1280; L.h = 720;
            L.chroma_off = L.luma_off + L.w * L.h;
            L.quality    = g_nvjpg_quality;
            static_assert(0x090000 + 1280 * 720 * 3 / 2 <= VicDstSize, "nvjpg probe surfaces must fit");

            /* A gradient with a hard edge: compresses to something obviously
             * non-trivial, and any plane mix-up is visible at a glance. */
            for (u32 y = 0; y < L.h; ++y) {
                u8 *row = g_vic_dst_buf + L.luma_off + y * L.w;
                for (u32 x = 0; x < L.w; ++x) {
                    row[x] = static_cast<u8>(((x * 255) / L.w) ^ ((y < L.h / 2) ? 0 : 0xFF));
                }
            }
            for (u32 y = 0; y < L.h / 2; ++y) {
                u8 *row = g_vic_dst_buf + L.chroma_off + y * L.w;
                for (u32 x = 0; x < L.w / 2; ++x) {
                    row[2 * x + 0] = static_cast<u8>((x * 255) / (L.w / 2));
                    row[2 * x + 1] = static_cast<u8>((y * 255) / (L.h / 2));
                }
            }

            auto wrap_and_save = [&](u32 scan_len, u32 attempt) {
                if (scan_len == 0 || scan_len > L.bits_size) { return; }
                u8 *out = g_vic_dst_buf + L.chroma_off + L.w * L.h / 2;
                const u32 avail = VicDstSize - static_cast<u32>(out - g_vic_dst_buf);
                if (scan_len + 0x400 > avail) { return; }
                const u32 total = NvjpgWriteContainer(out, L.w, L.h, L.quality,
                                                      g_vic_dst_buf + L.bits_off, scan_len);
                char path[64];
                std::snprintf(path, sizeof(path), "sdmc:/nvjpg-%03u.jpg", attempt);
                /* M78: `out` is inside the uncached engine buffer - the same
                 * write that could never have succeeded (see WriteSdVerified) */
                if (WriteEngineOutputToSd(path, out, total, nullptr)) {
                    LogLine("   *** NVJPG ENCODED %u BYTES -> %s (%u B file) ***", scan_len, path, total);
                }
            };

            /* ---- M74: the power-gating test ------------------------------
             *
             * M73 established that NVJPG fails exactly as NVENC does: channel
             * real, submit accepted, engine never runs, cycles=0. Two engines,
             * two unrelated configs, one with no version word to get wrong. So
             * the blocker is not configuration.
             *
             * The one difference left between these engines and the VIC, which
             * works for us, is that nvnflinger drives the VIC continuously. It
             * is powered, clocked and initialised before we touch it. NVENC and
             * NVJPG sit idle, and an idle Tegra engine is clock/power-gated - a
             * gated engine behaves precisely like this.
             *
             * That is testable without guessing a single ioctl. The system uses
             * NVJPG to encode screenshots. So: submit one well-formed job per
             * second for a long window while the console's own software wakes
             * the engine. If an attempt completes exactly when a screenshot is
             * taken, gating is the answer and the fix is to find how nvservices
             * ungates. If every attempt fails regardless, gating is ruled out
             * and the cause is elsewhere.
             *
             * Either result is worth the run, which is the point. */
            const u32 attempts = g_nvjpg_attempts;
            LogLine("   ---- NVJPG POWER-GATING TEST (quality %u, %u attempts, ~1/s) ----",
                    g_nvjpg_quality, attempts);
            LogLine("   TAKE SCREENSHOTS NOW - press Capture repeatedly for the next ~%u seconds.", attempts);
            LogLine("   The system encodes every screenshot with this engine; if one of our jobs");
            LogLine("   completes in the same second, power gating is confirmed.");

            u32 n_ok = 0, n_fail = 0, first_ok = 0;
            for (u32 i = 0; i < attempts; ++i) {
                u32 scan_len = 0, err = 0;
                const bool ok = NvjpgOneAttempt(nvmap_fd, cmd_handle, dst_handle, L,
                                                0 /* semi-planar, what the VIC makes */,
                                                0 /* input_type: M73 showed 0,1,2 behave alike */,
                                                std::addressof(scan_len), std::addressof(err));
                if (ok) {
                    if (n_ok == 0) { first_ok = i; }
                    ++n_ok;
                    wrap_and_save(scan_len, i);
                    /* One success answers the question. Keep going a little so
                     * the log shows whether it stays up or gates off again. */
                    if (n_ok >= 3) { break; }
                } else {
                    ++n_fail;
                }
                os::SleepThread(TimeSpan::FromMilliSeconds(1000));
            }

            LogLine("   ---- GATING TEST RESULT: %u completed, %u failed%s ----",
                    n_ok, n_fail,
                    n_ok > 0 ? "  *** THE ENGINE CAN BE MADE TO RUN ***"
                             : "  (no attempt ran - power gating is NOT the explanation)");
            if (n_ok > 0) {
                LogLine("   first success was attempt #%u - correlate that second with when "
                        "a screenshot was taken", first_ok);
            }
            VicStage("jpg:done");
        }

        /* ---- M76: NVJPG decode, the positive control ----------------------
         *
         * Every non-VIC job this project has submitted carried a configuration
         * that was itself a guess, so no stall could be attributed: engine,
         * clock, submit path and config were all unknowns at once. This job
         * removes the config from that list. The picture-info record is
         * generated on the PC by tools/nvjpg_dec_control.py in the T210 layout
         * that averne's oss-nvjpg drives on this hardware under Horizon, and
         * was checked byte-for-byte against that project's own struct and
         * parser (0 diffs in 0xB2C bytes). The method sequence is the same one
         * oss-nvjpg submits, plus the SETCL an L4T kernel would insert.
         *
         * So the outcomes mean something:
         *   completes, output matches  -> our submit path drives non-VIC
         *                                 engines; M68-M74 were clock and/or
         *                                 config. Encoding is a config problem.
         *   stalls with the clock held -> the problem is in how THIS process
         *                                 reaches the engine, not in any config.
         *
         * Two guards, both from this project's own history:
         *   - no submit unless clkrst shows NVJPG clocked after the mm:u
         *     request. A job the engine cannot run is what wedged M74.
         *   - on a stall the channel is deliberately LEFT OPEN. M74 found the
         *     wedge inside teardown (NvClose blocked ~20 s, then the compositor
         *     went), so this run does not tear down a channel with a stuck job.
         */
        void TryNvjpgDecodeControl(u32 nvmap_fd, u32 cmd_handle, u32 dst_handle) {
            namespace ctl = nvjpg_dec_control;
            VicStage("jd:1");
            LogLine("   ---- NVJPG DECODE POSITIVE CONTROL (M77: clock ensured after open, read at submit) ----");
            if (g_engine_wedged) { LogLine("   an earlier engine job never completed - not submitting"); return; }
            ON_SCOPE_EXIT { ClockWatchStop(); };

            /* M77: informational only. M76 gated the whole probe here on a
             * hold made ~10 s earlier, and Run B found NVJPG back at 0 by the
             * time it looked. The clock is now established after the channel
             * is open (the order oss-nvjpg and nvtegra both use) and checked
             * immediately before the submit - see ClockEnsure below. */
            ClockSurvey("jpgdec-pre");

            /* Layout inside the (uncached) dst buffer. Every offset is
             * 256-aligned: host1x carries addresses >> 8. */
            constexpr u32 PicOff = 0x000000, StatOff = 0x001000, ScanOff = 0x002000, OutOff = 0x010000;
            constexpr u32 OutSize = ctl::Pitch * ctl::Height;
            static_assert(sizeof(ctl::PictureInfo) <= StatOff - PicOff);
            static_assert(sizeof(ctl::ScanData) <= OutOff - ScanOff);
            static_assert(OutOff + OutSize <= VicDstSize);

            u32 jfd = 0, nverr = 0;
            if (R_FAILED(NvOpen("/dev/nvhost-nvjpg", std::addressof(jfd), std::addressof(nverr))) || nverr != 0) {
                LogLine("   jpgdec: /dev/nvhost-nvjpg open FAILED nverr=%u", nverr);
                VicStage("jd:open_FAILED");
                return;
            }
            u32 syncpt = 0;
            {
                struct { u32 module_id; u32 syncpt; } gs = { 0, 0 };
                nverr = 0;
                NvIoctl(jfd, NvHostIocChannelGetSyncpoint, std::addressof(gs), sizeof(gs), std::addressof(nverr));
                syncpt = gs.syncpt;
            }
            if (syncpt == 0 || syncpt == 12) {
                LogLine("   jpgdec: syncpt=%u (%s) - REFUSING", syncpt, syncpt == 12 ? "nvnflinger's" : "none");
                NvClose(jfd);
                VicStage("jd:bad_syncpt");
                return;
            }
            { struct { u32 fd; } sn = { nvmap_fd }; nverr = 0;
              NvIoctl(jfd, NvHostIocChannelSetNvmapFd, std::addressof(sn), sizeof(sn), std::addressof(nverr)); }

            u32 buf_addr = 0, cmd_addr = 0;
            MapCmdBuffer(jfd, dst_handle, std::addressof(buf_addr), "jpgdec-buf", 0);
            MapCmdBuffer(jfd, cmd_handle, std::addressof(cmd_addr), "jpgdec-cmd", 0);
            if (buf_addr == 0 || cmd_addr == 0) {
                LogLine("   jpgdec: pin failed (buf=%#x cmd=%#x) - refusing to submit", buf_addr, cmd_addr);
                if (buf_addr != 0) { UnmapCmdBuffer(jfd, dst_handle); }
                if (cmd_addr != 0) { UnmapCmdBuffer(jfd, cmd_handle); }
                NvClose(jfd);
                VicStage("jd:pin_FAILED");
                return;
            }
            const u32 a_pic = buf_addr + PicOff, a_stat = buf_addr + StatOff;
            const u32 a_scan = buf_addr + ScanOff, a_out = buf_addr + OutOff;

            std::memset(g_vic_dst_buf, 0, OutOff);
            std::memcpy(g_vic_dst_buf + PicOff,  ctl::PictureInfo, sizeof(ctl::PictureInfo));
            std::memcpy(g_vic_dst_buf + ScanOff, ctl::ScanData,    sizeof(ctl::ScanData));
            std::memset(g_vic_dst_buf + OutOff, 0xAB, OutSize);   /* poison: any write is visible */
            armDCacheFlush(g_vic_dst_buf, OutOff + OutSize);

            auto *w = reinterpret_cast<u32 *>(g_vic_cmd_buf);
            u32 n = 0;
            auto m = [&](u32 method, u32 value) {
                w[n++] = vic::Host1xOpcodeIncr(vic::UCLASS_METHOD_OFFSET, 2);
                w[n++] = method >> 2;
                w[n++] = value;
            };
            w[n++] = vic::Host1xOpcodeSetClass(0, 0xC0, 0);
            m(0x200, 1);             /* operation type: decode        */
            m(0x708, a_pic  >> 8);   /* picture info                  */
            m(0x70C, a_stat >> 8);   /* status ("read info")          */
            m(0x710, a_scan >> 8);   /* entropy-coded scan            */
            m(0x714, a_out  >> 8);   /* output surface                */
            m(0x300, 0x100);         /* EXECUTE, awaken               */
            n = AppendIncrSyncpt(w, n, syncpt, true);
            armDCacheFlush(g_vic_cmd_buf, VicCmdSize);
            LogLine("   jpgdec: syncpt=%u pic=%#x stat=%#x scan=%#x (%zu B) out=%#x, %u words",
                    syncpt, a_pic, a_stat, a_scan, sizeof(ctl::ScanData), a_out, n);

            alignas(8) u8 sb[16 + 12 + 20 + 4] = {};
            u32 off = 0;
            auto put = [&](u32 v) { std::memcpy(sb + off, std::addressof(v), 4); off += 4; };
            put(1); put(0); put(1); put(1);
            put(cmd_handle); put(0); put(n);
            put(syncpt); put(1); put(0); put(0); put(0);
            const u32 fence_off = off; put(0);
            const u32 sz = off;
            const u32 req = (UINT32_C(3) << 30) | (sz << 16) | (0x00u << 8) | 0x01u;

            /* M77: the clock, established with the channel already open and
             * everything else prepared, then read once more with nothing but
             * a timestamp between that read and the submit. */
            LogMark("jd:clock");
            const u32 ensured_hz = ClockEnsure(PcvModule_NVJPG, "jpgdec");
            /* the breadcrumb goes BEFORE the final read: it is an SD flush,
             * and nothing that slow may sit between the read and the submit */
            LogMark("jd:submit");
            u32 jpg_hz = 0;
            const bool readable = ClockRateOf(PcvModule_NVJPG, std::addressof(jpg_hz));
            const u64 t_read = armTicksToNs(armGetSystemTick());
            if (ensured_hz == 0 || !readable || jpg_hz == 0) {
                /* Nothing has been submitted, so closing is safe. */
                LogLine("   jpgdec: NOT submitting - ClockEnsure=%u Hz, final read %s %u Hz. An engine that",
                        ensured_hz, readable ? "=" : "UNREADABLE", jpg_hz);
                LogLine("   cannot run is what wedged M74. The clk-ensure lines above are this run's result.");
                UnmapCmdBuffer(jfd, dst_handle);
                UnmapCmdBuffer(jfd, cmd_handle);
                NvClose(jfd);
                VicStage("jd:no_clock");
                return;
            }

            u32 fence_val = 0; nverr = 0;
            const auto rc = NvIoctl(jfd, req, sb, sz, std::addressof(nverr));
            const u64 t_submit = armTicksToNs(armGetSystemTick());
            std::memcpy(std::addressof(fence_val), sb + fence_off, 4);
            LogMark("jd:submitted");

            bool done = false;
            u32 seen = 0;
            const u64 t0 = armTicksToNs(armGetSystemTick());
            if (R_SUCCEEDED(rc) && nverr == 0) {
                const u32 cfd = CtrlFd();
                if (cfd != 0) {
                    struct { u32 id; u32 thresh; u32 timeout; } a = { syncpt, fence_val, 300 };
                    u32 we = 0;
                    NvIoctl(cfd, NvHostIocCtrlSyncptWait, std::addressof(a), sizeof(a), std::addressof(we));
                    struct { u32 id; u32 value; } r = { syncpt, 0 };
                    for (u32 spin = 0; spin < 400 && !done; ++spin) {
                        u32 e2 = 0; r.value = 0;
                        NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e2));
                        seen = r.value;
                        done = (seen >= fence_val);
                        if (!done) { os::SleepThread(TimeSpan::FromMicroSeconds(200)); }
                    }
                }
            }
            const u64 us = (armTicksToNs(armGetSystemTick()) - t0) / 1000;
            armDCacheFlush(g_vic_dst_buf, OutOff + OutSize);

            struct Status { u32 used_bytes, mcu_x, mcu_y, reserved, result, pad[3]; };
            static_assert(sizeof(Status) == 0x20);
            Status st;
            std::memcpy(std::addressof(st), g_vic_dst_buf + StatOff, sizeof(st));

            u32 written = 0;
            for (u32 i = 0; i < OutSize; ++i) { if (g_vic_dst_buf[OutOff + i] != 0xAB) { ++written; } }
            u32 after_hz = 0;
            const bool after_ok = ClockRateOf(PcvModule_NVJPG, std::addressof(after_hz));
            LogLine("   jpgdec: submit rc=0x%x nverr=%u fence %u/%u %s after %llu us | NVJPG %u Hz read %llu us before submit returned, %u Hz%s after"
                    " | status used=%u mcu=%ux%u result=%u | %u/%u output bytes written",
                    rc, nverr, seen, fence_val, done ? "REACHED" : "STALLED",
                    static_cast<unsigned long long>(us), jpg_hz,
                    static_cast<unsigned long long>((t_submit - t_read) / 1000), after_hz, after_ok ? "" : " (unreadable)",
                    st.used_bytes, st.mcu_x, st.mcu_y, st.result, written, OutSize);

            if (done) {
                /* compare 8x8 block means with what libjpeg decodes on the PC */
                u32 worst = 0, worst_block = 0;
                for (u32 by = 0; by < ctl::Height / 8; ++by) {
                    for (u32 bx = 0; bx < ctl::Width / 8; ++bx) {
                        for (u32 ch = 0; ch < 3; ++ch) {
                            u32 sum = 0;
                            for (u32 y = by * 8; y < by * 8 + 8; ++y) {
                                for (u32 x = bx * 8; x < bx * 8 + 8; ++x) {
                                    sum += g_vic_dst_buf[OutOff + y * ctl::Pitch + x * 4 + ch];
                                }
                            }
                            const u32 mean = (sum + 32) / 64;
                            const u32 idx = (by * (ctl::Width / 8) + bx) * 3 + ch;
                            const u32 want = ctl::ExpectedBlockMeans[idx];
                            const u32 d = mean > want ? mean - want : want - mean;
                            if (d > worst) { worst = d; worst_block = idx / 3; }
                        }
                    }
                }
                const u8 *p0 = g_vic_dst_buf + OutOff;
                LogLine("   jpgdec: px(0,0)=%02x %02x %02x %02x  px(63,0)=%02x %02x %02x %02x  px(0,63)=%02x %02x %02x %02x",
                        p0[0], p0[1], p0[2], p0[3],
                        p0[63 * 4], p0[63 * 4 + 1], p0[63 * 4 + 2], p0[63 * 4 + 3],
                        p0[63 * ctl::Pitch], p0[63 * ctl::Pitch + 1], p0[63 * ctl::Pitch + 2], p0[63 * ctl::Pitch + 3]);
                LogLine("   jpgdec: worst 8x8 block-mean deviation %u (block %u)  ->  %s", worst, worst_block,
                        worst <= 6 ? "*** NVJPG RAN AND DECODED CORRECTLY - this process can drive a non-VIC engine ***"
                                   : "engine ran, output differs - check nvjpg-dec.rgba on the PC");

                /* raw surface to SD for tools/nvjpg_dec_control.py check. M78:
                 * staged through cached memory and verified - see
                 * WriteSdVerified for what M77 Run C's file actually held. */
                static_assert(OutSize <= 0x10000);
                u32 fnv = 0;
                WriteEngineOutputToSd("sdmc:/nvjpg-dec.rgba", g_vic_dst_buf + OutOff, OutSize, std::addressof(fnv));
                UnmapCmdBuffer(jfd, dst_handle);
                UnmapCmdBuffer(jfd, cmd_handle);
                NvClose(jfd);
                VicStage(worst <= 6 ? "jd:MATCH" : "jd:ran_mismatch");
            } else if (!(R_SUCCEEDED(rc) && nverr == 0)) {
                /* nvservices refused the submit: nothing reached the engine,
                 * so there is no stuck job and teardown is safe. */
                LogLine("   jpgdec: submit REJECTED (rc=0x%x nverr=%u) - nothing reached the engine", rc, nverr);
                UnmapCmdBuffer(jfd, dst_handle);
                UnmapCmdBuffer(jfd, cmd_handle);
                NvClose(jfd);
                VicStage("jd:submit_rejected");
            } else {
                g_engine_wedged = true;
                LogLine("   jpgdec: STALLED with NVJPG clocked at %u Hz. Leaving the channel OPEN on purpose", jpg_hz);
                LogLine("   (M74: tearing down a channel with a stuck job took the compositor with it).");
                LogLine("   No further engine work this boot. Reboot the console when convenient.");
                ClockSurvey("jpgdec-stall");
                VicStage("jd:STALLED_left_open");
            }
        }

        // The former GRC NVENC IDR replay probe is not needed for WLED.
        // The shared H.264 encoder layout below still needs this offset.
        constexpr u32 SetupRc = __builtin_offsetof(nvenc_h264_drv_pic_setup_s, rate_control);

        /* ---- M82/M83: NVENC on real game frames ------------------------------
         *
         * The presented slot is read out of the game (the debug-SVC route, M56),
         * the VIC converts it to 1280x720 NV12 in GPU block-linear layout, and
         * NVENC encodes that as an IDR frame with grc's setup at constant QP
         * (RCMODE 0: M81 variant c showed the QP then comes from the setup's
         * I-frame QP, patched per encode, with no rate-control state).
         *
         * M83 fixes the two things M82 Run H measured:
         *   - LAYOUT. M82 wrote 32-row blocks (VIC OutBlkHeight 2, log2 GOBs, the
         *     same number as grc's NVENC block_height 2). NVENC read those bytes
         *     as 16-row blocks: Run H's decode matched the VIC's bytes read at
         *     h=1 at 47.9 dB (QP 16) and at h=2 at 9.5 dB, luma and chroma alike.
         *     So NVENC's block_height 2 means 16 rows, and the VIC writes
         *     OutBlkHeight 1. Planes round to 16 rows: luma 720, chroma 368 -
         *     grc's own chroma allocation was 368 rows, which already said so.
         *   - COLOUR. The VIC's matrix is now understood (tools/vic_csc.py: the
         *     law reproduces all 528 values Run H measured) and programmed with
         *     real BT.709 limited range, generated into vic_csc_bt709.h.
         *
         * One arena, past the three stage buffers, holds every NVENC surface
         * plus the NVENC pushbuffer, and is mapped into BOTH channels: the VIC
         * writes the picture into it and NVENC reads it from there. The NVENC
         * pushbuffer does not share the VIC's: the VIC's completion is judged
         * on syncpoint 12, which the compositor also advances, so overwriting
         * the VIC's pushbuffer with NVENC methods after a fence that fired early
         * could feed them to the VIC. */
        constexpr u32 NvfW = 1280, NvfH = 720;
        constexpr OutDesc NvfOut{ NvfW, NvfH, NvfW, vic::PIXFMT_Y8_U8V8_N420, vic::BLK_KIND_GENERIC_16Bx2, 1 };
        constexpr u32 NvfLuma = Nv12LumaBytes(NvfOut), NvfChroma = Nv12ChromaBytes(NvfOut);
        constexpr u32 NvfOffSetup  = 0x000000;
        constexpr u32 NvfOffStatus = 0x001000;
        constexpr u32 NvfOffRc     = 0x002000;
        constexpr u32 NvfOffCmd    = 0x006000;
        constexpr u32 NvfOffBits   = 0x010000;  constexpr u32 NvfBitsSize = 0x160000;
        constexpr u32 NvfOffHist   = NvfOffBits + NvfBitsSize;
        constexpr u32 NvfOffCur    = NvfOffHist + 0x0C0000;
        constexpr u32 NvfOffCurUV  = NvfOffCur + NvfLuma;
        constexpr u32 NvfOffRefOut = (NvfOffCurUV + NvfChroma + 0xFFF) & ~0xFFFu;
        constexpr u32 NvfArenaSize = NvfOffRefOut + 0x160000;
        constexpr size_t NvfArenaBase = FbBlockRowStage + 3 * StreamStageSize;   /* past both stream stages */
        /* v0.1.1b: the live capture may use the first stage (g_stage_buf, which
         * the live stream never touches) as the tail of a larger slot */
        constexpr size_t MaxSlotBytes = FbBlockRowStage + StreamStageSize;
        static_assert(NvfLuma == 1280 * 720 && NvfChroma == 1280 * 368, "16-row blocks: what NVENC reads");
        static_assert(NvfOffCurUV % 0x100 == 0 && NvfArenaBase % 0x1000 == 0);
        static_assert(sizeof(nvenc_grc_idr::Setup) <= NvfOffStatus && sizeof(nvenc_pic_stat_s) <= NvfOffRc - NvfOffStatus);
        constexpr u32 NvfPictureIndex = 0x4D383300u;   /* "M83\0" + n */
        constexpr u32 SetupRcQpI = SetupRc + __builtin_offsetof(nvenc_h264_rc_s, QP) + 2;   /* QP[P,B,I] */
        static_assert(SetupRcQpI == 0x73 && nvenc_grc_idr::Setup[SetupRcQpI] == 24, "grc's I-frame QP is 24");
        constexpr u32 SetupIdrPicId = __builtin_offsetof(nvenc_h264_drv_pic_setup_s, pic_control)
                                    + __builtin_offsetof(nvenc_h264_pic_control_s, idr_pic_id);
        static_assert(SetupIdrPicId == 0x180 && nvenc_grc_idr::Setup[SetupIdrPicId] == 0,
                      "tools/nvenc_replay.py reads idr_pic_id at 0x180; grc's setup_8 has 0");

        /* the production matrix: RGB -> BT.709 limited range (tools/vic_csc.py gen) */
        constexpr CscProbe NvfBt709 = { "bt709", 1, vic_csc::Shift, {
            { vic_csc::Bt709[0][0], vic_csc::Bt709[0][1], vic_csc::Bt709[0][2], vic_csc::Bt709[0][3] },
            { vic_csc::Bt709[1][0], vic_csc::Bt709[1][1], vic_csc::Bt709[1][2], vic_csc::Bt709[1][3] },
            { vic_csc::Bt709[2][0], vic_csc::Bt709[2][1], vic_csc::Bt709[2][2], vic_csc::Bt709[2][3] } } };

        /* Handheld, MK8 renders 1280x720 into the top-left of the same
         * 1920x1080 surface (M36/M37). That corner is block-rows 0-5 (128 rows
         * each) and, in each, the first 80 of 120 GOB columns (1280 px x 4 B /
         * 64 B) - 655,360 contiguous bytes per block-row. Reading just those is
         * 3.9 MB instead of 8.8 MB, and the VIC samples only that rectangle. */
        constexpr u64 CornerBlockRowBytes = 80ull * 8192;
        constexpr u32 CornerBlockRows = 6;
        static_assert(CornerBlockRows * 128 >= NvfH && CornerBlockRowBytes * 120 / 80 == FbBlockRow);

        /* M90: everything size-dependent about an encode arena, so one set of
         * code drives 720p (the constants above, unchanged) and 1080p. */
        struct NvfLayout {
            u32 w, h;
            OutDesc out;                  /* what the VIC writes for NVENC */
            u32 luma, chroma;             /* plane bytes, 16-row blocks */
            u32 off_setup, off_status, off_rc, off_cmd, off_bits, bits_size, off_hist;
            u32 off_cur, off_cur_uv, off_ref_out, arena_size;
            u32 off_setup_p, off_ref_b, off_me_a, off_me_b, arena_size_p;   /* P frames */
            u32 off_cur2, off_cur2_uv, arena_size_pipe;                     /* M94: a second picture, for the pipeline */
            const u8 *idr_setup, *p_setup;
            const u8 *hdrs; u32 hdrs_len;
        };
        const NvfLayout &Layout720();     /* defined past the P-frame constants */

        struct VicFrameContext {
            u8 *a = nullptr;      /* VIC destination arena, CPU view */
            u32 ah = 0;           /* nvmap handle for the VIC arena */
            const NvfLayout *lay = nullptr;
        };

        /* Where one job's setup and references live in the arena. The default
         * is the IDR job; a P job (M83 nvp) adds grc's three P-only bindings. */
        struct NvfJob {
            u32 setup  = NvfOffSetup;
            u32 ref_out = NvfOffRefOut;
            u32 ref_in = 0;           /* 0: IDR - no ME or reference inputs */
            u32 me_in = 0, me_out = 0;
            u32 cur = 0, cur_uv = 0;  /* M94: 0 = the layout's picture */
        };

        constinit bool g_nvf_last_fence_done = false, g_nvf_last_ours = false;
        constinit u32  g_nvf_last_fence = 0, g_nvf_last_pic = 0;

        /* One job, grc's shape, RCMODE 0. Returns 0 done, 1 rejected, 2 no
         * status within 1 s (the caller treats that as a stall). */
        /* M94: submit only. 0 submitted (fence and start time out), 1 rejected. */
        /* Wait for a submitted job: 0 done, 2 still incomplete after 1 s.
         * Check completion before the deadline: a descheduled waiter can wake
         * after the deadline even though NVENC completed while it slept. */
        /* M95: has a submitted job finished? One syncpoint read, one status look,
         * no waiting - for use inside another wait. */
        /* v0.7.8: a 4-byte pixel's offset in any block-linear RGBA surface -
         * GOBs of 64 bytes x 8 rows (512 B), 2^bh_log2 GOBs to a block. MK8's
         * 1920-px stride with 16-GOB blocks was the only layout the size check
         * knew (Xenoblade Chronicles X back in handheld kept streaming 1080p). */
        constexpr u64 BlOffset(u32 x, u32 y, u32 stride_px, u32 bh_log2) {
            const u32 xb = x * 4;
            const u32 block_rows = 8u << bh_log2, block_bytes = 512u << bh_log2;
            const u64 gobs_x = (static_cast<u64>(stride_px) * 4 + 63) / 64;
            return static_cast<u64>(y / block_rows) * gobs_x * block_bytes + static_cast<u64>(xb / 64) * block_bytes
                 + ((y % block_rows) / 8) * 512 + ((xb % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((xb % 32) / 16) * 32 + (y % 2) * 16 + (xb % 16);
        }
        /* the MK8 formula it replaces, at the probe's corners */
        constexpr u64 Mk8Offset(u32 x, u32 y) {
            const u32 xb = x * 4;
            return (y / 128) * 120 * 8192 + (xb / 64) * 8192 + ((y % 128) / 8) * 512
                 + ((xb % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((xb % 32) / 16) * 32 + (y % 2) * 16 + (xb % 16);
        }
        static_assert(BlOffset(1300, 10, 1920, 4) == Mk8Offset(1300, 10) && BlOffset(1900, 1070, 1920, 4) == Mk8Offset(1900, 1070) &&
                      BlOffset(10, 740, 1920, 4) == Mk8Offset(10, 740) && BlOffset(1279, 719, 1920, 4) == Mk8Offset(1279, 719));

        /* All zero outside the top-left 1280x720 means handheld content. */
        bool SlotContentIs720(const u8 *slot, u32 *out_samples, u32 stride_px, u32 bh_log2) {
            auto px = [&](u32 x, u32 y) -> const u8 * { return slot + BlOffset(x, y, stride_px, bh_log2); };
            u32 n = 0;
            bool zero = true;
            for (u32 y = 10; y < 1080; y += 100) {
                for (u32 x = 1300; x < 1920; x += 100) { const u8 *p = px(x, y); zero &= (p[0] | p[1] | p[2]) == 0; ++n; }
            }
            for (u32 y = 740; y < 1080; y += 50) {
                for (u32 x = 10; x < 1280; x += 150) { const u8 *p = px(x, y); zero &= (p[0] | p[1] | p[2]) == 0; ++n; }
            }
            *out_samples = n;
            return zero;
        }

        /* v0.7.3: what a 1080p slot holds, for the 720p/1080p decision. Smash
         * drawing 720p into the corner after docked mode leaves the docked
         * picture around it - not black, so "all black outside" kept the
         * stream at 1080p with the old frame framing the new one. So also:
         * a hash of the same 140 points outside the corner and of 64 inside,
         * compared from one check to the next - a 720p picture changes inside
         * only. */
        struct ContentProbe { bool ok, outside_black, inside_black; u32 outside_hash, inside_hash; };

        /* v0.7.3: what the last size check decided, for the session it
         * restarts: its first frame cannot tell a 720p picture framed by an
         * old docked one from a 1080p picture (pid, buffer set, 1 = 720p) */
        struct SizeHint { u64 pid; u32 gen; s32 corner; };
        constinit SizeHint g_size_hint = { 0, 0, -1 };

        ContentProbe ProbeContent(::ams::svc::Handle dbg, u64 base, u32 stride_px, u32 bh_log2) {
            auto off = [&](u32 x, u32 y) -> u64 { return BlOffset(x, y, stride_px, bh_log2); };
            ContentProbe r = { true, true, true, 2166136261u, 2166136261u };
            alignas(8) u8 px[8];
            auto sample = [&](u32 x, u32 y, u32 *h, bool outside) {
                if (!r.ok) { return; }
                if (!DbgRead(dbg, px, base + off(x, y), 4)) { r.ok = false; return; }
                if ((px[0] | px[1] | px[2]) != 0) { (outside ? r.outside_black : r.inside_black) = false; }
                for (u32 i = 0; i < 3; ++i) { *h = (*h ^ px[i]) * 16777619u; }
            };
            for (u32 y = 10; y < 1080; y += 100) { for (u32 x = 1300; x < 1920; x += 100) { sample(x, y, std::addressof(r.outside_hash), true); } }
            for (u32 y = 740; y < 1080; y += 50) { for (u32 x = 10; x < 1280; x += 150) { sample(x, y, std::addressof(r.outside_hash), true); } }
            for (u32 y = 30; y < 720; y += 90) { for (u32 x = 40; x < 1280; x += 160) { sample(x, y, std::addressof(r.inside_hash), false); } }
            return r;
        }

        /* The 720p/1080p check both live streams run (v0.7.1-v0.7.6 rules, see
         * the H.264 stream). v0.7.8: on a clock - every 250 ms, not every 15
         * frames: Xenoblade Chronicles X runs at 30 fps, so 8 checks took 4 s
         * and the player left before the stream switched - and the game's own
         * crop going from 1920x1080 to 1280x720 or less switches to 720p at the
         * next check (it reported 1280x720 the moment it was back in handheld).
         * Smash keeps 1280x720 even fake-docked, so only the change counts.
         * v0.7.9: a game whose crop has ever been 1920x1080 is believed
         * outright - XCX back in handheld (crop 1280x720) still draws outside
         * the corner, and the content vote took it back to 1080p after 4 s
         * (masagrator). Only games that never showed a whole 1920x1080 crop
         * (Smash) are judged by their pixels.
         * And each check compares a slot with that slot's last probe: after
         * docked mode every slot holds a different old docked frame around the
         * corner, so slot-to-slot it looked like moving content and the 720p
         * stream went straight back to 1080p (Run v078-1, MK8). Every 15
         * frames had only worked because 15 is a multiple of 3 slots. */
        /* v0.7.18: "an unchanging old picture outside the corner" is how a game
         * that keeps its 1920x1080 buffers looks back in handheld (Mario Kart 8),
         * but a still background there looks the same: Project Diva Mega Mix
         * flipped 1080p -> 720p -> 1080p every 3-10 s in play (#43, ten restarts
         * a minute). A stale switch undone by movement out there within 30 s
         * marks the signal unreliable for that game until it exits; black
         * outside the corner and the game's own crop still count. */
        constinit u64 g_stale_switch_pid = 0, g_stale_switch_ns = 0;
        constinit u64 g_stale_unreliable_pid = 0;
        /* v0.7.19: and for the game itself (its program id) for the rest of the
         * boot - Diva learned it again after every restart of the game (#43) */
        constinit u64 g_stale_unreliable_tid = 0;

        struct SizeCheck {
            bool started_corner = false;
            u32 small_streak = 0, big_streak = 0, stale_streak = 0;
            u32 crop_small_streak = 0;      /* v0.7.15: checks in a row with a crop inside 1280x720 */
            u32 crop_720_streak = 0;        /* v0.7.15: ... of exactly 1280x720 */
            bool was_full = false;          /* v0.7.15: a 1920x1080 crop this session */
            ContentProbe last[8] = {};      /* per slot */
            u64 next_ns = 0;

            static bool CropFull(u32 crop) { return (crop >> 16) >= 1920 && (crop & 0xFFFF) >= 1080; }
            static bool CropSmall(u32 crop) { return crop != 0 && (crop >> 16) <= 1280 && (crop & 0xFFFF) <= 720; }
            /* v0.7.15: wider or taller than the handheld corner - not a 720p picture */
            static bool CropBig(u32 crop) { return crop != 0 && ((crop >> 16) > 1280 || (crop & 0xFFFF) > 720); }

            explicit SizeCheck(bool corner) : started_corner(corner),
                was_full(CropFull(g_queue_crop_wh.load(std::memory_order_relaxed))),
                next_ns(armTicksToNs(armGetSystemTick()) + UINT64_C(250000000)) {}

            /* nullptr, or why the stream restarts at the other size (g_size_hint set) */
            const char *Step(::ams::svc::Handle dbg, u64 addr, s32 slot, u32 stride_px, u32 bh_log2) {
                const u64 now = armTicksToNs(armGetSystemTick());
                if (now < next_ns) { return nullptr; }
                next_ns = now + UINT64_C(250000000);
                /* v0.7.16: a crop in the middle (Kingdom Hearts zooming in) says nothing about the mode */
                const u32 crop = g_queue_crop_origin0.load(std::memory_order_relaxed) ? g_queue_crop_wh.load(std::memory_order_relaxed) : 0;
                const bool crop_full = CropFull(crop);
                /* v0.7.15: a dynamic resolution dips into 1280x720 for a moment
                 * docked too (Super Mario Galaxy): 720p once the crop is
                 * exactly 1280x720 for 0.5 s (a game back in handheld says so
                 * at once - XCX), or anything inside it for 2 s. The dip itself
                 * is drawn right meanwhile (CropDynamic). */
                was_full = was_full || crop_full;
                crop_small_streak = CropSmall(crop) ? crop_small_streak + 1 : 0;
                crop_720_streak = crop == (1280u << 16 | 720u) ? crop_720_streak + 1 : 0;
                const bool small_now = crop_720_streak >= 2 || crop_small_streak >= 8;
                if (!started_corner && was_full && small_now) { return Switch(true, "the game switched to a 720p picture (its picture size went to 1280x720)"); }
                const u64 pid = g_app_pid.load(std::memory_order_relaxed);
                if (pid != 0 && g_crop_full_pid.load(std::memory_order_relaxed) == pid) {
                    if (!started_corner && small_now) { return Switch(true, "the game switched to a 720p picture (its picture size is 1280x720)"); }
                    /* any crop bigger than the corner cannot be the handheld picture */
                    if (started_corner && (crop_full || CropBig(crop))) { return Switch(false, "the game switched to a 1080p picture (its picture is bigger than 1280x720)"); }
                    return nullptr;
                }
                const ContentProbe pr = ProbeContent(dbg, addr, stride_px, bh_log2);
                ContentProbe &prev = last[static_cast<u32>(slot) & 7];
                const u64 tid = g_app_tid.load(std::memory_order_relaxed);
                const bool stale_ok = pid == 0 || (g_stale_unreliable_pid != pid && (tid == 0 || g_stale_unreliable_tid != tid));   /* v0.7.18, v0.7.19 */
                s32 vote = -1;    /* 1: 720p (black outside), 2: 720p (old picture outside), 0: 1080p, -1: no evidence */
                if (pr.ok) {
                    const bool moved_in  = prev.ok && pr.inside_hash != prev.inside_hash;
                    const bool moved_out = prev.ok && pr.outside_hash != prev.outside_hash;
                    if (crop_full)                                { vote = 0; }
                    else if (pr.outside_black && pr.inside_black) { vote = -1; }
                    else if (pr.outside_black)                    { vote = 1; }
                    else if (moved_out)                           { vote = 0; }
                    else if (moved_in && stale_ok)                { vote = 2; }
                }
                prev = pr;
                /* 720p after 8 checks in a row (2 s), 1080p after 2 (0.5 s) */
                small_streak = (vote == 1 || vote == 2) ? small_streak + 1 : 0;
                stale_streak = vote == 2 ? stale_streak + 1 : 0;
                big_streak   = vote == 0 ? big_streak + 1 : 0;
                if (!started_corner && small_streak >= 8) {
                    if (stale_streak != 0) { g_stale_switch_pid = pid; g_stale_switch_ns = now; }
                    return Switch(true, stale_streak != 0 ? "the game switched to a 720p picture (an unchanging old picture outside the corner)"
                                                          : "the game switched to a 720p picture (black outside the corner)");
                }
                if (started_corner && big_streak >= 2) {
                    /* v0.7.18: back to 1080p soon after a stale switch - that was a still background */
                    if (pid != 0 && g_stale_switch_pid == pid && now - g_stale_switch_ns < UINT64_C(30000000000) && g_stale_unreliable_pid != pid) {
                        g_stale_unreliable_pid = pid;
                        g_stale_unreliable_tid = g_app_tid.load(std::memory_order_relaxed);
                        LogLine("   nvstream: the picture outside the corner only stood still - this game is not taken for 720p by that again");
                    }
                    return Switch(false, "the game switched to a 1080p picture");
                }
                return nullptr;
            }

            const char *Switch(bool to720, const char *why) {
                g_size_hint = { g_app_pid.load(std::memory_order_relaxed), g_game_surface.generation, to720 ? 1 : 0 };
                return why;
            }
        };

        /* Everything one real-frame run needs, opened fresh and torn down in
         * reverse: the slot copy pinned for the VIC, the arena pinned for the
         * VIC and for a new msenc channel. A channel whose job stalled is left
         * OPEN (M74: tearing down a stuck channel took the compositor with it). */
        /* the last per-slot buffer match (VicCaptureSession::Calibrate): the next
         * session's first guess, so a restart (a size change, the viewer
         * reopened) starts on the right buffers */
        /* v0.7.16: + whose buffer set it was (pid, generation) and whether it was matched, not a give-up */
        struct SlotMatch { u64 addr[8]; u32 n; u32 buf; u64 pid; u32 gen; bool matched; s32 score; };
        constinit SlotMatch g_slot_match = {};

        /* Everything one real-frame run needs, opened fresh and torn down in
         * reverse: the slot copy pinned for the VIC, the arena pinned for the
         * VIC and for a new msenc channel. A channel whose job stalled is left
         * OPEN (M74: tearing down a stuck channel took the compositor with it). */
        /* v0.7.17: where a homebrew's swapchain buffers are, from the app
         * itself. libnx keeps every nvmap object in a struct NvMap { u32
         * handle; u32 id; u32 size; void *cpu_addr; ... } (nvidia/map.h) -
         * deko3d's memory blocks and libnx's Framebuffer both hold one - and
         * the parcels give us each slot's id. So: find those structs in the
         * app's writable memory (its .data/.bss and the start of its heap)
         * and take cpu_addr. gen1recomp and Dusklight draw into buffers no
         * device but the GPU maps, among dozens of their size: watching them
         * change never told the slots apart (flicker, a broken picture).
         * Out: out[k] the CPU address of ids[k]'s object; false unless all. */
        u32 FindLibnxNvMaps(::ams::svc::Handle dbg, const u32 *ids, u32 n, u32 min_size, u64 *out) {
            if (g_ind_buf == nullptr || g_ind_size < 0x10000 || n == 0 || n > 8) { return 0; }
            const u64 t0 = armTicksToNs(armGetSystemTick());
            const size_t chunk = g_ind_size >= 0x100000 ? 0x100000 : 0x10000;
            u32 found = 0;
            for (u32 k = 0; k < n; ++k) { out[k] = 0; }
            u64 scanned = 0, addr = 0;
            /* v0.7.17: 1 GB - Dusklight (Mesa's OpenGL) keeps two of its three
             * records past the first 256 MB of a heap full of game data */
            constexpr u64 Budget = UINT64_C(1024) << 20;
            for (u32 steps = 0; steps < 4000 && found < n && scanned < Budget; ++steps) {
                ::ams::svc::MemoryInfo mi = {};
                ::ams::svc::PageInfo   pi = {};
                if (!DbgQuery(dbg, std::addressof(mi), std::addressof(pi), addr) || mi.size == 0) { break; }
                const u32 attr = static_cast<u32>(mi.attribute);
                const bool dev = (attr & ::ams::svc::MemoryAttribute_DeviceShared) != 0 || mi.device_count > 0;
                const u32 st = static_cast<u32>(mi.state);
                const bool rw = (static_cast<u32>(mi.permission) & 3) == 3;
                const bool data = st == static_cast<u32>(::ams::svc::MemoryState_CodeData) || st == static_cast<u32>(::ams::svc::MemoryState_AliasCodeData) ||
                                  st == static_cast<u32>(::ams::svc::MemoryState_Normal);
                if (rw && data && !dev) {
                    for (u64 off = 0; off < mi.size && found < n && scanned < Budget; off += chunk) {
                        const size_t len = static_cast<size_t>(mi.size - off < chunk ? mi.size - off : chunk);
                        if (!DbgRead(dbg, g_ind_buf, mi.base_address + off, len)) { break; }
                        scanned += len;
                        /* the struct is 8-aligned: id at +4, size at +8, cpu_addr at +16 */
                        for (size_t q = 0; q + 24 <= len; q += 8) {
                            u32 f[4];
                            std::memcpy(f, g_ind_buf + q, sizeof(f));
                            if (f[0] == 0 || f[2] < min_size) { continue; }
                            for (u32 k = 0; k < n; ++k) {
                                if (out[k] != 0 || f[1] != ids[k]) { continue; }
                                u64 va = 0;
                                std::memcpy(std::addressof(va), g_ind_buf + q + 16, sizeof(va));
                                if (va == 0 || (va & 0xFFF) != 0) { continue; }
                                ::ams::svc::MemoryInfo vm = {};
                                ::ams::svc::PageInfo   vp = {};
                                if (!DbgQuery(dbg, std::addressof(vm), std::addressof(vp), va)) { continue; }
                                const bool vdev = (static_cast<u32>(vm.attribute) & ::ams::svc::MemoryAttribute_DeviceShared) != 0 || vm.device_count > 0;
                                if (!vdev || va + f[2] > vm.base_address + vm.size) { continue; }
                                out[k] = va;
                                ++found;
                                /* the same id twice (one object, slots at offsets) */
                                for (u32 j = k + 1; j < n; ++j) { if (ids[j] == ids[k] && out[j] == 0) { out[j] = va; ++found; } }
                            }
                        }
                    }
                }
                const u64 next = mi.base_address + mi.size;
                if (next <= addr) { break; }
                addr = next;
            }
            char line[200];
            int len = std::snprintf(line, sizeof(line), "   swapchain: libnx's nvmap records - %u of %u found in %llu MB of the app's memory (%llu ms):", found, n,
                                    static_cast<unsigned long long>(scanned >> 20), static_cast<unsigned long long>((armTicksToNs(armGetSystemTick()) - t0) / 1000000));
            for (u32 k = 0; k < n && len > 0 && static_cast<size_t>(len) < sizeof(line); ++k) {
                len += std::snprintf(line + len, sizeof(line) - len, " %u=%llx", ids[k], static_cast<unsigned long long>(out[k]));
            }
            LogLine("%s", line);
            return found;
        }

        /* VIC capture engine implementation kept in its own source component. */
#include "ambi_capture_session.inc"

        /* WLED capture does not derive from the legacy video-stream session. */
        struct AmbilightSession final : VicCaptureSession {};

        /* M84: a read fails for one expected reason - the game was closed */
        /* v0.7.16: a game's slot could not be read - it is quitting. Give the
         * donor's Applet-pool memory back at once, as for a game that stopped
         * presenting: what comes next may need it. Kingdom Hearts' launcher
         * (010090c022978000) quits and starts the game (...003) - with the
         * 41 MB still held, the game never started and the console hung on a
         * black screen; the 1 s stall that used to free it (v0.7.13) had not
         * come yet, and with the HOME menu streaming it was never freed. The
         * HOME menu's stream then starts after the 5 s hold instead. */
        void GameLeft() {
            if (g_ui_session_active.load(std::memory_order_relaxed)) { return; }   /* a HOME-menu session: vi, not a game */
            LogLine("   live: the game quit while streaming - giving the donor's memory back for what starts next");
            PrepareForHomeMenu();
        }

        /* v0.7.18: with the HOME menu streamed, the HOME button (or the 5 s
         * fallback) ends the game's session but keeps the engines and the
         * donor, so the HOME menu's session follows at once - giving them back
         * with the 5 s hold put ~5 s without picture between the game and the
         * HOME menu (test5). The Album, an applet that needs the memory or a
         * web applet's menu still give them back. */
        bool HomeDetachKeepsEngines(const char *home_why, bool web_fast) {
            if (!g_ui_stream_armed || web_fast || home_why == nullptr) { return false; }
            return std::strcmp(home_why, "HOME button") == 0 || std::strcmp(home_why, "present stall fallback") == 0;
        }

        const char *SlotReadFailure() {
            return DebugPumpTargetGone() ? "slot read failed: the game exited" : "slot read failed";
        }

                /* P-frame buffers, past the IDR arena (M83 nvp; M85 nvstream with nvgop) */
        constexpr u32 NvpOffSetupP = NvfArenaSize;
        constexpr u32 NvpOffRefB   = NvpOffSetupP + 0x1000;
        constexpr u32 NvpOffMeA    = NvpOffRefB + 0x160000;
        constexpr u32 NvpOffMeB    = NvpOffMeA + 0x40000;     /* grc's MEPRED buffers are 128 KB apart */
        constexpr u32 NvpArenaSize = NvpOffMeB + 0x40000;
        constexpr size_t NvpArenaBase = FbBlockRowStage + StreamStageSize;   /* past g_stage_buf */
        constexpr size_t NvpAccBase   = NvpArenaBase + NvpArenaSize;        /* CPU-only: the GOP's bitstreams */
        constexpr u32 SetupFrameNum = __builtin_offsetof(nvenc_h264_drv_pic_setup_s, pic_control)
                                    + __builtin_offsetof(nvenc_h264_pic_control_s, frame_num);
        constexpr u32 SetupRcQpP = SetupRc + __builtin_offsetof(nvenc_h264_rc_s, QP);   /* QP[P] */
        static_assert(SetupFrameNum == 0x17C && nvenc_grc_p::Setup[SetupFrameNum] == 1 && nvenc_grc_p::Setup[SetupFrameNum + 2] == 2,
                      "grc's P setup: frame_num 1 at 0x17C, POC lsb 2 at 0x17E");
        static_assert(nvenc_grc_p::Setup[0x1A8] == 0x90 && nvenc_grc_idr::Setup[0x1A8] == 0x9C,
                      "pic_control word: P (type 0, ref 1) vs IDR (type 3, ref 1)");
        static_assert(sizeof(nvenc_grc_p::Setup) <= 0x1000);
        static_assert((SetupFrameNum & ~0x3Fu) == ((SetupFrameNum + 3) & ~0x3Fu), "frame_num and POC share a cache line");

        /* M90: the 720p layout is exactly the constants every 720p run used */
        const NvfLayout &Layout720() {
            static constexpr NvfLayout L = {
                NvfW, NvfH, NvfOut, NvfLuma, NvfChroma,
                NvfOffSetup, NvfOffStatus, NvfOffRc, NvfOffCmd, NvfOffBits, NvfBitsSize, NvfOffHist,
                NvfOffCur, NvfOffCurUV, NvfOffRefOut, NvfArenaSize,
                NvpOffSetupP, NvpOffRefB, NvpOffMeA, NvpOffMeB, NvpArenaSize,
                (NvpArenaSize + 0xFFFu) & ~0xFFFu, ((NvpArenaSize + 0xFFFu) & ~0xFFFu) + NvfLuma,
                ((((NvpArenaSize + 0xFFFu) & ~0xFFFu) + NvfLuma + NvfChroma) + 0xFFFu) & ~0xFFFu,
                nvenc_grc_idr::Setup, nvenc_grc_p::Setup,
                nvenc_grc_hdrs::SpsPps, static_cast<u32>(sizeof(nvenc_grc_hdrs::SpsPps)),
            };
            return L;
        }

        /* M90/M91: any size up to 1920x1088. The setups are grc's, resized by
         * tools/nvenc_replay.py resize_setup (which reproduces grc's own 720p
         * setups byte for byte at 1280x720). Planes round to 16-row blocks.
         * The reference and reconstruction buffers are 16x16-tiled luma +
         * chroma (3 MB covers 1920x1088); MEPRED scales with the macroblock
         * count (grc: 128 KB for 3600 MBs -> 512 KB each here).
         *
         * M91: the rate-control scratch buffer (SET_IO_RC_PROCESS) is 128 KB,
         * as grc's is (its RC buffer sits 0x20000 below its bitstream buffer
         * in Run E's jobs). The 720p layout gives it 16 KB and has worked;
         * Run Q's 1080p job never completed, and this is one of the sizes
         * that could scale with the picture. */
        constexpr u32 LxRc = 0x2000, LxCmd = 0x22000, LxBits = 0x30000, LxHistMax = 0x185000;
        constexpr u32 LxRefSize = 0x300000, LxMeSize = 0x80000;
        static_assert(sizeof(nvenc_1080p::IdrSetup) == 0x1000 && sizeof(nvenc_1080p::PSetup) == 0x1000);
        static_assert(nvenc_1080p::HistSize <= LxHistMax && 1920u * 1088u * 3u / 2u <= LxRefSize);

        NvfLayout MakeLayout(u32 w, u32 h, const u8 *idr, const u8 *p, const u8 *hdrs, u32 hdrs_len) {
            NvfLayout L = {};
            L.w = w; L.h = h;
            L.out = OutDesc{ w, h, w, vic::PIXFMT_Y8_U8V8_N420, vic::BLK_KIND_GENERIC_16Bx2, 1 };
            L.luma = Nv12LumaBytes(L.out);
            L.chroma = Nv12ChromaBytes(L.out);
            L.off_setup = NvfOffSetup; L.off_status = NvfOffStatus; L.off_rc = LxRc; L.off_cmd = LxCmd;
            L.off_bits = LxBits; L.bits_size = nvenc_1080p::BitsSize; L.off_hist = LxBits + nvenc_1080p::BitsSize;
            L.off_cur = L.off_hist + LxHistMax;
            L.off_cur_uv = L.off_cur + L.luma;
            L.off_ref_out = (L.off_cur_uv + L.chroma + 0xFFF) & ~0xFFFu;
            L.arena_size = L.off_ref_out + LxRefSize;
            L.off_setup_p = L.arena_size; L.off_ref_b = L.off_setup_p + 0x1000;
            L.off_me_a = L.off_ref_b + LxRefSize; L.off_me_b = L.off_me_a + LxMeSize;
            L.arena_size_p = L.off_me_b + LxMeSize;
            L.off_cur2 = L.arena_size_p;
            L.off_cur2_uv = L.off_cur2 + L.luma;
            L.arena_size_pipe = (L.off_cur2_uv + L.chroma + 0xFFFu) & ~0xFFFu;
            L.idr_setup = idr; L.p_setup = p; L.hdrs = hdrs; L.hdrs_len = hdrs_len;
            return L;
        }

        /* M92: the VIC writes 1080 rows into planes that are 1088 rows (16-row
         * blocks); NVENC codes all 1088 (nvenc_1080p.h: 1920x1088 surfaces,
         * the only 1080p-class size that completes - Run R) and the SPS in
         * SpsPps crops the bottom 8. */
        const NvfLayout &Layout1080() {
            static const NvfLayout L = MakeLayout(1920, 1080, nvenc_1080p::IdrSetup, nvenc_1080p::PSetup,
                                                  nvenc_1080p::SpsPps, static_cast<u32>(sizeof(nvenc_1080p::SpsPps)));
            return L;
        }

        /* v0.7.7: webcam-any - two pitch-linear NV12 pictures (the M70 raw
         * stream's VIC output), nothing for an encoder */
        const NvfLayout &LayoutRaw(u32 w, u32 h) {
            static NvfLayout L;
            L = {};
            L.w = w; L.h = h;
            L.out = OutDesc{ w, h, w, vic::PIXFMT_Y8_U8V8_N420 };
            L.luma = Nv12LumaBytes(L.out);
            L.chroma = Nv12ChromaBytes(L.out);
            L.off_cur = 0x1000;
            L.off_cur_uv = L.off_cur + L.luma;
            L.off_cur2 = (L.off_cur_uv + L.chroma + 0xFFFu) & ~0xFFFu;
            L.off_cur2_uv = L.off_cur2 + L.luma;
            L.arena_size = L.arena_size_p = L.arena_size_pipe = (L.off_cur2_uv + L.chroma + 0xFFFu) & ~0xFFFu;
            return L;
        }


        bool ShotFromSession(const VicCaptureSession &s);

        /* Switch Ambilight: no H.264 encoding, no USB, no PC viewer.
         * Reuses the proven Capture -> VIC path at just 320x180 NV12.
         * Failure/return paths are managed by RunLive (detach and release donor). */
        StreamEnd TryAmbilightStream(::ams::svc::Handle dbg, u64 slot_base, u32 vfd, u32 nvmap_fd,
                                    u32 cmd_handle, u32 vsyncpt, u32 cfg_addr, const GameSurface *geo) {
            constexpr u32 W = 320, H = 180;
            AmbilightSession s;
            if (geo != nullptr && !s.SetGeometry(*geo, "ambi")) { return StreamEnd::Failed; }
            if (geo != nullptr) { s.PrepareSlots(dbg, slot_base, "ambi"); }
            { u64 rn = 0; u32 sig = 0;
              if (!s.Capture(dbg, slot_base, &rn, &sig)) { return StreamEnd::GameGone; }
              s.Decide("ambi"); }
            s.lay = &LayoutRaw(W, H);
            const NvfLayout &L = s.Lay();
            s.arena_size = L.arena_size;
            if (!s.Open(vfd, nvmap_fd, cmd_handle, vsyncpt, cfg_addr, "ambi")) {
                return StreamEnd::Failed;
            }
            g_live_w.store(W, std::memory_order_relaxed);
            g_live_h.store(H, std::memory_order_relaxed);
            g_live_state.store(LiveState_Streaming, std::memory_order_relaxed);
            g_streaming_now.store(true, std::memory_order_relaxed);
            ON_SCOPE_EXIT { g_streaming_now.store(false, std::memory_order_relaxed); };
            g_vic_quiet = true;
            ON_SCOPE_EXIT { g_vic_quiet = false; };
            sftambi::State colors;
            colors.brightness = static_cast<u8>(g_ambi_brightness.load(std::memory_order_relaxed));
            colors.alpha = static_cast<u8>(g_ambi_smoothing.load(std::memory_order_relaxed));
            sftambi::Rgb rgb[sftambi::Count] = {};
            u32 frames = 0, dropped = 0;
            u64 stat_start = armTicksToNs(armGetSystemTick());
            u64 last_tx = 0;
            const u32 fps = g_ambi_fps.load(std::memory_order_relaxed);
            const u64 FrameInterval = UINT64_C(1000000000) / (fps >= 5 && fps <= 30 ? fps : 25);
            LogLine("ambi: autonomous WLED DDP/4048, 114 LEDs, 320x180 NV12; target %u fps", fps);
            for (;;) {
                const u64 now = armTicksToNs(armGetSystemTick());
                if (!g_stream_enabled.load(std::memory_order_relaxed) ||
                    g_app_excluded.load(std::memory_order_relaxed)) { return StreamEnd::Disabled; }
                if (g_reconfig_request.exchange(false, std::memory_order_relaxed) ||
                    (geo != nullptr && g_session_surface->generation != geo->generation)) { return StreamEnd::Reconfigure; }
                if (g_applet_needs_memory.load(std::memory_order_relaxed) || WebMenuUp() ||
                    (g_ui_session_active.load(std::memory_order_relaxed) && g_game_presented_in_ui.load(std::memory_order_relaxed))) {
                    PrepareForHomeMenu();
                    return StreamEnd::GameGone;
                }
                if (NetIp() == 0) { return StreamEnd::ViewerGone; }
                if (g_detach_on_home.load(std::memory_order_relaxed) && !g_ui_session_active.load(std::memory_order_relaxed)) {
                    const u64 qt = g_queue_tick.load(std::memory_order_relaxed);
                    if (qt != 0) {
                        const u64 ms = armTicksToNs(armGetSystemTick() - qt) / UINT64_C(1000000);
                        const char *why = nullptr;
                        if (HomeDetachShouldFire(ms, g_home_detach_ms, g_home_fallback_ms, &why)) {
                            PrepareForHomeMenu();
                            return StreamEnd::GameGone;
                        }
                    }
                }
                if (last_tx != 0 && now - last_tx < FrameInterval) {
                    os::SleepThread(TimeSpan::FromMilliSeconds(2));
                    continue;
                }
                u64 read_ns = 0; u32 sig = 0;
                if (!s.Capture(dbg, slot_base, &read_ns, &sig, &dropped)) {
                    GameLeft();
                    return StreamEnd::GameGone;
                }
                if (!s.Vic("ambi:vic", L.off_cur)) { return StreamEnd::Failed; }
                armDCacheFlush(s.x.a + L.off_cur, L.luma + L.chroma);
                const uint8_t *Y = s.x.a + L.off_cur;
                const uint8_t *UV = s.x.a + L.off_cur_uv;
                if (!sftambi::Sample(colors, Y, UV, W, H, L.out.stride_px, rgb, sftambi::Count)) {
                    return StreamEnd::Failed;
                }
                if (!NetAmbilightSend(rgb, sizeof(rgb)) && (frames % 100) == 0) {
                    LogLine("ambi: UDP send failed (network unavailable?)");
                }
                last_tx = armTicksToNs(armGetSystemTick());
                ++frames;
                if (last_tx - stat_start >= UINT64_C(3000000000)) {
                    LogLine("ambi: %u frames sent/attempted, %u skipped presents", frames, dropped);
                    stat_start = last_tx;
                }
                if (TakeShotRequest()) { static_cast<void>(ShotFromSession(s)); }
            }
        }

        /* ---- M99: screenshots ----------------------------------------------- */

        /* what the session just read into g_ind_buf: the handheld corner, or
         * the whole surface */
        bool ShotFromSession(const VicCaptureSession &s) {
            /* v0.7.17: the PNG writer reads 4 bytes a pixel - not a 16-bit picture */
            if (vic::Is16Bit(s.src.pixfmt)) { LogLine("shot: a 16-bit picture - no screenshot from the stream"); return false; }
            const u32 shown = s.size_check_ok ? (s.src.w << 16 | s.src.h) : VicCaptureSession::PresentRect(s.src, s.cap_crop);   /* v0.7.13: what the game shows */
            u32 w = s.corner ? NvfW : shown >> 16, h = s.corner ? NvfH : shown & 0xFFFF;
            if (s.size_check_ok) {          /* v0.7.15: dynamic resolution - only the presented part */
                SrcDesc r = s.src;
                r.rect_w = w; r.rect_h = h;
                if (const u32 dyn = VicCaptureSession::DynamicRect(r, s.cap_crop); dyn != 0) { w = dyn >> 16; h = dyn & 0xFFFF; }
            }
            const size_t n = s.CornerFast() ? static_cast<size_t>(CornerBlockRows) * FbBlockRow : s.buf_bytes;
            return WriteShot(g_shot_out, g_ind_buf, n, w, h, s.src.stride_px * 4, s.src.blk_h_log2, s.src.pixfmt == vic::PIXFMT_A8B8G8R8, s.present_flip);
        }

        bool ShotStandalone() {
            if (g_app_excluded.load(std::memory_order_relaxed)) {
                ++g_shot_fail; LogLine("shot: this app is excluded - not attaching"); NotifyShot("Screenshots are off for this app (excluded)"); return false;
            }
            ::ams::os::ProcessId pid{};
            if (g_pmdmnt_rc != 0 || R_FAILED(::ams::pm::dmnt::GetApplicationProcessId(std::addressof(pid)))) {
                ++g_shot_fail; LogLine("shot: no game is running"); return false;
            }
            GameSurface geo;
            for (u32 tries = 0; tries < 10; ++tries) {
                const u32 gen = g_game_surface.generation;
                geo = g_game_surface;
                if (gen == g_game_surface.generation) { break; }
            }
            if (geo.num_slots == 0 || geo.buf_size == 0) { ++g_shot_fail; LogLine("shot: this game's swapchain has not been seen"); return false; }
            /* v0.7.10: the frame needs the capture region - just for the shot */
            const bool took = g_ind_buf == nullptr;
            if (!CaptureAcquire("shot")) { ++g_shot_fail; NotifyShot("Screenshot failed: not enough memory free"); return false; }
            ON_SCOPE_EXIT { if (took) { CaptureRelease("shot"); } };
            ::ams::svc::Handle dbg = ::ams::svc::InvalidHandle;
            if (!DmAttach(pid.value, std::addressof(dbg))) { ++g_shot_fail; LogLine("shot: could not attach to the game"); return false; }
            u64 slot_base = 0;
            const bool found = LocateSwapchain(dbg, geo, std::addressof(slot_base), "shot");
            u32 shot_nev = 0;
            static_cast<void>(DbgResume(dbg, std::addressof(shot_nev)));
            bool ok = false;
            if (!found) {
                ++g_shot_fail; LogLine("shot: the swapchain was not found in the game's memory");
            } else {
                VicCaptureSession s;
                if (s.SetGeometry(geo, "shot")) {
                    s.PrepareSlots(dbg, slot_base, "shot");
                    s.seen = g_queue_count.load(std::memory_order_acquire) - 1;   /* the latest present counts as new */
                    u64 rn = 0; u32 sg = 0;
                    if (s.Capture(dbg, slot_base, std::addressof(rn), std::addressof(sg))) {
                        /* M99b: the frame is in g_ind_buf - let go of the game
                         * BEFORE the seconds of SD writing (Run AB held it).
                         * DbgDetach is a no-op for dmnt (it owns the handle). */
                        DbgDetach(dbg);
                        dbg = ::ams::svc::InvalidHandle;
                        s.Decide("shot");
                        ok = ShotFromSession(s);
                    } else {
                        ++g_shot_fail; LogLine("shot: %s", SlotReadFailure());
                    }
                } else {
                    ++g_shot_fail;
                }
            }
            if (dbg != ::ams::svc::InvalidHandle) { DbgDetach(dbg); }
            return ok;
        }

        /* ---- M83: the stream; M85: P frames in it ---------------------------
         *
         * game frame -> VIC (BT.709 NV12) -> NVENC -> USB bulk -> PC.
         *
         * Each frame goes out as one SFTR packet, the transport M57-M71 proved:
         * a 32-byte header in its own transfer (a short packet the host reads
         * on its own), then the payload posted asynchronously, so frame N's
         * transfer runs while frame N+1 is captured and encoded. flags bit 1
         * marks an H.264 payload: SPS + PPS + one slice, a complete Annex-B
         * access unit; bit 2 (M85) marks an IDR. Every packet carries the
         * parameter sets (a few dozen bytes), so a receiver can start at any
         * IDR. idr_pic_id alternates 0/1 as H.264 requires of consecutive IDR
         * pictures. kind carries the frame number; stride, unused for H.264,
         * carries the console-side age of the frame in us (M85: queueBuffer ->
         * header sent), which raw-view reports as the console's latency.
         *
         * nvgop=N (M85): an IDR every N frames and grc's P job in between, with
         * the references and MEPRED buffers ping-ponging as in grc's own jobs
         * (Run E) and nvp. Run J's nvp decoded IPPP... cleanly; its "drift" was
         * a one-frame offset in the probe (see TryNvencPFrames). Any encode
         * error forces the next frame to be an IDR, so a bad frame can never
         * become a reference. At the end the last frame's VIC planes are saved
         * with its frame number: the PC decodes the recording and compares, a
         * drift test at the end of a long P chain. */
        /* v0.7.7: webcam-any - the camera's frames as uncompressed NV12. Each
         * finished present goes through the VIC (BT.709, scaled to the size the
         * camera app committed, pitch-linear, as the M70 raw stream did), is
         * cut into UVC payloads in a stage buffer and handed to the sender.
         * No encoder, so nothing to pipeline: USB 2.0 is the limit (720p ~25
         * fps, 432p 60), and the loop simply waits for the previous frame to
         * leave before it reads the newest present. */
        StreamEnd TryUvcRawStream(::ams::svc::Handle dbg, u64 slot_base, u32 vfd, u32 nvmap_fd,
                                  u32 cmd_handle, u32 vsyncpt, u32 cfg_addr, const GameSurface *geo) {
            VicStage("uvcraw:1");
            if (g_stream_stage[0] == nullptr || g_stream_stage[1] == nullptr) { LogLine("   uvcraw: no stage buffers"); return StreamEnd::Failed; }
            VicCaptureSession s;
            if (geo != nullptr && !s.SetGeometry(*geo, "uvcraw")) { VicStage("uvcraw:geometry"); return StreamEnd::Failed; }
            if (geo != nullptr) { s.PrepareSlots(dbg, slot_base, "uvcraw"); }
            {
                u64 rn = 0; u32 sg = 0;
                if (!s.Capture(dbg, slot_base, std::addressof(rn), std::addressof(sg))) { LogLine("   uvcraw: %s", SlotReadFailure()); return StreamEnd::GameGone; }
                s.Decide("uvcraw");
            }
            u32 w = 0, h = 0;
            UvcFrameSize(std::addressof(w), std::addressof(h));
            g_uvc_new_consumer.store(false, std::memory_order_relaxed);
            s.lay = std::addressof(LayoutRaw(w, h));
            const NvfLayout &L = s.Lay();
            s.arena_size = L.arena_size;
            const size_t frame_bytes = L.luma + L.chroma;     /* the planes are back to back */
            LogLine("   ---- WEBCAM (any app): NV12 %ux%u, %zu B a frame, no encoder ----", w, h, frame_bytes);
            if (!s.Open(vfd, nvmap_fd, cmd_handle, vsyncpt, cfg_addr, "uvcraw")) { VicStage("uvcraw:open_FAILED"); return g_engine_wedged ? StreamEnd::EngineStall : StreamEnd::Failed; }
            g_live_w.store(w, std::memory_order_relaxed);
            g_live_h.store(h, std::memory_order_relaxed);
            g_live_state.store(LiveState_Streaming, std::memory_order_relaxed);
            g_streaming_now.store(true, std::memory_order_relaxed);
            ON_SCOPE_EXIT { g_streaming_now.store(false, std::memory_order_relaxed); };
            SetViewerReason(ViewerReason_None);
            g_vic_quiet = true;
            ON_SCOPE_EXIT { g_vic_quiet = false; };

            const char *why = "all frames sent";
            StreamEnd end = StreamEnd::Done;
            u32 parity = 0, sent = 0, frame_no = 0, dropped = 0;
            bool pending = false;
            SizeCheck size_check(s.corner);
            u64 s_read = 0, s_vic = 0, s_pack = 0, s_wait = 0, w_n = 0;
            const u64 loop_t0 = armTicksToNs(armGetSystemTick());
            u64 o_t0 = loop_t0, w_t0 = loop_t0;
            u32 o_sent0 = 0, w_sent0 = 0, o_q0 = g_queue_count.load(std::memory_order_relaxed);
            for (u32 i = 0; ; ++i) {
                const u64 t0 = armTicksToNs(armGetSystemTick());
                if (!g_stream_enabled.load(std::memory_order_relaxed)) { why = "turned off from the overlay"; end = StreamEnd::Disabled; break; }
                if (g_reconfig_request.exchange(false, std::memory_order_relaxed)) { why = "new settings from the manager app"; end = StreamEnd::Reconfigure; break; }
                if (g_app_excluded.load(std::memory_order_relaxed)) { why = "this app was excluded"; end = StreamEnd::Disabled; break; }
                if (g_ui_session_active.load(std::memory_order_relaxed) && g_game_presented_in_ui.load(std::memory_order_relaxed)) { why = "a game is on screen"; end = StreamEnd::GameGone; break; }   /* v0.7.14 */
                if (g_detach_on_home && !g_ui_session_active.load(std::memory_order_relaxed)) {
                    const u64 qt = g_queue_tick.load(std::memory_order_relaxed);
                    if (qt != 0) {
                        const u64 age_ms = armTicksToNs(armGetSystemTick() - qt) / UINT64_C(1000000);
                        const char *home_why = nullptr;
                        /* v0.7.17: 0.5 s with a web applet's menu over the game */
                        const bool web_fast = age_ms >= 500 && WebMenuUp();
                        if (web_fast || HomeDetachShouldFire(age_ms, g_home_detach_ms, g_home_fallback_ms, std::addressof(home_why))) {
                            why = "HOME / game stopped presenting";
                            if (web_fast && home_why == nullptr) { home_why = "web menu over game"; }
                            LogLine("home: detach (%s) after present stall %llu ms (probe %u, fallback %u)",
                                    home_why != nullptr ? home_why : "?",
                                    static_cast<unsigned long long>(age_ms), g_home_detach_ms, g_home_fallback_ms);
                            if (HomeDetachKeepsEngines(home_why, web_fast)) {
                                LogLine("home: the HOME menu is streamed - the engines stay for its session");
                            } else {
                                /* Free donor now on confirm (and on 5 s fallback) so
                                 * Album / SwitchU is not racing a mapped 42 MB. */
                                PrepareForHomeMenu();
                            }
                            end = StreamEnd::GameGone;
                            break;
                        }
                    }
                }
                if (geo != nullptr && g_session_surface->generation != geo->generation) { why = "the game made new frame buffers"; end = StreamEnd::Reconfigure; break; }
                if (g_uvc_new_consumer.exchange(false, std::memory_order_relaxed)) {
                    u32 nw = 0, nh = 0;
                    UvcFrameSize(std::addressof(nw), std::addressof(nh));
                    if (nw != w) { why = "the camera app asked for another size"; end = StreamEnd::Reconfigure; break; }
                }
                if (UvcHostStalled()) { why = "the camera app stopped reading"; end = StreamEnd::ViewerGone; break; }
                if (!g_uvc_streaming.load(std::memory_order_relaxed)) { why = "the camera app closed (host detached)"; end = StreamEnd::ViewerGone; break; }
                if (t0 - o_t0 >= UINT64_C(1000000000)) {
                    const u32 qn = g_queue_count.load(std::memory_order_relaxed);
                    g_live_fps_x10.store(static_cast<u32>(static_cast<u64>(sent - o_sent0) * UINT64_C(10000000000) / (t0 - o_t0)), std::memory_order_relaxed);
                    g_live_game_fps_x10.store(static_cast<u32>(static_cast<u64>(qn - o_q0) * UINT64_C(10000000000) / (t0 - o_t0)), std::memory_order_relaxed);
                    o_t0 = t0; o_sent0 = sent; o_q0 = qn;
                }
                u64 rn = 0; u32 sg = 0;
                if (!s.Capture(dbg, slot_base, std::addressof(rn), std::addressof(sg), std::addressof(dropped))) { why = SlotReadFailure(); GameLeft(); end = StreamEnd::GameGone; break; }
                const u64 t1 = armTicksToNs(armGetSystemTick());
                if (!s.Vic("uvcraw:vic", L.off_cur)) { why = "VIC did not complete"; end = StreamEnd::Failed; break; }
                if (TakeShotRequest()) { static_cast<void>(ShotFromSession(s)); }
                const u64 t2 = armTicksToNs(armGetSystemTick());
                /* the engine wrote it: drop any stale cache lines before reading */
                armDCacheFlush(s.x.a + L.off_cur, frame_bytes);
                if (pending) { UvcWaitSent(); ++sent; pending = false; }
                const u64 t3 = armTicksToNs(armGetSystemTick());
                const size_t total = UvcRawPack(g_stream_stage[parity], s.x.a + L.off_cur, frame_bytes, frame_no++);
                UvcQueueFrame(g_stream_stage[parity], total);
                pending = true;
                parity ^= 1;
                const u64 t4 = armTicksToNs(armGetSystemTick());
                s_read += t1 - t0; s_vic += t2 - t1; s_wait += t3 - t2; s_pack += t4 - t3; ++w_n;

                /* the 720p/1080p check (SizeCheck) */
                if (s.size_check_ok) {
                    const s32 sl = g_queue_slot.load(std::memory_order_relaxed);
                    if (const char *r = size_check.Step(dbg, s.SlotAddr(slot_base, sl), sl, s.src.stride_px, s.src.blk_h_log2)) {
                        why = r; end = StreamEnd::Reconfigure; break;
                    }
                }
                if ((i + 1) % 1200 == 0) {
                    const u64 wn = armTicksToNs(armGetSystemTick());
                    const u64 n = w_n ? w_n : 1;
                    const u64 fps = wn > w_t0 ? static_cast<u64>(sent - w_sent0) * UINT64_C(10000000000) / (wn - w_t0) : 0;
                    LogLine("   uvcraw window: %llu.%llu fps sent; per frame avg us: read %llu  VIC %llu  waiting for USB %llu  pack %llu",
                            static_cast<unsigned long long>(fps / 10), static_cast<unsigned long long>(fps % 10),
                            static_cast<unsigned long long>(s_read / n / 1000), static_cast<unsigned long long>(s_vic / n / 1000),
                            static_cast<unsigned long long>(s_wait / n / 1000), static_cast<unsigned long long>(s_pack / n / 1000));
                    s_read = s_vic = s_wait = s_pack = 0; w_n = 0; w_t0 = wn; w_sent0 = sent;
                    DebugPumpLogStats("uvcraw");
                }
                /* hand the core back (M60b) */
                os::SleepThread(TimeSpan::FromMicroSeconds(500));
            }
            if (pending) { UvcWaitSent(); ++sent; }
            const u64 ms = (armTicksToNs(armGetSystemTick()) - loop_t0) / 1000000;
            LogLine("   uvcraw: stopped: %s", why);
            LogLine("   uvcraw: %u frames sent in %llu ms (%llu.%llu fps), %u presents skipped", sent, static_cast<unsigned long long>(ms),
                    static_cast<unsigned long long>(ms ? sent * 1000ull / ms : 0), static_cast<unsigned long long>(ms ? (sent * 10000ull / ms) % 10 : 0), dropped);
            UvcLogStats("uvcraw");
            return end;
        }

        /* The H.264 streaming pipeline is removed; use VIC -> DDP. */

        /* ---- M83: P frames, an opt-in probe ("nvp") ---------------------------
         *
         * One IDR then N-1 P frames of real game frames, with grc's own P setup
         * (nvenc_grc_p.h, generated from Run E's setup_2 and checked to differ
         * from the IDR setup in exactly the 23 bytes grc's P setups do) and
         * grc's P job: the IDR job's methods plus SET_IN_MEPRED_DATA,
         * SET_OUT_MEPRED_DATA and SET_IN_REF_PIC0_LUMA, in grc's order. The
         * references ping-pong as grc's do (Run E: each job's OUT_REF_PIC is the
         * next job's IN_REF_PIC0; the MEPRED buffers swap). Per P frame only
         * frame_num (0x17C) and POC lsb (0x17E, = 2 x frame_num) change, as in
         * grc's setups 2, 6 and 5 (frame_num 1, 2, 3).
         *
         * It runs LAST: a P job is the first job since M71 whose setup is not an
         * IDR, and a stall ends engine work for the boot. The frames go to the
         * SD (not USB): tools/nvp_check.py decodes the GOP, checks every frame
         * decoded, and compares the last one - a P frame predicted from a chain
         * of P frames - with the VIC's own picture, which is the drift test. */
                /* ---- M90: 1080p on the encoder ("nv1080[=N]") -----------------------
         *
         * The final goal is docked 1080p60, and grc never builds a 1080p job
         * (its clips are 720p). This probe runs grc's setups resized to
         * 1920x1080 (Layout1080, nvenc_1080p.h) on real game frames, in three
         * parts, each saved or logged:
         *
         *   frame 0  - one capture, the VIC to 1920x1080 BT.709 NV12, IDR at
         *              QP 16/20/24: status, bits and the VIC's planes to the
         *              SD (nv1080-*); docked, also the game's top 128 rows for
         *              the colour check
         *   GOP      - IDR + 9 P frames at QP 20, the bitstreams and the last
         *              frame's VIC planes (nv1080-gop-*): the drift test
         *   timed    - N frames, an IDR every 60, nothing saved: read, VIC and
         *              NVENC per frame, sizes, and our rate against the game's
         *
         * Docked, MK8 renders 1920x1080 into the swapchain and the VIC converts
         * it 1:1. Handheld it renders 1280x720 into the corner; the VIC then
         * scales that UP, which tests the encoder at 1080p but not a native
         * picture - the log says which. Nothing here needs USB. */
                /* ---- M75: the grc observer ---------------------------------------
         *
         * M72-M74 cost three forced power-offs. Every one came from the same
         * mechanism: we opened a channel to an engine, submitted work that
         * never completed, and the teardown took the compositor with it.
         * host1x is shared and nvnflinger is on the other side of it.
         *
         * So this observes and submits nothing at all. No channel, no cmdbuf,
         * no engine contact of any kind. It attaches to grc - the system's own
         * recorder, the one client known to drive NVENC successfully on this
         * firmware - resumes it immediately, reads its memory, and detaches.
         *
         * That is exactly the mechanism already used on the game's framebuffer
         * at 60 fps without incident, pointed at a different process. The one
         * thing at risk is grc itself, not the display.
         *
         * What we are looking for is grc's nvenc_h264_drv_pic_setup_s. Its
         * first word is a magic constant, which makes it findable by scanning,
         * and it is the single most valuable artifact available: the exact
         * configuration a working client hands this engine. Also searched for
         * is a host1x SETCL to class 0x21, which would give the real method
         * sequence - including any method we never emit. */
        constexpr u64 GrcProgramIdValue = 0x0100000000000035ull;

        struct ScanHit { u64 addr; u32 kind; u32 word0, word1, word2, word3; };

        void TryGrcScan() {
            VicStage("grcscan:1");
            LogLine("   ---- GRC OBSERVER (read-only; no channel, no submit) ----");

            os::ProcessId pid = {};
            {
                const Result r = ::ams::pm::dmnt::GetProcessId(std::addressof(pid), ncm::ProgramId{GrcProgramIdValue});
                LogLine("   pm:dmnt GetProcessId(%016llx) rc=0x%x -> pid=%llu",
                        static_cast<unsigned long long>(GrcProgramIdValue), r.GetValue(),
                        static_cast<unsigned long long>(pid.value));
                if (R_FAILED(r) || pid.value == 0) {
                    LogLine("   grc is not running - nothing to observe");
                    VicStage("grcscan:no_proc");
                    return;
                }
            }

            ::ams::svc::Handle dbg = ::ams::svc::InvalidHandle;
            const Result r_attach = ::ams::svc::DebugActiveProcess(std::addressof(dbg), pid.value);
            LogLine("   DebugActiveProcess rc=0x%x", r_attach.GetValue());
            if (R_FAILED(r_attach)) { VicStage("grcscan:no_attach"); return; }

            /* Resume grc at once. Attaching stops it, and a stopped recorder
             * is the only thing this run can plausibly disturb - so the window
             * is made as short as the API allows, before any scanning. */
            u32 nev = 0;
            {
                ::ams::svc::DebugEventInfo ev;
                while (nev < 256 && R_SUCCEEDED(::ams::svc::GetDebugEvent(std::addressof(ev), dbg))) { ++nev; }
                const Result rc = ::ams::svc::ContinueDebugEvent(dbg,
                                      ::ams::svc::ContinueFlag_ExceptionHandled | ::ams::svc::ContinueFlag_ContinueAll,
                                      nullptr, 0);
                LogLine("   drained %u events; ContinueDebugEvent rc=0x%x -> grc %s",
                        nev, rc.GetValue(), R_SUCCEEDED(rc) ? "RESUMED" : "STILL HALTED");
            }

            static constexpr u32 NvencMagics[4] = { 0xd0b70006u, 0xc1b70006u, 0xc0b70006u, 0xa0b70006u };
            /* SETCL word: opcode 0, offset 0, class in bits 15:6. */
            const u32 setcl_nvenc = vic::Host1xOpcodeSetClass(0, 0x21, 0);
            const u32 setcl_nvjpg = vic::Host1xOpcodeSetClass(0, 0xC0, 0);
            /* M79: SET_IN_DRV_PIC_SETUP as a method-offset value. Run D found
             * grc's setups and not one NVENC SETCL - like oss-nvjpg on Horizon,
             * grc evidently leaves the class to nvservices - so its command
             * buffers are found by the write that points the engine at a
             * setup, whichever opcode carries it. */
            constexpr u32 MethodDrvPicSetup = 0x710u >> 2;

            /* g_ind_buf is the scratch this scan reads into; it only exists
             * once the VIC heap is up, so "vic" must be armed alongside. */
            if (g_ind_buf == nullptr || g_ind_size < 64 * 1024) {
                LogLine("   no scratch buffer (arm \"vic\" too) - skipping the scan");
                static_cast<void>(::ams::svc::CloseHandle(dbg));
                VicStage("grcscan:no_scratch");
                return;
            }

            enum : u32 { Hit_SetclEnc = 5, Hit_SetclJpg = 6, Hit_Method = 7, Hit_SetclVic = 8, Hit_Status = 9, NumKinds = 10 };
            const u32 setcl_vic = vic::Host1xOpcodeSetClass(0, 0x5D, 0);
            /* M81: grc's own NVENC status blocks, found by content. Run F's
             * job came back with error_status = 2 and an otherwise perfect
             * encode; if grc's frames - which record and play back fine -
             * carry the same flag, it is benign. */
            u32 status_err_hist[4] = {}, status_ucode_nonzero = 0;
            u32 nvic_dumps = 0;
            static constexpr u32 MaxVicDumps = 4;
            static constexpr u32 MaxLogged = 32;
            ScanHit hits[MaxLogged] = {};
            u32 nlogged = 0, nkind[NumKinds] = {};
            u32 nregions = 0, nskipped = 0;
            u64 scanned = 0, skipped_bytes = 0;
            /* M79: M78 stopped at a 96 MB cap in address order and may never
             * have reached grc's command buffers. Command buffers and setups
             * live in small allocations, so every region up to 8 MB is scanned
             * in full and only larger ones (frame and bitstream storage) are
             * skipped - each one noted, so nothing is silently left out. */
            constexpr u64 MaxRegion = 8ull * 1024 * 1024;
            constexpr u64 ScanCap   = 256ull * 1024 * 1024;
            constexpr u64 ChunkSize = 64 * 1024;

            /* M78: every hit is dumped, not just located, as records in the
             * M72 recorder's format (tools/nvrec.py, tools/nvsetup-dump),
             * built in the cached stage buffer and written with
             * WriteSdVerified after detaching. M79: a CMDBUF record's `rq`
             * field carries the offset of the hit inside its payload. */
            constexpr size_t RecCap = 0x40000;
            u8 *const rec = g_stage_buf;
            size_t rec_len = 0;
            const bool can_dump = rec != nullptr && 2 * RecCap <= StreamStageSize && g_ind_size >= FbBlockRowStage + 2 * RecCap;
            auto rec_put = [&](u16 kind, u32 rq, u32 a, u32 b, const void *payload, u32 len) -> u8 * {
                if (!can_dump || rec_len + 32 + len > RecCap) { return nullptr; }
                const u32 ms = static_cast<u32>(armTicksToNs(armGetSystemTick()) / UINT64_C(1000000));
                const u32 hdr[8] = { 0x4352564Eu, static_cast<u32>(kind) | (32u << 16), 0, rq, a, b, len, ms };
                std::memcpy(rec + rec_len, hdr, sizeof(hdr));
                u8 *body = rec + rec_len + 32;
                if (payload != nullptr) { std::memcpy(body, payload, len); }
                rec_len += 32 + len;
                return body;
            };
            char note[160];
            auto rec_note = [&](int n) {
                if (n > 0) { rec_put(9, 0, 0, 0, note, static_cast<u32>(n < static_cast<int>(sizeof(note)) ? n : sizeof(note) - 1)); }
            };
            /* read `len` bytes of grc at `va` straight into a new record */
            u32 ndumped = 0;
            auto dump = [&](u16 kind, u32 rq, u64 va, u32 len) -> u8 * {
                u8 *body = rec_put(kind, rq, static_cast<u32>(va >> 32), static_cast<u32>(va), nullptr, len);
                if (body == nullptr) { return nullptr; }
                if (!DbgRead(dbg, body, va, len)) {
                    std::memset(body, 0, len);
                    rec_note(std::snprintf(note, sizeof(note), "dump @ %#llx: DbgRead failed - payload zeroed",
                                           static_cast<unsigned long long>(va)));
                    return nullptr;
                }
                ++ndumped;
                return body;
            };

            /* setups worth watching: magic, and a picture size and profile that
             * make sense (Run D's two heap hits were some other object) */
            static constexpr u32 MaxSetups = 6;
            u64 setup_va[MaxSetups] = {};
            u32 nsetups = 0;
            auto plausible = [](const u8 *p) {
                nvenc_h264_drv_pic_setup_s st;
                std::memcpy(std::addressof(st), p, sizeof(st));
                const u32 w = st.input_cfg.frame_width_minus1 + 1u, h = st.input_cfg.frame_height_minus1 + 1u;
                return st.sps_data.profile_idc != 0 && w >= 16 && w <= 4096 && h >= 16 && h <= 4096;
            };

            /* command-buffer windows: 2 KB from 0x200 before each hit, at most
             * two per region and twelve in all */
            struct Win { u64 va; u32 len; u32 hit_off; };
            static constexpr u32 MaxWins = 12;
            Win wins[MaxWins] = {};
            u32 nwins = 0;
            /* scan-time dump budgets, so a process full of magic-bearing
             * objects cannot use up the record buffer before the command
             * buffers and the watch get their turn */
            u32 nmagic_dumps = 0, nsetcl_dumps = 0;
            static constexpr u32 MaxMagicDumps = 8, MaxSetclDumps = 4;

            u64 addr = 0;
            for (u32 steps = 0; steps < 8000; ++steps) {
                ::ams::svc::MemoryInfo mi = {};
                ::ams::svc::PageInfo   pi = {};
                if (!DbgQuery(dbg, std::addressof(mi), std::addressof(pi), addr)) { break; }
                if (mi.size == 0) { break; }

                const u32 perm  = static_cast<u32>(mi.permission);
                const u32 state = static_cast<u32>(mi.state);
                const bool readable = (perm & ::ams::svc::MemoryPermission_Read) != 0;
                /* Code and stack are not where a driver config struct lives. */
                const bool interesting = readable && mi.size >= 0x1000 &&
                                         state != static_cast<u32>(::ams::svc::MemoryState_Free) &&
                                         state != static_cast<u32>(::ams::svc::MemoryState_Code) &&
                                         state != static_cast<u32>(::ams::svc::MemoryState_AliasCode);

                if (interesting && mi.size > MaxRegion) {
                    ++nskipped;
                    skipped_bytes += mi.size;
                    rec_note(std::snprintf(note, sizeof(note), "skipped region %#llx+%#llx state %#x perm %#x (over 8 MB)",
                                           static_cast<unsigned long long>(mi.base_address),
                                           static_cast<unsigned long long>(mi.size), state, perm));
                } else if (interesting && scanned < ScanCap) {
                    ++nregions;
                    u32 region_wins = 0;
                    const u64 region_end = mi.base_address + mi.size;
                    for (u64 off = 0; off < mi.size && scanned < ScanCap; off += ChunkSize) {
                        const u64 n = (mi.size - off < ChunkSize) ? (mi.size - off) : ChunkSize;
                        if (!DbgRead(dbg, g_ind_buf, mi.base_address + off, n)) { break; }
                        scanned += n;
                        const u32 *w = reinterpret_cast<const u32 *>(g_ind_buf);
                        const u64 nw = n / 4;
                        for (u64 i = 0; i + 4 < nw; ++i) {
                            u32 kind = 0;
                            for (u32 m = 0; m < 4; ++m) { if (w[i] == NvencMagics[m]) { kind = 1 + m; } }
                            /* M78: a SETCL counts only if the next word writes the
                             * method-offset register (0x10) with INCR, NONINCR or
                             * MASK. */
                            const u32 nx = w[i + 1] >> 16;
                            /* M81: or a write to THI register 0x0B, which is what
                             * grc's NVENC jobs do right after SETCL (Run E) */
                            const bool method_next = nx == 0x1010 || nx == 0x2010 || nx == 0x3010 || nx == 0x100b;
                            if (kind == 0 && w[i] == setcl_nvenc && method_next) { kind = Hit_SetclEnc; }
                            if (kind == 0 && w[i] == setcl_nvjpg && method_next) { kind = Hit_SetclJpg; }
                            if (kind == 0 && w[i] == setcl_vic   && method_next) { kind = Hit_SetclVic; }
                            if (kind == 0 && i + 11 < nw) {
                                /* nvenc_pic_stat_s: 3600 MBs (720p), pic_type 0-3,
                                 * 1-64 slices, a bit count that fits the buffer */
                                const u32 mbs = (w[i + 10] & 0xFFFF) + (w[i + 10] >> 16);
                                const u32 pt = w[i + 4] & 0xFFFF, ns = w[i + 4] >> 16;
                                if (mbs == 3600 && pt <= 3 && ns >= 1 && ns <= 64 &&
                                    w[i + 2] != 0 && w[i + 2] <= 1382400u * 8 && w[i + 9] <= 1382400u) {
                                    kind = Hit_Status;
                                    ++status_err_hist[w[i + 1] & 3];
                                    if ((w[i + 1] >> 2) != 0) { ++status_ucode_nonzero; }
                                    if (nkind[Hit_Status] < 32) rec_note(std::snprintf(note, sizeof(note),
                                             "grc status @ %#llx: picture_index %#x error_status %u ucode %#x bits %u pic_type %u slices %u avgQP %u intra/inter %u/%u",
                                             static_cast<unsigned long long>(mi.base_address + off + i * 4),
                                             w[i], w[i + 1] & 3, w[i + 1] >> 2, w[i + 2], pt, ns, w[i + 5] >> 16,
                                             w[i + 10] & 0xFFFF, w[i + 10] >> 16));
                                }
                            }
                            const u32 op = w[i] >> 16;
                            if (kind == 0 && (((op == 0x1010 || op == 0x2010 || op == 0x3010) && w[i + 1] == MethodDrvPicSetup) ||
                                              w[i] == (0x40100000u | MethodDrvPicSetup))) {
                                kind = Hit_Method;
                            }
                            if (kind == 0) { continue; }
                            ++nkind[kind];
                            const u64 hit_addr = mi.base_address + off + i * 4;
                            /* Run E had 326 method writes; list only the first 8 so
                             * the other kinds still get a line each */
                            const bool list_it = kind != Hit_Method || nkind[Hit_Method] <= 8;
                            if (list_it && nlogged < MaxLogged) { hits[nlogged++] = { hit_addr, kind, w[i], w[i+1], w[i+2], w[i+3] }; }

                            if (kind <= 4 && nmagic_dumps < MaxMagicDumps) {
                                ++nmagic_dumps;
                                /* 4 KB of setup: the 512-byte header plus the
                                 * control arrays pic_control points into */
                                u32 want = 0x1000u;
                                if (hit_addr + want > region_end) { want = static_cast<u32>(region_end - hit_addr) & ~3u; }
                                rec_note(std::snprintf(note, sizeof(note), "setup magic @ %#llx in region %#llx+%#llx state %#x perm %#x",
                                                       static_cast<unsigned long long>(hit_addr),
                                                       static_cast<unsigned long long>(mi.base_address),
                                                       static_cast<unsigned long long>(mi.size), state, perm));
                                const u8 *body = dump(8, 0, hit_addr, want);
                                if (body != nullptr && want >= sizeof(nvenc_h264_drv_pic_setup_s) && plausible(body) && nsetups < MaxSetups) {
                                    setup_va[nsetups++] = hit_addr;
                                }
                            } else if (kind == Hit_SetclEnc && nsetcl_dumps < MaxSetclDumps) {
                                ++nsetcl_dumps;
                                u32 want = 0x400u;
                                if (hit_addr + want > region_end) { want = static_cast<u32>(region_end - hit_addr) & ~3u; }
                                rec_note(std::snprintf(note, sizeof(note), "NVENC SETCL @ %#llx", static_cast<unsigned long long>(hit_addr)));
                                dump(7, 0, hit_addr, want);
                            } else if (kind == Hit_SetclVic && nvic_dumps < MaxVicDumps) {
                                /* M81: does grc drive the VIC too? A working VIC job
                                 * would show how the RGB -> YUV matrix is really set */
                                ++nvic_dumps;
                                u32 want = 0x400u;
                                if (hit_addr + want > region_end) { want = static_cast<u32>(region_end - hit_addr) & ~3u; }
                                rec_note(std::snprintf(note, sizeof(note), "VIC SETCL @ %#llx in region %#llx+%#llx state %#x",
                                                       static_cast<unsigned long long>(hit_addr),
                                                       static_cast<unsigned long long>(mi.base_address),
                                                       static_cast<unsigned long long>(mi.size), state));
                                dump(7, 0, hit_addr, want);
                            } else if (kind == Hit_Method && region_wins < 2 && nwins < MaxWins) {
                                bool covered = false;
                                for (u32 k = 0; k < nwins; ++k) {
                                    if (hit_addr >= wins[k].va && hit_addr < wins[k].va + wins[k].len) { covered = true; }
                                }
                                if (!covered) {
                                    const u64 ws = (hit_addr - mi.base_address > 0x200) ? hit_addr - 0x200 : mi.base_address;
                                    u32 wl = 0x800u;
                                    if (ws + wl > region_end) { wl = static_cast<u32>(region_end - ws) & ~3u; }
                                    wins[nwins] = { ws, wl, static_cast<u32>(hit_addr - ws) };
                                    rec_note(std::snprintf(note, sizeof(note), "SET_IN_DRV_PIC_SETUP write @ %#llx (+%#x in the dump) in region %#llx+%#llx state %#x",
                                                           static_cast<unsigned long long>(hit_addr), wins[nwins].hit_off,
                                                           static_cast<unsigned long long>(mi.base_address),
                                                           static_cast<unsigned long long>(mi.size), state));
                                    dump(7, wins[nwins].hit_off, ws, wl);
                                    ++nwins;
                                    ++region_wins;
                                }
                            }
                        }
                    }
                }

                const u64 next = mi.base_address + mi.size;
                if (next <= addr) { break; }
                addr = next;
            }

            /* ---- M79: watch grc's setup ring --------------------------------
             * Run D caught three P-frame setups (frame_num 1 and 2). The first
             * job we replay should be an intra frame, which needs no reference
             * picture, so poll the plausible setups for up to three seconds and
             * dump each new (slot, frame_num, pic_type), always keeping intra
             * ones. At 30 fps and a GOP of 15 an intra frame comes every
             * 0.5 s. When the first one shows up, the command-buffer windows
             * are dumped again at that same moment. grc runs throughout; each
             * pass drains any debug event so nothing is left halted. */
            u32 polls = 0, extra_setups = 0, extra_nonintra = 0, nev_watch = 0;
            bool intra_seen = false, cmd_redumped = false;
            u32 seen_n = 0;
            struct Seen { u32 slot; u32 frame_num; u32 pic_type; };
            Seen seen[48] = {};
            const u64 w0 = armTicksToNs(armGetSystemTick());
            if (nsetups > 0) {
                alignas(8) u8 hdr_buf[sizeof(nvenc_h264_drv_pic_setup_s)];
                while (armTicksToNs(armGetSystemTick()) - w0 < UINT64_C(3000000000)) {
                    ++polls;
                    {
                        ::ams::svc::DebugEventInfo ev;
                        u32 n = 0;
                        while (n < 64 && R_SUCCEEDED(::ams::svc::GetDebugEvent(std::addressof(ev), dbg))) { ++n; }
                        if (n != 0) {
                            nev_watch += n;
                            static_cast<void>(::ams::svc::ContinueDebugEvent(dbg,
                                ::ams::svc::ContinueFlag_ExceptionHandled | ::ams::svc::ContinueFlag_ContinueAll, nullptr, 0));
                        }
                    }
                    for (u32 k = 0; k < nsetups; ++k) {
                        if (!DbgRead(dbg, hdr_buf, setup_va[k], sizeof(hdr_buf))) { continue; }
                        nvenc_h264_drv_pic_setup_s st;
                        std::memcpy(std::addressof(st), hdr_buf, sizeof(st));
                        const u32 fn = st.pic_control.frame_num, pt = st.pic_control.pic_type;
                        bool known = false;
                        for (u32 j = 0; j < seen_n; ++j) {
                            if (seen[j].slot == k && seen[j].frame_num == fn && seen[j].pic_type == pt) { known = true; }
                        }
                        if (known) { continue; }
                        if (seen_n < 48) { seen[seen_n++] = { k, fn, pt }; }
                        const bool intra = pt >= 2;   /* 2 = I, 3 = IDR */
                        if (extra_setups >= 8 || (!intra && extra_nonintra >= 3)) { continue; }
                        rec_note(std::snprintf(note, sizeof(note), "watch +%llu ms: slot %u @ %#llx frame_num %u pic_type %u%s",
                                               static_cast<unsigned long long>((armTicksToNs(armGetSystemTick()) - w0) / 1000000),
                                               k, static_cast<unsigned long long>(setup_va[k]), fn, pt, intra ? " (INTRA)" : ""));
                        if (dump(8, 0, setup_va[k], 0x1000) != nullptr) {
                            ++extra_setups;
                            if (!intra) { ++extra_nonintra; }
                        }
                        if (intra && !cmd_redumped) {
                            rec_note(std::snprintf(note, sizeof(note), "intra frame seen: command-buffer windows dumped again now"));
                            for (u32 j = 0; j < nwins; ++j) { dump(7, wins[j].hit_off, wins[j].va, wins[j].len); }
                            cmd_redumped = true;
                        }
                        intra_seen = intra_seen || intra;
                    }
                    if (intra_seen && extra_nonintra >= 2) { break; }
                    os::SleepThread(TimeSpan::FromMilliSeconds(5));
                }
            }
            const u64 watch_ms = (armTicksToNs(armGetSystemTick()) - w0) / 1000000;

            static_cast<void>(::ams::svc::CloseHandle(dbg));
            LogLine("   scanned %llu KB across %u regions; skipped %u region(s) over 8 MB (%llu KB)%s",
                    static_cast<unsigned long long>(scanned / 1024), nregions, nskipped,
                    static_cast<unsigned long long>(skipped_bytes / 1024),
                    scanned >= ScanCap ? "  (hit the 256 MB cap)" : "");
            LogLine("   hits: setup magic %u, NVENC SETCL %u, NVJPG SETCL %u, VIC SETCL %u, SET_IN_DRV_PIC_SETUP writes %u, NVENC status blocks %u",
                    nkind[1] + nkind[2] + nkind[3] + nkind[4], nkind[Hit_SetclEnc], nkind[Hit_SetclJpg],
                    nkind[Hit_SetclVic], nkind[Hit_Method], nkind[Hit_Status]);
            LogLine("   grc's own NVENC status blocks: error_status 0:%u 1:%u 2:%u 3:%u, ucode_error_status non-zero in %u",
                    status_err_hist[0], status_err_hist[1], status_err_hist[2], status_err_hist[3], status_ucode_nonzero);

            static const char *KindName[NumKinds] = { "?", "NVENC magic 5.0", "NVENC magic 6.0",
                                                      "NVENC magic 1.0", "MSENC magic 2.0",
                                                      "SETCL class 0x21 (NVENC)", "SETCL class 0xC0 (NVJPG)",
                                                      "SET_IN_DRV_PIC_SETUP write", "SETCL class 0x5D (VIC)",
                                                      "NVENC status block" };
            for (u32 i = 0; i < nlogged; ++i) {
                LogLine("   hit %2u @ %#llx  %-26s  %08x %08x %08x %08x", i,
                        static_cast<unsigned long long>(hits[i].addr),
                        KindName[hits[i].kind < NumKinds ? hits[i].kind : 0],
                        hits[i].word0, hits[i].word1, hits[i].word2, hits[i].word3);
            }
            LogLine("   plausible setups %u, command-buffer windows %u; watch: %u polls in %llu ms, %u new setups dumped, intra %s, %u debug events",
                    nsetups, nwins, polls, static_cast<unsigned long long>(watch_ms), extra_setups,
                    intra_seen ? "CAPTURED" : "not seen", nev_watch);
            if (nkind[1] + nkind[2] + nkind[3] + nkind[4] + nkind[Hit_Method] == 0) {
                LogLine("   nothing found. Either grc is idle and has not allocated its encoder");
                LogLine("   buffers, or they are in memory this process cannot read.");
            }
            if (!can_dump) {
                LogLine("   no cached stage buffer - hits logged above, nothing dumped");
            } else if (rec_len != 0) {
                LogLine("   dumped %u block(s) into %zu B of records -> sdmc:/grc-scan.bin (decode: tools/nvrec.py)", ndumped, rec_len);
                WriteSdVerified("sdmc:/grc-scan.bin", rec, rec_len, rec + RecCap, nullptr);
            }
            VicStage("grcscan:done");
        }

                void VicWorkerThread(void *) {
            bool ui_started = false;
            for (;;) {
                os::SleepThread(TimeSpan::FromMilliSeconds(200));
                UsbFollowStreamSwitch();
                /* v0.7.14: with home_menu = 1 the engines do not wait for the
                 * first game's present - the HOME menu streams from boot
                 * (test2: nothing until a game was opened) */
                if (!ui_started && g_ui_stream_armed && g_live_armed && g_vic_pending.load(std::memory_order_acquire) < 0 &&
                    armTicksToNs(armGetSystemTick()) >= static_cast<u64>(g_probe_delay_s) * UINT64_C(1000000000)) {
                    ui_started = true;
                    LogLine("live: home_menu - starting without waiting for a game");
                    g_last_slot = 0;
                    RunVicBlit(0);
                    while (g_rearm.exchange(false)) { RunVicBlit(g_last_slot); }
                }
                const s32 slot = g_vic_pending.exchange(-1, std::memory_order_acq_rel);
                if (slot >= 0) {
                    g_last_slot = slot;
                    RunVicBlit(slot);
                }
                while (g_rearm.exchange(false)) { RunVicBlit(g_last_slot); }
            }
        }

    }

    void StartVicWorker() {
        if (!g_vic_armed) { return; }
        /* M60b: deliberately LOWER priority than main/IPC (higher number is
         * lower priority on Horizon). All our threads share core 3, so a worker
         * at equal priority round-robins against the IPC thread and the game
         * blocks on binder calls we never get scheduled to answer. Below it,
         * IPC preempts us the moment a request arrives and the worker simply
         * uses what is left. */
        const s32 worker_prio = os::GetThreadPriority(os::GetCurrentThread()) + 4;
        SFT_ABORT_UNLESS(os::CreateThread(std::addressof(g_vic_thread), VicWorkerThread, nullptr,
                                        g_vic_stack, sizeof(g_vic_stack), worker_prio));
        os::SetThreadNamePointer(std::addressof(g_vic_thread), "applet-mitm.VIC");
        os::StartThread(std::addressof(g_vic_thread));
        NoteThread(std::addressof(g_vic_thread));
        g_vic_stage.store("waiting", std::memory_order_relaxed);
        LogLine("VIC worker started (execute=%s)", g_vic_execute ? "PhaseB-full-blit" : "PhaseA-noop-cmdbuf");
    }

    void RequestVicBlit(s32 slot) {
        /* binder thread: hand off and get out. */
        g_vic_pending.store(slot, std::memory_order_release);
    }

}
