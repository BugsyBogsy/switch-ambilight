/*
 * v0.7.14 research probe: what is on screen outside the game (HOME menu,
 * applets), toward streaming the whole system. Off unless
 * sdmc:/config/switch-frame-tap/probe-ui.txt exists; a word "no<part>" in it
 * turns one part off (nomap, nodispdrv, nobinders).
 *
 * probe1 (logs/v0714-probe1): the HOME menu, the user picker and the
 * controller applet have no frame buffers of their own - none of their
 * memory is mapped by two devices (GPU and display), while the overlay and
 * the Album each have 2 x 1280x768x4 that are. Their frames are in a buffer
 * someone else owns (the system shared buffer, vi's "fbshare").
 *
 * probe2 (this):
 *  map      every process's large regions (>= 2 MB), at idle and when a
 *           HOME-menu process or applet starts - where that buffer lives
 *  dispdrv  the applets' own frame traffic (applet_mitm_uibinder.cpp): their
 *           slots, crops, and the NvGraphicBuffer of each slot
 *  binders  4 minutes after boot, from our own dispdrv session: which binder
 *           ids answer "query" (read-only), the HOME menu's among them, its
 *           buffer history (code 17), and requestBuffer on its slots - which
 *           only succeeds while the slot is dequeued, i.e. being drawn
 *
 * probe2 result (logs/v0714-probe2): the buffer is in vi's .bss, 26,880 KB =
 * 7 slots of 1280x768x4, device count 2, the same address all boot; the
 * dispdrv mitm stayed off (dispdrv's pointer buffer is 0x3000); every
 * binder answered query with a bare 4-byte status.
 *
 * probe3 result (logs/v0714-probe3): attaching to vi costs nothing (vi
 * resumed 39 us after the attach); 26.9 MB read in 18-25 ms; the HOME menu
 * and the controller applet both alternate slots 3 and 4 at 60 Hz; slot 6 is
 * the boot logo, 0-2 capture buffers; reading a slot over 160 ms tears. No
 * applet opened dispdrv. Every binder query answered alike (448, 720).
 *
 * probe4 adds (binders now opt-in with the word "binders"):
 *  vi       library applets' vi:s and vi:m wrapped: Acquire/PresentSharedFrameBuffer
 *  corr     with an applet open, vi's writable memory searched for the words
 *           that follow the presented slot; then checked on the HOME menu
 *
 * probe3 adds:
 *  snap     vi attached as a debugger for a moment, its 7 slots hashed in 12
 *           passes ~20 ms apart (which slots are being drawn) and written to
 *           sdmc:/config/switch-frame-tap/probe/vi-<home|applet>-slotN.raw -
 *           on the HOME menu alone 75 s in, and with a library applet open 4 s
 */
#include <stratosphere.hpp>
#include <cctype>
#include <cstdio>
#include <cstring>
#include "applet_mitm_log.hpp"
#include "applet_mitm_nv.hpp"
#include "applet_mitm_service.hpp"
#include "applet_mitm_gbuf.hpp"
#include "applet_mitm_uibinder.hpp"

/* v0.7.14: research only - left out of release builds (SFT_UI_PROBE,
 * applet_mitm_uibinder.hpp) */
#if !SFT_UI_PROBE

namespace ams::mitm::applet {

    bool UiProbeOn() { return false; }
    bool UiProbeWants(const char *) { return false; }
    /* only its pointer buffer size was wanted */
    void UiProbeSetDispdrv(const ::Service &s) { ::Service c = s; ::serviceClose(std::addressof(c)); }
    void UiProbeStep() { }

}

#else

namespace ams::mitm::applet {

    namespace {

        constinit bool g_probe_on = false;
        constinit bool g_probe_checked = false;
        constinit char g_probe_words[256] = {};
        constinit u64 g_next_ns = 0;
        constinit u64 g_dump_at_ns = 0;     /* a map dump due (a moment after a change) */
        constinit const char *g_dump_why = "";
        constinit u32 g_dumps = 0;
        constinit u64 g_last_app = ~UINT64_C(0);
        constinit u64 g_next_summary_ns = 0;

        struct Proc { u64 pid; u64 tid; };
        constinit Proc g_prev[96] = {};
        constinit u32 g_prev_n = 0;

        void ReadConfig() {
            if (g_probe_checked) { return; }
            g_probe_checked = true;
            fs::FileHandle f;
            if (R_FAILED(fs::OpenFile(std::addressof(f), "sdmc:/config/switch-frame-tap/probe-ui.txt", fs::OpenMode_Read))) { return; }
            size_t n = 0;
            static_cast<void>(fs::ReadFile(std::addressof(n), f, 0, g_probe_words, sizeof(g_probe_words) - 1));
            fs::CloseFile(f);
            g_probe_words[n < sizeof(g_probe_words) ? n : sizeof(g_probe_words) - 1] = 0;
            g_probe_on = true;
        }

        /* whole words only (a substring once armed a dangerous feature) */
        bool HasWord(const char *w) {
            const size_t wl = std::strlen(w);
            for (const char *p = g_probe_words; (p = std::strstr(p, w)) != nullptr; p += wl) {
                const bool start = p == g_probe_words || !std::isalnum(static_cast<unsigned char>(p[-1]));
                const bool end = !std::isalnum(static_cast<unsigned char>(p[wl]));
                if (start && end) { return true; }
            }
            return false;
        }

        const char *AppletName(u64 tid) {
            switch (tid) {
                case 0x0100000000001000: return "HOME menu (qlaunch)";
                case 0x0100000000001001: return "auth";
                case 0x0100000000001002: return "cabinet (amiibo)";
                case 0x0100000000001003: return "controller";
                case 0x0100000000001004: return "dataErase";
                case 0x0100000000001005: return "error";
                case 0x0100000000001006: return "netConnect";
                case 0x0100000000001007: return "user picker";
                case 0x0100000000001008: return "keyboard";
                case 0x0100000000001009: return "miiEdit";
                case 0x010000000000100A: return "web";
                case 0x010000000000100B: return "shop";
                case 0x010000000000100C: return "overlay (overlayDisp)";
                case 0x010000000000100D: return "Album (photoViewer)";
                case 0x010000000000100F: return "offlineWeb";
                case 0x0100000000001010: return "loginShare";
                case 0x0100000000001012: return "starter";
                case 0x0100000000001013: return "myPage";
                default: return nullptr;
            }
        }

        const char *ProcName(u64 tid) {
            switch (tid) {
                case 0x0100000000000000: return "fs";
                case 0x0100000000000003: return "pm";
                case 0x0100000000000004: return "sm";
                case 0x0100000000000012: return "bsdsockets";
                case 0x0100000000000013: return "hid";
                case 0x0100000000000019: return "nvservices";
                case 0x010000000000001C: return "nvnflinger";
                case 0x010000000000001F: return "ns";
                case 0x0100000000000022: return "capsrv";
                case 0x0100000000000023: return "am";
                case 0x0100000000000024: return "ssl";
                case 0x010000000000002D: return "vi";
                case 0x0100000000000035: return "grc";
                case 0x0100000000000C20: return "SFT";
                case 0x0100000000000C21: return "SFT-donor";
                default: return AppletName(tid);
            }
        }

        u64 ProgramOf(u64 pid, os::NativeHandle *out_h) {
            os::NativeHandle h = os::InvalidNativeHandle;
            ncm::ProgramLocation loc = {};
            cfg::OverrideStatus st = {};
            if (R_FAILED(::ams::pm::dmnt::AtmosphereGetProcessInfo(std::addressof(h), std::addressof(loc), std::addressof(st), os::ProcessId{pid}))) { return 0; }
            if (out_h != nullptr) { *out_h = h; } else { os::CloseNativeHandle(h); }
            return loc.program_id.value;
        }

        /* every process but the game: regions of 2 MB and up, and for the
         * HOME menu and applets any device-mapped one of 512 KB and up */
        void DumpMaps(const char *why) {
            static u64 pids[96];
            s32 n = 0;
            if (R_FAILED(::svcGetProcessList(&n, pids, 96))) { return; }
            const u64 app = g_app_pid.load(std::memory_order_relaxed);
            LogLine("uiprobe: memory map (%s): %d processes, regions >= 2 MB (type/perm/attr/devices/ipc)", why, n);
            u32 lines = 0;
            for (s32 i = 0; i < n && lines < 160; ++i) {
                if (pids[i] == app) { continue; }
                os::NativeHandle h = os::InvalidNativeHandle;
                const u64 tid = ProgramOf(pids[i], std::addressof(h));
                if (tid == 0) { continue; }
                const bool ui = IsUiProgram(tid);
                const char *nm = ProcName(tid);
                u64 addr = 0;
                for (u32 guard = 0; guard < 4096 && lines < 160; ++guard) {
                    ::MemoryInfo mi = {};
                    u32 pi = 0;
                    if (R_FAILED(::svcQueryProcessMemory(&mi, &pi, h, addr)) || mi.size == 0) { break; }
                    const u32 type = mi.type & 0xFF;
                    const bool big = mi.size >= 2 * 1024 * 1024 && type != 0 && type != 0x10;
                    const bool dev = ui && mi.device_refcount > 0 && mi.size >= 512 * 1024;
                    if (big || dev) {
                        ++lines;
                        LogLine("map %-14s %#llx %7llu KB type %#04x perm %u attr %#x dev %u ipc %u",
                                nm != nullptr ? nm : "", static_cast<unsigned long long>(mi.addr), static_cast<unsigned long long>(mi.size / 1024),
                                mi.type, mi.perm, mi.attr, mi.device_refcount, mi.ipc_refcount);
                        if (nm == nullptr) { LogLine("    (that is %016llx, pid %llu)", static_cast<unsigned long long>(tid), static_cast<unsigned long long>(pids[i])); }
                    }
                    const u64 next = mi.addr + mi.size;
                    if (next <= addr) { break; }
                    addr = next;
                }
                os::CloseNativeHandle(h);
            }
            LogLine("uiprobe: memory map done, %u lines", lines);
        }

        /* ---- our own dispdrv session ---------------------------------- */

        constinit ::Service g_dd = {};
        constinit bool g_dd_ok = false;
        alignas(0x10) constinit u8 g_tx_in[0x200] = {};
        alignas(0x10) constinit u8 g_tx_out[0x400] = {};

        /* an IGraphicBufferProducer parcel: header, interface token, args */
        size_t BuildParcel(const s32 *args, u32 nargs) {
            static constexpr char Token[] = "android.gui.IGraphicBufferProducer";
            constexpr u32 TokenLen = sizeof(Token) - 1;   /* 34 */
            std::memset(g_tx_in, 0, sizeof(g_tx_in));
            u8 *p = g_tx_in + 0x10;
            u32 off = 0;
            const u32 policy = 0x100;
            std::memcpy(p + off, std::addressof(policy), 4); off += 4;
            std::memcpy(p + off, std::addressof(TokenLen), 4); off += 4;
            for (u32 i = 0; i < TokenLen; ++i) { const u16 c = static_cast<u8>(Token[i]); std::memcpy(p + off + 2 * i, std::addressof(c), 2); }
            off += ((TokenLen + 1) * 2 + 3) & ~3u;
            for (u32 i = 0; i < nargs; ++i) { std::memcpy(p + off, args + i, 4); off += 4; }
            const u32 hdr[4] = { off, 0x10, 0, 0x10 + off };
            std::memcpy(g_tx_in, hdr, sizeof(hdr));
            return 0x10 + off;
        }

        /* out: the reply's payload and its length */
        Result OwnTransact(s32 id, u32 code, const s32 *args, u32 nargs, const u8 **out_payload, u32 *out_len) {
            const size_t n = BuildParcel(args, nargs);
            std::memset(g_tx_out, 0, sizeof(g_tx_out));
            const struct { s32 id; u32 code; u32 flags; } in = { id, code, 0 };
            const ::Result rc = serviceDispatchIn(std::addressof(g_dd), 3, in,
                .buffer_attrs = { SfBufferAttr_HipcAutoSelect | SfBufferAttr_In, SfBufferAttr_HipcAutoSelect | SfBufferAttr_Out },
                .buffers      = { { g_tx_in, n }, { g_tx_out, sizeof(g_tx_out) } },
            );
            u32 hdr[4];
            std::memcpy(hdr, g_tx_out, sizeof(hdr));
            if (hdr[1] > sizeof(g_tx_out) || hdr[0] > sizeof(g_tx_out) - hdr[1]) { hdr[0] = 0; hdr[1] = 0x10; }
            *out_payload = g_tx_out + hdr[1];
            *out_len = hdr[0];
            R_RETURN(::ams::Result(rc));
        }

        s32 Word(const u8 *p, u32 len, u32 i) {
            s32 v = 0;
            if ((i + 1) * 4 <= len) { std::memcpy(std::addressof(v), p + 4 * i, 4); }
            return v;
        }

        void Hex(const char *tag, const u8 *p, u32 len) {
            char hex[3 * 64 + 1] = {};
            const u32 n = len < 64 ? len : 64;
            for (u32 i = 0; i < n; ++i) { std::snprintf(hex + 3 * i, 4, "%02x ", p[i]); }
            LogLine("   %s (%u bytes): %s%s", tag, len, hex, len > n ? "..." : "");
        }

        /* the binders that answered query with a size, and what requestBuffer did */
        struct Cand {
            s32 id;
            s32 w, h, fmt;
            u32 ok[4], bad[4];
            bool logged[4];
        };
        constinit Cand g_cands[8] = {};
        constinit u32 g_ncands = 0;
        constinit u32 g_binder_stage = 0;   /* 0 waiting, 1 racing, 2 done */
        constinit u32 g_race_steps = 0;

        bool SeenInMitm(s32 id) {
            static UiBinderInfo b[16];
            const u32 n = UiBinderSnapshot(b, 16);
            for (u32 i = 0; i < n; ++i) { if (b[i].id == id) { return true; } }
            return false;
        }

        void Enumerate() {
            static UiBinderInfo b[16];
            const u32 nb = UiBinderSnapshot(b, 16);
            s32 top = 24;
            for (u32 i = 0; i < nb; ++i) { if (b[i].id + 8 > top) { top = b[i].id + 8; } }
            if (top > 64) { top = 64; }
            LogLine("uiprobe: binders 1..%d from our own dispdrv session (query: read-only)", top);
            for (s32 id = 1; id <= top; ++id) {
                const u8 *p = nullptr;
                u32 len = 0;
                s32 a = 0;   /* NATIVE_WINDOW_WIDTH */
                const Result rw = OwnTransact(id, 9, std::addressof(a), 1, std::addressof(p), std::addressof(len));
                const s32 w = Word(p, len, 0), sw = Word(p, len, 1);
                if (R_FAILED(rw) || len < 8) {
                    /* probe2: every id 1..24 answered with 4 bytes - the status alone */
                    LogLine("   binder %d: query rc=0x%x reply %u bytes, status %d", id, rw.GetValue(), len, Word(p, len, 0));
                    if (id == 1 && R_SUCCEEDED(rw)) { Hex("reply parcel", g_tx_out, 0x20); }
                    continue;
                }
                a = 1;   /* HEIGHT */
                static_cast<void>(OwnTransact(id, 9, std::addressof(a), 1, std::addressof(p), std::addressof(len)));
                const s32 h = Word(p, len, 0);
                a = 2;   /* FORMAT */
                static_cast<void>(OwnTransact(id, 9, std::addressof(a), 1, std::addressof(p), std::addressof(len)));
                const s32 fmt = Word(p, len, 0);
                LogLine("   binder %d: %dx%d format %d (status %d)%s", id, w, h, fmt, sw, SeenInMitm(id) ? " - an applet's, seen by the mitm" : "");
                if (sw == 0 && w > 0 && h > 0 && g_ncands < 8) {
                    g_cands[g_ncands++] = { id, w, h, fmt, {}, {}, {} };
                    /* the buffer history: no slot in it per the emulators - see what the console says */
                    s32 cnt = 8;
                    const Result rh = OwnTransact(id, 17, std::addressof(cnt), 1, std::addressof(p), std::addressof(len));
                    LogLine("   binder %d: getBufferHistory(8) rc=0x%x", id, rh.GetValue());
                    if (R_SUCCEEDED(rh)) { Hex("history", p, len); }
                }
            }
        }

        /* one sample per candidate per step: which slots are dequeued now */
        void RaceStep() {
            for (u32 c = 0; c < g_ncands; ++c) {
                Cand &k = g_cands[c];
                for (s32 slot = 0; slot < 4; ++slot) {
                    const u8 *p = nullptr;
                    u32 len = 0;
                    const Result rc = OwnTransact(k.id, 1, std::addressof(slot), 1, std::addressof(p), std::addressof(len));
                    const auto *gb = R_SUCCEEDED(rc) ? FindGraphicBuffer(p, len) : nullptr;
                    if (gb != nullptr) {
                        ++k.ok[slot];
                        if (!k.logged[slot]) {
                            k.logged[slot] = true;
                            char tag[64];
                            std::snprintf(tag, sizeof(tag), "binder %d slot %d (our requestBuffer)", k.id, slot);
                            LogGraphicBuffer(tag, gb);
                        }
                    } else {
                        ++k.bad[slot];
                        if (k.bad[slot] == 1) { LogLine("   binder %d slot %d: requestBuffer rc=0x%x, reply %u bytes, status %d", k.id, slot, rc.GetValue(), len, Word(p, len, len >= 4 ? len / 4 - 1 : 0)); }
                    }
                }
            }
        }

        /* ---- probe3: vi's system shared buffer ------------------------
         * probe2: vi holds one 26,880 KB region of .bss mapped by GPU and
         * display = 7 x 3,840 KB = 7 slots of 1280x768x4. The HOME menu and
         * the library applets draw into it (they have no buffers of their
         * own). vi is attached as a debugger only for the snapshot - the
         * attach burst drained and continued at once, events polled between
         * chunks - and each slot is written raw to the SD card. */
        constexpr u64 ViProgramId = UINT64_C(0x010000000000002D);
        constexpr size_t ViSlotSize = 1280 * 768 * 4;
        constexpr u32 ViSlots = 7;
        constexpr size_t SnapChunk = 64 * 1024;
        alignas(0x1000) constinit u8 g_snap_buf[SnapChunk] = {};
        constinit bool g_snap_home_done = false;
        constinit bool g_snap_applet_done = false;
        constinit u64 g_applet_since_ns = 0;
        constinit u64 g_applet_tid = 0;

        /* continue whatever the attach or a thread start/exit left pending */
        u32 DrainContinue(::ams::svc::Handle dbg) {
            u32 n = 0;
            ::ams::svc::DebugEventInfo ev;
            while (n < 256 && R_SUCCEEDED(::ams::svc::GetDebugEvent(std::addressof(ev), dbg))) { ++n; }
            if (n != 0) {
                static_cast<void>(::ams::svc::ContinueDebugEvent(dbg, ::ams::svc::ContinueFlag_ExceptionHandled | ::ams::svc::ContinueFlag_ContinueAll, nullptr, 0));
            }
            return n;
        }

        /* the 7-slot region in vi: CodeMutable, two devices, 26,880 KB */
        bool FindViBuffer(u64 *out_pid, u64 *out_addr, u64 *out_size) {
            os::ProcessId pid{};
            if (R_FAILED(::ams::pm::dmnt::GetProcessId(std::addressof(pid), ncm::ProgramId{ViProgramId})) || pid.value == 0) { return false; }
            os::NativeHandle h = os::InvalidNativeHandle;
            if (ProgramOf(pid.value, std::addressof(h)) != ViProgramId) { if (h != os::InvalidNativeHandle) { os::CloseNativeHandle(h); } return false; }
            u64 addr = 0, best = 0, best_size = 0;
            for (u32 guard = 0; guard < 4096; ++guard) {
                ::MemoryInfo mi = {};
                u32 pi = 0;
                if (R_FAILED(::svcQueryProcessMemory(&mi, &pi, h, addr)) || mi.size == 0) { break; }
                if (mi.device_refcount >= 2 && mi.size >= ViSlotSize * 2 && mi.size > best_size) { best = mi.addr; best_size = mi.size; }
                const u64 next = mi.addr + mi.size;
                if (next <= addr) { break; }
                addr = next;
            }
            os::CloseNativeHandle(h);
            *out_pid = pid.value;
            *out_addr = best;
            *out_size = best_size;
            return best != 0;
        }

        u64 Hash(const u8 *p, size_t n) {
            u64 h = 0xcbf29ce484222325ull;
            for (size_t i = 0; i + 8 <= n; i += 256) { u64 v; std::memcpy(std::addressof(v), p + i, 8); h = (h ^ v) * 0x100000001b3ull; }
            return h;
        }

        void SnapVi(const char *tag) {
            u64 pid = 0, base = 0, size = 0;
            if (!FindViBuffer(std::addressof(pid), std::addressof(base), std::addressof(size))) { LogLine("uiprobe: snap %s: vi's shared buffer not found", tag); return; }
            const u32 slots = static_cast<u32>(size / ViSlotSize) < ViSlots ? static_cast<u32>(size / ViSlotSize) : ViSlots;
            LogLine("uiprobe: snap %s: vi pid %llu, shared buffer %#llx, %llu KB = %u slots (+%llu KB)", tag, static_cast<unsigned long long>(pid),
                    static_cast<unsigned long long>(base), static_cast<unsigned long long>(size / 1024), slots,
                    static_cast<unsigned long long>((size - static_cast<u64>(slots) * ViSlotSize) / 1024));
            ::ams::svc::Handle dbg = ::ams::svc::InvalidHandle;
            const u64 t_attach = armTicksToNs(armGetSystemTick());
            const Result ra = ::ams::svc::DebugActiveProcess(std::addressof(dbg), pid);
            if (R_FAILED(ra)) { LogLine("uiprobe: snap %s: DebugActiveProcess(vi) rc=0x%x", tag, ra.GetValue()); return; }
            const u32 burst = DrainContinue(dbg);
            const u64 t_resumed = armTicksToNs(armGetSystemTick());

            /* which slots change: 12 passes ~20 ms apart, a sparse hash of each slot */
            static u64 hashes[12][ViSlots];
            u64 pass_ns[12] = {};
            u32 events = 0, read_fail = 0;
            for (u32 p = 0; p < 12; ++p) {
                const u64 t0 = armTicksToNs(armGetSystemTick());
                for (u32 s = 0; s < slots; ++s) {
                    u64 h = 0;
                    for (size_t off = 0; off < ViSlotSize; off += SnapChunk) {
                        if (R_FAILED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(g_snap_buf), dbg, base + s * ViSlotSize + off, SnapChunk))) { ++read_fail; continue; }
                        h ^= Hash(g_snap_buf, SnapChunk) + off;
                    }
                    hashes[p][s] = h;
                }
                pass_ns[p] = armTicksToNs(armGetSystemTick()) - t0;
                events += DrainContinue(dbg);
                os::SleepThread(TimeSpan::FromMilliSeconds(8));
            }

            /* each slot to the SD card, raw (tools/deswizzle.py --size 1280x720) -
             * probe5: only with the word "files" (probe3 has them) */
            static_cast<void>(fs::CreateDirectory("sdmc:/config/switch-frame-tap/probe"));
            const u64 t_files = armTicksToNs(armGetSystemTick());
            u32 written = 0;
            for (u32 s = 0; s < slots && HasWord("files"); ++s) {
                char path[96];
                std::snprintf(path, sizeof(path), "sdmc:/config/switch-frame-tap/probe/vi-%s-slot%u.raw", tag, s);
                static_cast<void>(fs::DeleteFile(path));
                if (R_FAILED(fs::CreateFile(path, static_cast<s64>(ViSlotSize)))) { LogLine("uiprobe: snap %s: cannot create %s", tag, path); break; }
                fs::FileHandle f;
                if (R_FAILED(fs::OpenFile(std::addressof(f), path, fs::OpenMode_Write))) { break; }
                bool ok = true;
                for (size_t off = 0; off < ViSlotSize && ok; off += SnapChunk) {
                    if (R_FAILED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(g_snap_buf), dbg, base + s * ViSlotSize + off, SnapChunk))) { ++read_fail; std::memset(g_snap_buf, 0, SnapChunk); }
                    ok = R_SUCCEEDED(fs::WriteFile(f, static_cast<s64>(off), g_snap_buf, SnapChunk, fs::WriteOption::None));
                    events += DrainContinue(dbg);
                }
                static_cast<void>(fs::FlushFile(f));
                fs::CloseFile(f);
                if (ok) { ++written; }
            }
            const u64 t_done = armTicksToNs(armGetSystemTick());
            static_cast<void>(::ams::svc::CloseHandle(dbg));

            LogLine("uiprobe: snap %s: attach %u events, vi resumed after %llu us; hash passes %llu..%llu us each (%u slots = %llu KB); files %u in %llu ms; %u events while attached, %u failed reads; detached",
                    tag, burst, static_cast<unsigned long long>((t_resumed - t_attach) / 1000),
                    static_cast<unsigned long long>(pass_ns[1] / 1000), static_cast<unsigned long long>(pass_ns[11] / 1000), slots,
                    static_cast<unsigned long long>(slots * ViSlotSize / 1024), written, static_cast<unsigned long long>((t_done - t_files) / 1000000), events, read_fail);
            for (u32 s = 0; s < slots; ++s) {
                char line[12 * 6 + 1] = {};
                u32 changes = 0;
                for (u32 p = 0; p < 12; ++p) {
                    const bool ch = p > 0 && hashes[p][s] != hashes[p - 1][s];
                    changes += ch;
                    std::snprintf(line + 6 * p, 7, "%04x%s", static_cast<unsigned>(hashes[p][s] & 0xFFFF), ch ? "* " : "  ");
                }
                LogLine("   slot %u: changed %2u of 11 passes: %s", s, changes, line);
            }
        }

        /* ---- probe4: find vi's own record of the presented slot ---------
         * probe3: the HOME menu and the controller applet both draw into
         * slots 3 and 4, alternating at 60 Hz; reading the slot being drawn
         * tears. The applet's presents are now seen (vi wrapped for library
         * applets), so while it presents, vi's writable memory is searched
         * for words that follow the presented slot: each 64 KB chunk read on
         * one side of a present and again on the other (a diff gives
         * candidates), then 40 more reads keep only the words whose value
         * always matches the last present's parity. vi keeps that state for
         * the slots, not for one applet - so the same words should say which
         * of the HOME menu's slots is finished, without its IPC. */
        alignas(0x1000) constinit u8 g_snap_buf2[SnapChunk] = {};
        struct Region { u64 addr, size; };
        struct CorrCand { u32 reg; u32 off; u32 v[2]; u8 hits[2]; u8 misses; u8 pad; };
        constexpr u32 MaxCands = 4096;
        constinit CorrCand g_corr[MaxCands] = {};
        constinit u32 g_ncorr = 0;
        constexpr u32 MaxRegs = 48;
        constinit Region g_regs[MaxRegs] = {};
        constinit u32 g_nregs = 0;
        constinit u32 g_survivors[16] = {};
        constinit u32 g_nsurv = 0;
        constinit bool g_verify_done = false;
        constinit u64 g_home_alone_since = 0;

        bool WaitPresent(u32 since, u32 timeout_ms) {
            for (u32 t = 0; t < timeout_ms; ++t) {
                if (g_shared_present.count.load(std::memory_order_acquire) != since) { return true; }
                os::SleepThread(TimeSpan::FromMilliSeconds(1));
            }
            return false;
        }

        /* vi's writable memory but the shared buffer, at most 16 MB (probe4
         * stopped at 8 regions = 320 KB: after the shared buffer vi's .bss
         * comes in many small pieces) */
        void FindViRegions(u64 pid, u64 skip) {
            g_nregs = 0;
            os::NativeHandle h = os::InvalidNativeHandle;
            if (ProgramOf(pid, std::addressof(h)) != ViProgramId) { if (h != os::InvalidNativeHandle) { os::CloseNativeHandle(h); } return; }
            u64 addr = 0, total = 0;
            for (u32 guard = 0; guard < 4096 && g_nregs < MaxRegs; ++guard) {
                ::MemoryInfo mi = {};
                u32 pi = 0;
                if (R_FAILED(::svcQueryProcessMemory(&mi, &pi, h, addr)) || mi.size == 0) { break; }
                const u32 type = mi.type & 0xFF;
                const bool data = type == 0x04 || type == 0x05 || type == 0x09;   /* .data/.bss, heap, module .data */
                if (data && (mi.perm & 2) != 0 && mi.addr != skip && mi.size <= 16 * 1024 * 1024 && total + mi.size <= 16 * 1024 * 1024) {
                    g_regs[g_nregs++] = { mi.addr, mi.size };
                    total += mi.size;
                }
                const u64 next = mi.addr + mi.size;
                if (next <= addr) { break; }
                addr = next;
            }
            os::CloseNativeHandle(h);
            LogLine("uiprobe: corr: %u vi regions, %llu KB to search", g_nregs, static_cast<unsigned long long>(total / 1024));
            for (u32 r = 0; r < g_nregs; ++r) { if (g_regs[r].size >= 64 * 1024) { LogLine("   region %u: %#llx %llu KB", r, static_cast<unsigned long long>(g_regs[r].addr), static_cast<unsigned long long>(g_regs[r].size / 1024)); } }
        }

        /* one chunk read with no present landing during it; false if presents stopped */
        bool ReadQuiet(::ams::svc::Handle dbg, u64 va, u8 *dst, size_t n, u32 *out_count, s32 *out_slot) {
            for (u32 attempt = 0; attempt < 6; ++attempt) {
                const u32 c0 = g_shared_present.count.load(std::memory_order_acquire);
                const s32 s0 = g_shared_present.slot.load(std::memory_order_relaxed);
                if (R_FAILED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(dst), dbg, va, n))) { return false; }
                if (g_shared_present.count.load(std::memory_order_acquire) == c0) { *out_count = c0; *out_slot = s0; return true; }
            }
            return false;
        }

        void Correlate() {
            u64 pid = 0, base = 0, size = 0;
            if (!FindViBuffer(std::addressof(pid), std::addressof(base), std::addressof(size))) { return; }
            if (!WaitPresent(g_shared_present.count.load(), 200)) { LogLine("uiprobe: corr: no shared-layer presents - nothing to correlate (is vi wrapped?)"); return; }
            FindViRegions(pid, base);
            if (g_nregs == 0) { return; }
            ::ams::svc::Handle dbg = ::ams::svc::InvalidHandle;
            if (const Result ra = ::ams::svc::DebugActiveProcess(std::addressof(dbg), pid); R_FAILED(ra)) { LogLine("uiprobe: corr: attach rc=0x%x", ra.GetValue()); return; }
            static_cast<void>(DrainContinue(dbg));
            const u64 t0 = armTicksToNs(armGetSystemTick());
            g_ncorr = 0;
            u32 chunks = 0, skipped = 0, overflow = 0;
            bool stopped = false;
            /* phase 1: each chunk on both sides of a present that changed the slot */
            for (u32 r = 0; r < g_nregs && !stopped; ++r) {
                for (u64 off = 0; off < g_regs[r].size && !stopped; off += SnapChunk) {
                    const size_t n = static_cast<size_t>(std::min<u64>(SnapChunk, g_regs[r].size - off));
                    bool done = false;
                    for (u32 attempt = 0; attempt < 3 && !done && !stopped; ++attempt) {
                        u32 ca = 0, cb = 0;
                        s32 sa = 0, sb = 0;
                        if (!ReadQuiet(dbg, g_regs[r].addr + off, g_snap_buf, n, std::addressof(ca), std::addressof(sa))) { continue; }
                        /* the next present that names the other slot */
                        u32 seen = ca;
                        bool flipped = false;
                        for (u32 k = 0; k < 4 && !flipped; ++k) {
                            if (!WaitPresent(seen, 100)) { stopped = true; break; }
                            seen = g_shared_present.count.load(std::memory_order_acquire);
                            flipped = g_shared_present.slot.load(std::memory_order_relaxed) != sa;
                        }
                        if (!flipped) { continue; }
                        if (!ReadQuiet(dbg, g_regs[r].addr + off, g_snap_buf2, n, std::addressof(cb), std::addressof(sb)) || sb == sa) { continue; }
                        for (size_t i = 0; i + 4 <= n; i += 4) {
                            u32 a, b;
                            std::memcpy(std::addressof(a), g_snap_buf + i, 4);
                            std::memcpy(std::addressof(b), g_snap_buf2 + i, 4);
                            if (a == b) { continue; }
                            if (g_ncorr >= MaxCands) { ++overflow; continue; }
                            CorrCand &c = g_corr[g_ncorr++];
                            c = {};
                            c.reg = r;
                            c.off = static_cast<u32>(off + i);
                            c.v[sa & 1] = a;
                            c.v[sb & 1] = b;
                        }
                        done = true;
                    }
                    ++chunks;
                    if (!done) { ++skipped; }
                    static_cast<void>(DrainContinue(dbg));
                }
            }
            const u64 t1 = armTicksToNs(armGetSystemTick());
            LogLine("uiprobe: corr: phase 1 %u chunks (%u skipped%s) in %llu ms: %u candidates (+%u over the cap)", chunks, skipped, stopped ? ", presents STOPPED" : "",
                    static_cast<unsigned long long>((t1 - t0) / 1000000), g_ncorr, overflow);
            /* phase 2: 40 quiet reads of the chunks that hold candidates */
            u32 passes = 0;
            for (u32 p = 0; p < 40 && g_ncorr != 0; ++p) {
                if (!WaitPresent(g_shared_present.count.load(), 100)) { break; }
                os::SleepThread(TimeSpan::FromMicroSeconds(500 + 1700 * (p % 7)));   /* spread over the frame */
                ++passes;
                u32 i = 0;
                while (i < g_ncorr) {
                    const u32 r = g_corr[i].reg;
                    const u32 chunk_off = g_corr[i].off & ~static_cast<u32>(SnapChunk - 1);
                    const size_t n = static_cast<size_t>(std::min<u64>(SnapChunk, g_regs[r].size - chunk_off));
                    u32 c = 0;
                    s32 s = 0;
                    const bool ok = ReadQuiet(dbg, g_regs[r].addr + chunk_off, g_snap_buf, n, std::addressof(c), std::addressof(s));
                    for (; i < g_ncorr && g_corr[i].reg == r && (g_corr[i].off & ~static_cast<u32>(SnapChunk - 1)) == chunk_off; ++i) {
                        if (!ok) { continue; }
                        CorrCand &k = g_corr[i];
                        u32 v;
                        std::memcpy(std::addressof(v), g_snap_buf + (k.off - chunk_off), 4);
                        if (v == k.v[s & 1]) { if (k.hits[s & 1] < 255) { ++k.hits[s & 1]; } } else if (k.misses < 255) { ++k.misses; }
                    }
                }
                static_cast<void>(DrainContinue(dbg));
            }
            static_cast<void>(::ams::svc::CloseHandle(dbg));
            /* survivors: never wrong more than twice, and seen on both slots */
            g_nsurv = 0;
            u32 total = 0;
            for (u32 i = 0; i < g_ncorr; ++i) {
                const CorrCand &k = g_corr[i];
                if (k.misses <= 2 && k.hits[0] >= 5 && k.hits[1] >= 5 && static_cast<u32>(k.hits[0] + k.hits[1] + k.misses) >= passes / 2) {
                    ++total;
                    if (g_nsurv < 16) { g_survivors[g_nsurv++] = i; }
                    if (total <= 40) {
                        LogLine("   survivor: region %u +%#x (%#llx): %#x when the last present was slot %s, %#x when %s; hits %u/%u, misses %u",
                                k.reg, k.off, static_cast<unsigned long long>(g_regs[k.reg].addr + k.off),
                                k.v[0], "even", k.v[1], "odd", k.hits[0], k.hits[1], k.misses);
                    }
                }
            }
            LogLine("uiprobe: corr: %u passes, %u of %u candidates follow the presented slot; %llu ms attached; detached",
                    passes, total, g_ncorr, static_cast<unsigned long long>((armTicksToNs(armGetSystemTick()) - t0) / 1000000));
        }

        /* On the HOME menu alone: do the survivors pick the finished slot?
         * Each sample hashes slots 3 and 4 twice back to back (~12 ms): a
         * slot whose hash moved is being drawn. A word that tracks "last
         * presented" should point at a steady slot every time. */
        void VerifyOnHome() {
            if (g_nsurv == 0) { return; }
            u64 pid = 0, base = 0, size = 0;
            if (!FindViBuffer(std::addressof(pid), std::addressof(base), std::addressof(size))) { return; }
            ::ams::svc::Handle dbg = ::ams::svc::InvalidHandle;
            if (const Result ra = ::ams::svc::DebugActiveProcess(std::addressof(dbg), pid); R_FAILED(ra)) { LogLine("uiprobe: verify: attach rc=0x%x", ra.GetValue()); return; }
            static_cast<void>(DrainContinue(dbg));
            auto slot_hash = [&](u32 s) {
                u64 h = 0;
                for (size_t off = 0; off < ViSlotSize; off += SnapChunk) {
                    if (R_SUCCEEDED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(g_snap_buf), dbg, base + s * ViSlotSize + off, SnapChunk))) { h ^= Hash(g_snap_buf, SnapChunk) + off; }
                }
                return h;
            };
            u32 good[16] = {}, bad[16] = {}, unknown[16] = {}, moving[2] = {}, samples = 0;
            for (u32 n = 0; n < 60; ++n) {
                u32 val[16] = {};
                for (u32 k = 0; k < g_nsurv; ++k) {
                    const CorrCand &c = g_corr[g_survivors[k]];
                    static_cast<void>(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(std::addressof(val[k])), dbg, g_regs[c.reg].addr + c.off, 4));
                }
                const u64 a1 = slot_hash(3), b1 = slot_hash(4), a2 = slot_hash(3), b2 = slot_hash(4);
                const bool steady[2] = { b1 == b2, a1 == a2 };   /* index = parity: slot 4 even, slot 3 odd */
                moving[0] += !steady[0];
                moving[1] += !steady[1];
                ++samples;
                for (u32 k = 0; k < g_nsurv; ++k) {
                    const CorrCand &c = g_corr[g_survivors[k]];
                    const int par = val[k] == c.v[0] ? 0 : val[k] == c.v[1] ? 1 : -1;
                    if (par < 0) { ++unknown[k]; continue; }
                    if (steady[par] && !steady[par ^ 1]) { ++good[k]; } else if (!steady[par] && steady[par ^ 1]) { ++bad[k]; }
                }
                static_cast<void>(DrainContinue(dbg));
                os::SleepThread(TimeSpan::FromMicroSeconds(3000 + 2300 * (n % 5)));
            }
            static_cast<void>(::ams::svc::CloseHandle(dbg));
            LogLine("uiprobe: verify on the HOME menu: %u samples, slot 4 moving in %u, slot 3 moving in %u", samples, moving[0], moving[1]);
            for (u32 k = 0; k < g_nsurv; ++k) {
                const CorrCand &c = g_corr[g_survivors[k]];
                LogLine("   %#llx: pointed at the steady slot %u times, at the one being drawn %u times, neither value %u times",
                        static_cast<unsigned long long>(g_regs[c.reg].addr + c.off), good[k], bad[k], unknown[k]);
            }
        }

        /* ---- probe5: vi's per-layer records, found by their slot list ---
         * probe4: the controller applet's shared layer (handle 0x10) has the
         * slot list {3, 4, -1, -1} (AcquireSharedFrameBuffer's reply). vi
         * must keep that list for each shared layer; the bytes around it are
         * the layer's state. Find every copy, then log what changes from
         * frame to frame - with the last present's slot while an applet
         * presents, and with which of slots 3/4 is steady on the HOME menu. */
        void LayerStructs(const char *tag, bool presents) {
            u64 pid = 0, base = 0, size = 0;
            if (!FindViBuffer(std::addressof(pid), std::addressof(base), std::addressof(size))) { return; }
            FindViRegions(pid, base);
            ::ams::svc::Handle dbg = ::ams::svc::InvalidHandle;
            if (const Result ra = ::ams::svc::DebugActiveProcess(std::addressof(dbg), pid); R_FAILED(ra)) { LogLine("uiprobe: layers: attach rc=0x%x", ra.GetValue()); return; }
            static_cast<void>(DrainContinue(dbg));
            static constexpr s32 Pattern[4] = { 3, 4, -1, -1 };
            constexpr u32 MaxHits = 6, Before = 96, Window = 256;
            u64 hits[MaxHits] = {};
            u32 nhits = 0, total = 0;
            for (u32 r = 0; r < g_nregs; ++r) {
                for (u64 off = 0; off < g_regs[r].size; off += SnapChunk) {
                    const size_t n = static_cast<size_t>(std::min<u64>(SnapChunk, g_regs[r].size - off));
                    if (R_FAILED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(g_snap_buf), dbg, g_regs[r].addr + off, n))) { continue; }
                    for (size_t i = 0; i + sizeof(Pattern) <= n; i += 4) {
                        if (std::memcmp(g_snap_buf + i, Pattern, sizeof(Pattern)) != 0) { continue; }
                        ++total;
                        const u64 va = g_regs[r].addr + off + i;
                        if (nhits < MaxHits && va >= g_regs[r].addr + Before && va - Before + Window <= g_regs[r].addr + g_regs[r].size) { hits[nhits++] = va; }
                    }
                }
                static_cast<void>(DrainContinue(dbg));
            }
            LogLine("uiprobe: layers (%s): {3,4,-1,-1} found %u times in %u regions; windows %#x bytes from -%#x", tag, total, g_nregs, Window, Before);
            static u8 first[MaxHits][Window], cur[Window];
            for (u32 k = 0; k < nhits; ++k) {
                static_cast<void>(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(first[k]), dbg, hits[k] - Before, Window));
                LogLine("   hit %u at %#llx:", k, static_cast<unsigned long long>(hits[k]));
                for (u32 row = 0; row < Window; row += 32) {
                    u32 w[8];
                    std::memcpy(w, first[k] + row, sizeof(w));
                    LogLine("    %+5d: %08x %08x %08x %08x %08x %08x %08x %08x", static_cast<int>(row) - static_cast<int>(Before), w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
                }
            }
            auto slot_hash = [&](u32 s) {
                u64 h = 0;
                for (size_t off = 0; off < ViSlotSize; off += SnapChunk) {
                    if (R_SUCCEEDED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(g_snap_buf), dbg, base + s * ViSlotSize + off, SnapChunk))) { h ^= Hash(g_snap_buf, SnapChunk) + off; }
                }
                return h;
            };
            /* 16 samples ~20-40 ms apart: the words that differ from the first look */
            for (u32 n = 1; n <= 16 && nhits != 0; ++n) {
                const u32 c0 = g_shared_present.count.load(std::memory_order_acquire);
                const s32 s0 = g_shared_present.slot.load(std::memory_order_relaxed);
                char what[64];
                if (presents) {
                    std::snprintf(what, sizeof(what), "last present slot %d", s0);
                } else {
                    const u64 a1 = slot_hash(3), b1 = slot_hash(4), a2 = slot_hash(3), b2 = slot_hash(4);
                    std::snprintf(what, sizeof(what), "slot 3 %s, slot 4 %s", a1 == a2 ? "steady" : "MOVING", b1 == b2 ? "steady" : "MOVING");
                }
                for (u32 k = 0; k < nhits; ++k) {
                    static_cast<void>(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(cur), dbg, hits[k] - Before, Window));
                    char line[300];
                    int len = 0;
                    u32 diffs = 0;
                    for (u32 i = 0; i < Window && len < static_cast<int>(sizeof(line)) - 40; i += 4) {
                        u32 a, b;
                        std::memcpy(std::addressof(a), first[k] + i, 4);
                        std::memcpy(std::addressof(b), cur + i, 4);
                        if (a == b) { continue; }
                        ++diffs;
                        len += std::snprintf(line + len, sizeof(line) - len, " %+d:%x", static_cast<int>(i) - static_cast<int>(Before), b);
                    }
                    const bool raced = presents && g_shared_present.count.load(std::memory_order_acquire) != c0;
                    if (diffs != 0) { LogLine("   sample %2u hit %u (%s%s): %u changed:%s", n, k, what, raced ? ", a present landed meanwhile" : "", diffs, line); }
                }
                static_cast<void>(DrainContinue(dbg));
                os::SleepThread(TimeSpan::FromMicroSeconds(9000 + 6100 * (n % 4)));
            }
            static_cast<void>(::ams::svc::CloseHandle(dbg));
            LogLine("uiprobe: layers (%s) done", tag);
        }

        /* ---- probe6: the words that flip between 3 and 4 -----------------
         * probe5: the word that follows the controller applet's presents
         * (5848 KB region +0x1760e8) stays put on the HOME menu - it is the
         * applet's layer's, not the HOME menu's. So search on the HOME menu
         * itself: every word of vi's data read twice ~9 ms apart; the ones
         * that went 3 -> 4 or 4 -> 3 are candidates. Then 40 samples, each
         * with which of slots 3/4 is being drawn (hashed twice ~6 ms apart),
         * say what each candidate means: equal to the slot being drawn
         * (read the other one), or to the finished one. */
        struct FlipCand { u32 reg; u32 off; u16 flips; u8 eq_moving; u8 eq_steady; u8 n3; u8 n4; u8 other; u8 pad; };
        constexpr u32 MaxFlip = 2048;
        constinit FlipCand g_flip[MaxFlip] = {};

        void FlipSearch(const char *tag) {
            u64 pid = 0, base = 0, size = 0;
            if (!FindViBuffer(std::addressof(pid), std::addressof(base), std::addressof(size))) { return; }
            FindViRegions(pid, base);
            ::ams::svc::Handle dbg = ::ams::svc::InvalidHandle;
            if (const Result ra = ::ams::svc::DebugActiveProcess(std::addressof(dbg), pid); R_FAILED(ra)) { LogLine("uiprobe: flips: attach rc=0x%x", ra.GetValue()); return; }
            static_cast<void>(DrainContinue(dbg));
            const u64 t0 = armTicksToNs(armGetSystemTick());
            u32 n = 0, over = 0, big = 0;
            u64 big_base = 0, big_size = 0;
            for (u32 r = 0; r < g_nregs; ++r) {
                if (g_regs[r].size > big_size) { big = r; big_size = g_regs[r].size; big_base = g_regs[r].addr; }
                for (u64 off = 0; off < g_regs[r].size; off += SnapChunk) {
                    const size_t len = static_cast<size_t>(std::min<u64>(SnapChunk, g_regs[r].size - off));
                    if (R_FAILED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(g_snap_buf), dbg, g_regs[r].addr + off, len))) { continue; }
                    os::SleepThread(TimeSpan::FromMilliSeconds(9));
                    if (R_FAILED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(g_snap_buf2), dbg, g_regs[r].addr + off, len))) { continue; }
                    for (size_t i = 0; i + 4 <= len; i += 4) {
                        u32 a, b;
                        std::memcpy(std::addressof(a), g_snap_buf + i, 4);
                        std::memcpy(std::addressof(b), g_snap_buf2 + i, 4);
                        if (!((a == 3 && b == 4) || (a == 4 && b == 3))) { continue; }
                        if (n >= MaxFlip) { ++over; continue; }
                        g_flip[n++] = { r, static_cast<u32>(off + i), 1, 0, 0, 0, 0, 0, 0 };
                    }
                    static_cast<void>(DrainContinue(dbg));
                }
            }
            const u64 t1 = armTicksToNs(armGetSystemTick());
            auto slot_hash = [&](u32 s) {
                u64 h = 0;
                for (size_t off = 0; off < ViSlotSize; off += SnapChunk) {
                    if (R_SUCCEEDED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(g_snap_buf), dbg, base + s * ViSlotSize + off, SnapChunk))) { h ^= Hash(g_snap_buf, SnapChunk) + off; }
                }
                return h;
            };
            static u32 last[MaxFlip];
            for (u32 i = 0; i < n; ++i) { last[i] = 0; }
            u32 one_moving = 0;
            for (u32 p = 0; p < 40 && n != 0; ++p) {
                /* the candidates, then the slots, then the candidates again: kept only if unchanged across */
                static u32 v1[MaxFlip];
                for (u32 i = 0; i < n; ++i) { static_cast<void>(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(std::addressof(v1[i])), dbg, g_regs[g_flip[i].reg].addr + g_flip[i].off, 4)); }
                const u64 a1 = slot_hash(3), b1 = slot_hash(4), a2 = slot_hash(3), b2 = slot_hash(4);
                const bool m3 = a1 != a2, m4 = b1 != b2;
                const s32 moving = (m3 && !m4) ? 3 : (m4 && !m3) ? 4 : -1;
                one_moving += moving >= 0;
                for (u32 i = 0; i < n; ++i) {
                    u32 v2 = 0;
                    static_cast<void>(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(std::addressof(v2)), dbg, g_regs[g_flip[i].reg].addr + g_flip[i].off, 4));
                    FlipCand &c = g_flip[i];
                    if (v2 == 3) { ++c.n3; } else if (v2 == 4) { ++c.n4; } else if (c.other < 255) { ++c.other; }
                    if (p > 0 && v2 != last[i] && c.flips < 65535) { ++c.flips; }
                    last[i] = v2;
                    if (moving >= 0 && v1[i] == v2) {
                        if (static_cast<s32>(v2) == moving) { ++c.eq_moving; } else if (v2 == static_cast<u32>(7 - moving)) { ++c.eq_steady; }
                    }
                }
                static_cast<void>(DrainContinue(dbg));
                os::SleepThread(TimeSpan::FromMicroSeconds(2000 + 3100 * (p % 5)));
            }
            static_cast<void>(::ams::svc::CloseHandle(dbg));
            LogLine("uiprobe: flips (%s): %u words went 3<->4 in ~9 ms (+%u over the cap), search %llu ms; 40 samples, %u with exactly one slot being drawn; the big region is %u (%#llx)",
                    tag, n, over, static_cast<unsigned long long>((t1 - t0) / 1000000), one_moving, big, static_cast<unsigned long long>(big_base));
            u32 shown = 0;
            for (u32 i = 0; i < n && shown < 40; ++i) {
                const FlipCand &c = g_flip[i];
                if (c.flips < 6 || c.other > 2) { continue; }
                ++shown;
                const u64 va = g_regs[c.reg].addr + c.off;
                LogLine("   %#llx (%s +%#llx): flipped %u of 39, 3 x%u 4 x%u other x%u; = the slot being drawn %u, = the finished one %u",
                        static_cast<unsigned long long>(va), c.reg == big ? "big" : "region", static_cast<unsigned long long>(c.reg == big ? va - big_base : c.off),
                        c.flips, c.n3, c.n4, c.other, c.eq_moving, c.eq_steady);
            }
            if (shown == 0) { LogLine("   none flipped with the frames"); }
        }

        /* the memory around the applet's word (5848 KB region +0x1760e8 in probe5) */
        void DumpAroundAppletWord(const char *tag) {
            if (g_nsurv == 0) { return; }
            const CorrCand &c = g_corr[g_survivors[0]];
            const u64 va = g_regs[c.reg].addr + c.off;
            u64 pid = 0, base = 0, size = 0;
            if (!FindViBuffer(std::addressof(pid), std::addressof(base), std::addressof(size))) { return; }
            ::ams::svc::Handle dbg = ::ams::svc::InvalidHandle;
            if (R_FAILED(::ams::svc::DebugActiveProcess(std::addressof(dbg), pid))) { return; }
            static_cast<void>(DrainContinue(dbg));
            static u8 win[1024];
            const bool ok = R_SUCCEEDED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(win), dbg, va - 512, sizeof(win)));
            static_cast<void>(::ams::svc::CloseHandle(dbg));
            if (!ok) { return; }
            LogLine("uiprobe: around the applet's word %#llx (%s), -512..+512:", static_cast<unsigned long long>(va), tag);
            for (u32 row = 0; row < sizeof(win); row += 32) {
                u32 w[8];
                std::memcpy(w, win + row, sizeof(w));
                LogLine("    %+5d: %08x %08x %08x %08x %08x %08x %08x %08x", static_cast<int>(row) - 512, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
            }
        }

        /* ---- probe7: the HOME menu's presents, read out of vi's IPC buffers
         * probe6: the "word" is the slot argument of the last
         * PresentSharedFrameBuffer (8255) request still in one of vi's IPC
         * message buffers - "SFCI", command 0x203f, then the arguments:
         * fence (count, {id, value} x4) at -0x48, crop -0x24, transform
         * -0x14, swap interval -0x10, layer -0x08, slot +0. The HOME menu's
         * is another buffer (0x400 lower). So every frame the HOME menu
         * presents shows up there with its fence. A present of slot B means
         * slot A is finished and on screen until B is shown (the HOME menu
         * cannot draw into A before the display lets go of it). */
        struct PresentMsg { u64 va; u32 fence_id, fence_value; u64 layer; u64 slot; };

        bool ParsePresentAt(const u8 *p, PresentMsg *m) {   /* p = the slot word */
            u32 cnt, id, val, swap, pad, slot_hi, layer_hi;
            u64 layer, slot;
            std::memcpy(std::addressof(cnt), p - 0x48, 4);
            std::memcpy(std::addressof(id), p - 0x44, 4);
            std::memcpy(std::addressof(val), p - 0x40, 4);
            std::memcpy(std::addressof(swap), p - 0x10, 4);
            std::memcpy(std::addressof(pad), p - 0x0C, 4);
            std::memcpy(std::addressof(layer), p - 0x08, 8);
            std::memcpy(std::addressof(slot), p, 8);
            slot_hi = static_cast<u32>(slot >> 32);
            layer_hi = static_cast<u32>(layer >> 32);
            if (cnt < 1 || cnt > 4 || id >= 192 || swap > 4 || pad != 0 || layer_hi != 0 || slot_hi != 0) { return false; }
            if (layer == 0 || layer >= 0x1000 || slot >= 7) { return false; }
            m->fence_id = id;
            m->fence_value = val;
            m->layer = layer;
            m->slot = slot;
            return true;
        }

        constexpr u32 MaxMsgs = 16;
        constinit PresentMsg g_msgs[MaxMsgs] = {};
        constinit u32 g_nmsgs = 0;

        /* every present-shaped message in vi's data; live = its fence moved in 40 ms */
        u32 ScanPresents(::ams::svc::Handle dbg, const char *tag) {
            g_nmsgs = 0;
            u32 shaped = 0;
            for (u32 r = 0; r < g_nregs; ++r) {
                for (u64 off = 0; off < g_regs[r].size; off += SnapChunk) {
                    const size_t len = static_cast<size_t>(std::min<u64>(SnapChunk, g_regs[r].size - off));
                    if (R_FAILED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(g_snap_buf), dbg, g_regs[r].addr + off, len))) { continue; }
                    for (size_t i = 0x48; i + 8 <= len; i += 4) {
                        PresentMsg m;
                        if (!ParsePresentAt(g_snap_buf + i, std::addressof(m))) { continue; }
                        ++shaped;
                        if (g_nmsgs < MaxMsgs) { m.va = g_regs[r].addr + off + i; g_msgs[g_nmsgs++] = m; }
                    }
                }
                static_cast<void>(DrainContinue(dbg));
            }
            os::SleepThread(TimeSpan::FromMilliSeconds(40));
            u32 live = 0;
            LogLine("uiprobe: presents (%s): %u present-shaped messages in vi's data", tag, shaped);
            for (u32 k = 0; k < g_nmsgs; ++k) {
                u8 buf[0x50];
                PresentMsg now = {};
                const bool ok = R_SUCCEEDED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(buf), dbg, g_msgs[k].va - 0x48, sizeof(buf))) && ParsePresentAt(buf + 0x48, std::addressof(now));
                const bool moved = ok && now.fence_value != g_msgs[k].fence_value;
                live += moved;
                u32 hdr[4] = {};
                static_cast<void>(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(hdr), dbg, g_msgs[k].va - 0x58, sizeof(hdr)));
                LogLine("   %#llx: layer %#llx slot %llu, fence %u: %u -> %u%s; header %08x cmd %u",
                        static_cast<unsigned long long>(g_msgs[k].va), static_cast<unsigned long long>(g_msgs[k].layer), static_cast<unsigned long long>(g_msgs[k].slot),
                        g_msgs[k].fence_id, g_msgs[k].fence_value, ok ? now.fence_value : 0, moved ? " LIVE" : "", hdr[0], hdr[2]);
                if (moved) { const u64 keep = g_msgs[k].va; g_msgs[k] = now; g_msgs[k].va = keep; }
                if (!moved) { g_msgs[k].fence_value = 0xFFFFFFFFu; }   /* not live */
            }
            return live;
        }

        void ScanPresentsAttached(const char *tag) {
            u64 pid = 0, base = 0, size = 0;
            if (!FindViBuffer(std::addressof(pid), std::addressof(base), std::addressof(size))) { return; }
            FindViRegions(pid, base);
            ::ams::svc::Handle dbg = ::ams::svc::InvalidHandle;
            if (R_FAILED(::ams::svc::DebugActiveProcess(std::addressof(dbg), pid))) { return; }
            static_cast<void>(DrainContinue(dbg));
            static_cast<void>(ScanPresents(dbg, tag));
            static_cast<void>(::ams::svc::CloseHandle(dbg));
        }

        /* the capture loop the stream would run, on the HOME menu, 10 s */
        void HomeCapture(const char *tag) {
            u64 pid = 0, base = 0, size = 0;
            if (!FindViBuffer(std::addressof(pid), std::addressof(base), std::addressof(size))) { return; }
            FindViRegions(pid, base);
            ::ams::svc::Handle dbg = ::ams::svc::InvalidHandle;
            if (const Result ra = ::ams::svc::DebugActiveProcess(std::addressof(dbg), pid); R_FAILED(ra)) { LogLine("uiprobe: capture: attach rc=0x%x", ra.GetValue()); return; }
            static_cast<void>(DrainContinue(dbg));
            const u32 live = ScanPresents(dbg, tag);
            u32 pick = MaxMsgs;
            for (u32 k = 0; k < g_nmsgs; ++k) { if (g_msgs[k].fence_value != 0xFFFFFFFFu) { pick = k; break; } }
            if (live == 0 || pick == MaxMsgs) { LogLine("uiprobe: capture: no live present message - the HOME menu's presents were not found"); static_cast<void>(::ams::svc::CloseHandle(dbg)); return; }
            const u64 va = g_msgs[pick].va;
            LogLine("uiprobe: capture: following %#llx (layer %#llx, syncpoint %u) for 10 s", static_cast<unsigned long long>(va),
                    static_cast<unsigned long long>(g_msgs[pick].layer), g_msgs[pick].fence_id);
            auto read_msg = [&](PresentMsg *m) {
                u8 buf[0x50];
                return R_SUCCEEDED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(buf), dbg, va - 0x48, sizeof(buf))) && ParsePresentAt(buf + 0x48, m);
            };
            auto slot_hash = [&](u32 s) {
                u64 h = 0;
                for (size_t off = 0; off < ViSlotSize; off += SnapChunk) {
                    if (R_SUCCEEDED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(g_snap_buf), dbg, base + s * ViSlotSize + off, SnapChunk))) { h ^= Hash(g_snap_buf, SnapChunk) + off; }
                }
                return h;
            };
            PresentMsg last = {};
            static_cast<void>(read_msg(std::addressof(last)));
            const u32 fence_first = last.fence_value;
            const u64 t0 = armTicksToNs(armGetSystemTick());
            u32 presents = 0, frames = 0, clean = 0, torn = 0, same_slot = 0, polls = 0, skipped = 0;
            u64 read_max = 0, gap_max = 0, last_t = t0;
            while (armTicksToNs(armGetSystemTick()) - t0 < UINT64_C(10000000000)) {
                PresentMsg m;
                ++polls;
                if (!read_msg(std::addressof(m)) || m.fence_value == last.fence_value) {
                    os::SleepThread(TimeSpan::FromMicroSeconds(700));
                    if ((polls & 63) == 0) { static_cast<void>(DrainContinue(dbg)); }
                    continue;
                }
                const u64 t = armTicksToNs(armGetSystemTick());
                if (presents != 0 && t - last_t > gap_max) { gap_max = t - last_t; }
                last_t = t;
                ++presents;
                if (m.slot == last.slot) { ++same_slot; last = m; continue; }
                const u32 finished = static_cast<u32>(last.slot);
                last = m;
                if ((presents & 1) == 0) { ++skipped; continue; }   /* every other frame: room to poll between reads */
                /* read the finished slot whole (what the stream would hand to the VIC), then check it did not move */
                const u64 r0 = armTicksToNs(armGetSystemTick());
                const u64 h1 = slot_hash(finished);
                const u64 h2 = slot_hash(finished);
                const u64 rt = (armTicksToNs(armGetSystemTick()) - r0) / 2;
                if (rt > read_max) { read_max = rt; }
                ++frames;
                if (h1 == h2) { ++clean; } else { ++torn; }
            }
            LogLine("uiprobe: capture (%s): %u presents seen in 10 s (fence %u -> %u), longest gap %llu ms, %u named the same slot twice; %u frames read, %u clean, %u changed under the read; slot read %llu us at most; %u polls",
                    tag, presents, fence_first, last.fence_value, static_cast<unsigned long long>(gap_max / 1000000), same_slot, frames, clean, torn,
                    static_cast<unsigned long long>(read_max / 1000), polls);
            AMS_UNUSED(skipped);

            /* one clean picture: each 64 KB piece read right after a present, from the slot just finished */
            static_cast<void>(fs::CreateDirectory("sdmc:/config/switch-frame-tap/probe"));
            char path[96];
            std::snprintf(path, sizeof(path), "sdmc:/config/switch-frame-tap/probe/capture-%s.raw", tag[0] == 'h' && tag[5] == 'a' ? "home2" : "home1");
            static_cast<void>(fs::DeleteFile(path));
            u32 pieces = 0, mixed = 0;
            if (R_SUCCEEDED(fs::CreateFile(path, static_cast<s64>(ViSlotSize)))) {
                fs::FileHandle f;
                if (R_SUCCEEDED(fs::OpenFile(std::addressof(f), path, fs::OpenMode_Write))) {
                    s32 first_slot = -1;
                    for (size_t off = 0; off < ViSlotSize; off += SnapChunk) {
                        /* wait for a present that leaves the same slot finished as the first piece's */
                        for (u32 guard = 0; guard < 400; ++guard) {
                            PresentMsg m;
                            if (read_msg(std::addressof(m)) && m.fence_value != last.fence_value) {
                                const s32 fin = static_cast<s32>(last.slot);
                                last = m;
                                if (m.slot != static_cast<u64>(fin) && (first_slot < 0 || fin == first_slot)) {
                                    if (first_slot < 0) { first_slot = fin; }
                                    break;
                                }
                            }
                            os::SleepThread(TimeSpan::FromMicroSeconds(500));
                        }
                        if (R_FAILED(::ams::svc::ReadDebugProcessMemory(reinterpret_cast<uintptr_t>(g_snap_buf), dbg, base + static_cast<u32>(first_slot < 0 ? 3 : first_slot) * ViSlotSize + off, SnapChunk))) { ++mixed; }
                        PresentMsg after;
                        if (read_msg(std::addressof(after)) && after.fence_value != last.fence_value) { ++mixed; }   /* a present landed during the read */
                        static_cast<void>(fs::WriteFile(f, static_cast<s64>(off), g_snap_buf, SnapChunk, fs::WriteOption::None));
                        ++pieces;
                        static_cast<void>(DrainContinue(dbg));
                    }
                    static_cast<void>(fs::FlushFile(f));
                    fs::CloseFile(f);
                }
            }
            static_cast<void>(::ams::svc::CloseHandle(dbg));
            LogLine("uiprobe: capture: picture %s (%u pieces, %u with a present landing mid-read); detached", path, pieces, mixed);
        }

        /* the HOME menu alone, 75 s in; a library applet 4 s after it opened */
        void SnapSteps(u64 now, const Proc *cur, u32 cn) {
            u64 lib = 0;
            for (u32 j = 0; j < cn; ++j) {
                const u64 t = cur[j].tid;
                if (IsUiProgram(t) && t != UINT64_C(0x0100000000001000) && t != UINT64_C(0x010000000000100C)) { lib = t; }
            }
            const bool app = g_app_pid.load(std::memory_order_relaxed) != 0;
            if (!g_snap_home_done && lib == 0 && !app && now >= UINT64_C(75000000000)) {
                g_snap_home_done = true;
                SnapVi("home");
                HomeCapture("home");
                if (HasWord("layers")) { LayerStructs("home", false); }
            }
            if (lib != g_applet_tid) { g_applet_tid = lib; g_applet_since_ns = now; }
            if (!g_snap_applet_done && lib != 0 && lib != UINT64_C(0x010000000000100D) && now - g_applet_since_ns >= UINT64_C(4000000000)) {
                g_snap_applet_done = true;
                const char *nm = AppletName(lib);
                LogLine("uiprobe: %s has been open 4 s (%u shared-layer presents seen so far)", nm != nullptr ? nm : "an applet", g_shared_present.count.load());
                SnapVi("applet");
                ScanPresentsAttached("applet");
                if (HasWord("corr")) { Correlate(); FlipSearch("applet"); DumpAroundAppletWord("applet"); }
                if (HasWord("verify")) { VerifyOnHome(); }   /* probe5's check, kept for reference */
            }
            /* the survivors checked on the HOME menu, 3 s after it is alone again */
            const bool home_alone = lib == 0 && !app;
            if (!home_alone) { g_home_alone_since = 0; } else if (g_home_alone_since == 0) { g_home_alone_since = now; }
            if (g_snap_applet_done && !g_verify_done && home_alone && now - g_home_alone_since >= UINT64_C(3000000000)) {
                g_verify_done = true;
                HomeCapture("home again");
            }
        }

        void Summaries(u64 now) {
            if (now < g_next_summary_ns) { return; }
            g_next_summary_ns = now + UINT64_C(5000000000);
            static u32 last_q[16] = {};
            static s32 last_id[16] = {};
            static UiBinderInfo b[16];
            const u32 n = UiBinderSnapshot(b, 16);
            for (u32 i = 0; i < n; ++i) {
                if (b[i].id == last_id[i] && b[i].queues == last_q[i]) { continue; }
                last_id[i] = b[i].id;
                last_q[i] = b[i].queues;
                const char *nm = AppletName(b[i].tid);
                LogLine("ui binder %d (%s): %u txns, %u queued, %u dequeued, %u requests; last slot %d, slots used 0x%x, crop (%d,%d)-(%d,%d), transform 0x%x",
                        b[i].id, nm != nullptr ? nm : "?", b[i].txns, b[i].queues, b[i].dequeues, b[i].requests, b[i].last_slot, b[i].slots_seen,
                        b[i].crop[0], b[i].crop[1], b[i].crop[2], b[i].crop[3], b[i].transform);
            }
        }

    }

    bool UiProbeOn() { ReadConfig(); return g_probe_on; }

    bool UiProbeWants(const char *word) {
        ReadConfig();
        if (!g_probe_on) { return false; }
        char no[32];
        std::snprintf(no, sizeof(no), "no%s", word);
        return !HasWord(no);
    }

    void UiProbeSetDispdrv(const ::Service &s) { g_dd = s; g_dd_ok = true; }

    void UiProbeStep() {
        if (!g_probe_checked) {
            ReadConfig();
            if (g_probe_on) {
                LogLine("uiprobe: on (config/switch-frame-tap/probe-ui.txt: \"%.40s\") - map %s, dispdrv %s, vi %s, binders %s, snap %s", g_probe_words,
                        UiProbeWants("map") ? "on" : "off", UiProbeWants("dispdrv") ? "on" : "off", UiProbeWants("vi") ? "on" : "off",
                        HasWord("binders") ? "on" : "off", UiProbeWants("snap") ? "on" : "off");
            }
        }
        if (!g_probe_on || g_pmdmnt_rc != 0) { return; }
        const u64 now = armTicksToNs(armGetSystemTick());

        Summaries(now);

        /* the binder look, last in the boot: 4 minutes in, then 60 s of samples */
        if (HasWord("binders") && g_dd_ok) {   /* probe4: opt-in - every id answered alike */
            if (g_binder_stage == 0 && now >= UINT64_C(240000000000)) {
                g_binder_stage = 1;
                Enumerate();
                LogLine("uiprobe: sampling requestBuffer on %u binder(s) for 60 s (succeeds only while the slot is being drawn)", g_ncands);
            } else if (g_binder_stage == 1) {
                RaceStep();
                if (++g_race_steps >= 240) {
                    g_binder_stage = 2;
                    for (u32 c = 0; c < g_ncands; ++c) {
                        const Cand &k = g_cands[c];
                        LogLine("uiprobe: binder %d (%dx%d): dequeued when sampled - slot0 %u/%u, slot1 %u/%u, slot2 %u/%u, slot3 %u/%u",
                                k.id, k.w, k.h, k.ok[0], k.ok[0] + k.bad[0], k.ok[1], k.ok[1] + k.bad[1], k.ok[2], k.ok[2] + k.bad[2], k.ok[3], k.ok[3] + k.bad[3]);
                    }
                    LogLine("uiprobe: binder look done");
                }
            }
        }

        if (now < g_next_ns) { return; }
        g_next_ns = now + UINT64_C(1000000000);

        static u64 pids[96];
        s32 n = 0;
        if (R_FAILED(::svcGetProcessList(&n, pids, 96))) { return; }
        Proc cur[96];
        u32 cn = 0;
        bool changed = false;
        for (s32 i = 0; i < n && cn < 96; ++i) {
            u64 tid = 0;
            for (u32 k = 0; k < g_prev_n; ++k) { if (g_prev[k].pid == pids[i]) { tid = g_prev[k].tid; break; } }
            const bool known = tid != 0;
            if (!known) { tid = ProgramOf(pids[i], nullptr); }
            cur[cn++] = { pids[i], tid };
            if (!known && g_prev_n != 0 && tid != 0) {
                const char *nm = AppletName(tid);
                LogLine("uiprobe: started %016llx%s%s%s (pid %llu)", static_cast<unsigned long long>(tid),
                        nm ? " (" : "", nm ? nm : "", nm ? ")" : "", static_cast<unsigned long long>(pids[i]));
                if (IsUiProgram(tid)) { changed = true; }
            }
        }
        for (u32 k = 0; k < g_prev_n && g_prev_n != 0; ++k) {
            bool alive = false;
            for (u32 j = 0; j < cn; ++j) { if (cur[j].pid == g_prev[k].pid) { alive = true; break; } }
            if (!alive) {
                const char *nm = AppletName(g_prev[k].tid);
                LogLine("uiprobe: exited %016llx%s%s%s", static_cast<unsigned long long>(g_prev[k].tid), nm ? " (" : "", nm ? nm : "", nm ? ")" : "");
            }
        }
        const bool first = g_prev_n == 0;
        std::memcpy(g_prev, cur, sizeof(Proc) * cn);
        g_prev_n = cn;

        /* the game in front or not (pid 0: the HOME menu or an applet) */
        const u64 app = g_app_pid.load(std::memory_order_relaxed);
        if (app != g_last_app) {
            LogLine("uiprobe: application running: %s", app != 0 ? "yes" : "no");
            g_last_app = app;
        }
        if (UiProbeWants("snap")) { SnapSteps(now, cur, cn); }
        /* test5: System Settings showed no new process in the stream's eyes but
         * took the Applet pool up ~317 MB - dump the maps on a jump like that */
        {
            static u64 base_mb = 0;
            u64 cur = 0, lim = 0;
            if (R_SUCCEEDED(::ams::pm::dmnt::AtmosphereGetCurrentLimitInfo(std::addressof(cur), std::addressof(lim), ::ams::pm::ResourceLimitGroup_Applet, svc::LimitableResource_PhysicalMemoryMax))) {
                const u64 mb = cur >> 20;
                if (base_mb == 0 || mb < base_mb) { base_mb = mb; }
                if (mb >= base_mb + 100 && UiProbeWants("map") && g_dump_at_ns == 0) {
                    LogLine("uiprobe: the Applet pool grew %llu -> %llu MB", static_cast<unsigned long long>(base_mb), static_cast<unsigned long long>(mb));
                    base_mb = mb;
                    g_dump_at_ns = now + UINT64_C(3000000000);
                    g_dump_why = "the Applet pool grew 100 MB or more";
                }
            }
        }
        if ((first || changed) && UiProbeWants("map")) {
            g_dump_at_ns = now + UINT64_C(2000000000);   /* let it allocate first */
            g_dump_why = first ? "idle" : "a HOME-menu process or applet started";
        }
        if (g_dump_at_ns != 0 && now >= g_dump_at_ns && g_dumps < 16) {
            g_dump_at_ns = 0;
            ++g_dumps;
            DumpMaps(g_dump_why);
        }
    }

}

#endif
