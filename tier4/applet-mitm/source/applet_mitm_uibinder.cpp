#include "applet_mitm_uibinder.hpp"
#include <cstdio>
#include <cstring>
#include "applet_mitm_log.hpp"
#include "applet_mitm_gbuf.hpp"
#include "applet_mitm_service.hpp"

namespace ams::mitm::applet {

    namespace {

        const char *CodeName(u32 code) {
            switch (code) {
                case 1:  return "requestBuffer";
                case 2:  return "setBufferCount";
                case 3:  return "dequeueBuffer";
                case 4:  return "detachBuffer";
                case 5:  return "detachNextBuffer";
                case 6:  return "attachBuffer";
                case 7:  return "queueBuffer";
                case 8:  return "cancelBuffer";
                case 9:  return "query";
                case 10: return "connect";
                case 11: return "disconnect";
                case 13: return "allocateBuffers";
                case 14: return "setPreallocatedBuffer";
                case 17: return "getBufferHistory";
                default: return "?";
            }
        }

        struct Rec {
            s32 id;
            u64 tid;
            u32 txns, queues, dequeues, requests;
            s32 last_slot;
            u32 slots_seen;
            s32 crop[4];
            u32 transform;
            u64 tick;
            u32 codes_dumped;   /* bit per code: its first parcel was hex-dumped */
            u32 logged;
        };
        constexpr u32 MaxRecs = 16;
        constinit os::SdkMutex g_lock;
        constinit Rec g_recs[MaxRecs] = {};
        constinit u32 g_nrecs = 0;
        constinit u32 g_gb_logged = 0;

        /* under g_lock */
        Rec *Find(s32 id, u64 tid) {
            for (u32 i = 0; i < g_nrecs; ++i) {
                if (g_recs[i].id == id) {
                    if (g_recs[i].tid != tid) { g_recs[i] = {}; g_recs[i].id = id; g_recs[i].tid = tid; g_recs[i].last_slot = -1; }
                    return std::addressof(g_recs[i]);
                }
            }
            Rec *r = std::addressof(g_recs[g_nrecs < MaxRecs ? g_nrecs++ : (static_cast<u32>(id) % MaxRecs)]);
            *r = {};
            r->id = id;
            r->tid = tid;
            r->last_slot = -1;
            return r;
        }

        /* the parcel's payload, past the 16-byte header */
        void DumpParcel(const char *tag, const void *p, size_t sz) {
            if (p == nullptr || sz < 16) { LogLine("   %s: %zu bytes", tag, sz); return; }
            u32 hdr[4];
            std::memcpy(hdr, p, sizeof(hdr));
            const u8 *b = static_cast<const u8 *>(p);
            size_t off = hdr[1], len = hdr[0];
            if (off > sz || len > sz - off) { off = 0; len = sz; }
            /* skip the interface token (u32 policy, u32 len, UTF-16 + NUL, padded to 4) */
            if (len >= 8) {
                u32 tl = 0;
                std::memcpy(std::addressof(tl), b + off + 4, 4);
                if (tl > 0 && tl < 128) {
                    const size_t skip = (8 + (static_cast<size_t>(tl) + 1) * 2 + 3) & ~static_cast<size_t>(3);
                    if (skip <= len) { off += skip; len -= skip; }
                }
            }
            char hex[3 * 48 + 1] = {};
            const size_t n = len < 48 ? len : 48;
            for (size_t i = 0; i < n; ++i) { std::snprintf(hex + 3 * i, 4, "%02x ", b[off + i]); }
            LogLine("   %s: header (data %u @%u, objects %u @%u), %zu argument byte(s): %s%s", tag, hdr[0], hdr[1], hdr[2], hdr[3], len, hex, len > n ? "..." : "");
        }

        /* bookkeeping for one transaction, and a line for the first of each kind */
        void Note(u64 tid, s32 id, u32 code, const u8 *in, size_t in_size) {
            std::scoped_lock lk(g_lock);
            Rec *r = Find(id, tid);
            ++r->txns;
            if (code == 3) { ++r->dequeues; }
            if (code == 1) { ++r->requests; }
            if (code == 7) {
                ++r->queues;
                const s32 slot = ParseParcelSlot(in, in_size);
                u32 fn = 0, xf = 0;
                u64 fv[4] = {};
                s32 crop[4] = {};
                const bool ok = ParseParcelQueueInput(in, in_size, std::addressof(fn), fv, std::addressof(xf), crop);
                r->last_slot = slot;
                if (slot >= 0 && slot < 32) { r->slots_seen |= 1u << slot; }
                if (ok) { std::memcpy(r->crop, crop, sizeof(crop)); r->transform = xf; }
                r->tick = armGetSystemTick();
                if (r->queues <= 3) {
                    LogLine("ui binder %d (%016llx): queueBuffer #%u slot %d, crop (%d,%d)-(%d,%d), transform 0x%x, fence %u: %08llx %08llx",
                            id, static_cast<unsigned long long>(tid), r->queues, slot, crop[0], crop[1], crop[2], crop[3], xf, fn,
                            static_cast<unsigned long long>(fv[0]), static_cast<unsigned long long>(fv[1]));
                }
                return;
            }
            const u32 bit = code < 32 ? (1u << code) : 0;
            const bool first_of_code = bit != 0 && (r->codes_dumped & bit) == 0;
            r->codes_dumped |= bit;
            /* the rare ones always, the per-frame dequeueBuffer only its first */
            if (first_of_code || (code != 3 && r->logged < 40)) {
                ++r->logged;
                LogLine("ui binder %d (%016llx): %s (%u) #%u, in %zu bytes", id, static_cast<unsigned long long>(tid), CodeName(code), code, r->txns, in_size);
                if (first_of_code || code == 1 || code == 14) { DumpParcel("  in", in, in_size); }
            }
        }

    }

    u32 UiBinderSnapshot(UiBinderInfo *out, u32 max) {
        std::scoped_lock lk(g_lock);
        u32 n = 0;
        for (u32 i = 0; i < g_nrecs && n < max; ++i) {
            const Rec &r = g_recs[i];
            out[n++] = { r.id, r.tid, r.txns, r.queues, r.dequeues, r.requests, r.last_slot, r.slots_seen,
                         { r.crop[0], r.crop[1], r.crop[2], r.crop[3] }, r.transform, r.tick };
        }
        return n;
    }

    Result UiBinderMitm::Transact(u32 cmd, s32 binder_id, u32 code, u32 flags, const void *in, size_t in_size, void *out, size_t out_size) {
        const struct { s32 id; u32 code; u32 flags; } args = { binder_id, code, flags };
        const u32 kind = cmd == 0 ? SfBufferAttr_HipcMapAlias : SfBufferAttr_HipcAutoSelect;
        R_RETURN(::ams::Result(serviceDispatchIn(m_forward_service.get(), cmd, args,
            .buffer_attrs = { kind | SfBufferAttr_In, kind | SfBufferAttr_Out },
            .buffers      = { { in, in_size }, { out, out_size } },
        )));
    }

    namespace {

        /* requestBuffer: forwarded by hand, its reply read for the slot's
         * NvGraphicBuffer (where the applet's pixels are) */
        template<typename F>
        Result ForwardAndRead(u64 tid, s32 id, u32 code, const void *in, size_t in_size, void *out, size_t out_size, F &&forward) {
            const Result rc = forward();
            const s32 slot = ParseParcelSlot(static_cast<const u8 *>(in), in_size);
            if (R_FAILED(rc)) {
                LogLine("ui binder %d (%016llx): %s slot %d forwarded by hand -> rc=0x%x", id, static_cast<unsigned long long>(tid), CodeName(code), slot, rc.GetValue());
                R_RETURN(rc);
            }
            u32 n = 0;
            {
                std::scoped_lock lk(g_lock);
                n = ++g_gb_logged;
            }
            if (n <= 24) {
                if (const auto *gb = FindGraphicBuffer(out, out_size); gb != nullptr) {
                    char tag[64];
                    std::snprintf(tag, sizeof(tag), "ui binder %d (%016llx) slot %d", id, static_cast<unsigned long long>(tid), slot);
                    LogGraphicBuffer(tag, gb);
                } else {
                    DumpParcel("  reply (no NvGraphicBuffer)", out, out_size);
                }
            }
            R_SUCCEED();
        }

    }

    constinit std::atomic<bool> g_ui_dispdrv_probe{false};

    Result UiBinderMitm::TransactParcel(s32 binder_id, u32 code, u32 flags, const sf::InBuffer &parcel_in, const sf::OutBuffer &parcel_out) {
        const u64 tid = m_client_info.program_id.value;
        /* v0.7.14: the Album - its frames feed the stream like a game's */
        if (IsAlbumProgram(tid)) {
            GameBinderTransaction(m_client_info, binder_id, code, flags, parcel_in.GetPointer(), parcel_in.GetSize(), parcel_out.GetSize());
            R_RETURN(sm::mitm::ResultShouldForwardToSession());
        }
        Note(tid, binder_id, code, parcel_in.GetPointer(), parcel_in.GetSize());
        if (code == 14) {
            if (const auto *gb = FindGraphicBuffer(parcel_in.GetPointer(), parcel_in.GetSize()); gb != nullptr) {
                char tag[64];
                std::snprintf(tag, sizeof(tag), "ui binder %d (%016llx) setPreallocatedBuffer slot %d", binder_id, static_cast<unsigned long long>(tid),
                              ParseParcelSlot(parcel_in.GetPointer(), parcel_in.GetSize()));
                LogGraphicBuffer(tag, gb);
            }
        }
        if (code == 1) {
            R_RETURN(ForwardAndRead(tid, binder_id, code, parcel_in.GetPointer(), parcel_in.GetSize(), parcel_out.GetPointer(), parcel_out.GetSize(), [&] {
                return this->Transact(0, binder_id, code, flags, parcel_in.GetPointer(), parcel_in.GetSize(), parcel_out.GetPointer(), parcel_out.GetSize());
            }));
        }
        R_RETURN(sm::mitm::ResultShouldForwardToSession());
    }

    Result UiBinderMitm::TransactParcelAuto(s32 binder_id, u32 code, u32 flags, const sf::InAutoSelectBuffer &parcel_in, const sf::OutAutoSelectBuffer &parcel_out) {
        const u64 tid = m_client_info.program_id.value;
        if (IsAlbumProgram(tid)) {
            GameBinderTransaction(m_client_info, binder_id, code, flags, parcel_in.GetPointer(), parcel_in.GetSize(), parcel_out.GetSize());
            R_RETURN(sm::mitm::ResultShouldForwardToSession());
        }
        Note(tid, binder_id, code, parcel_in.GetPointer(), parcel_in.GetSize());
        if (code == 14) {
            if (const auto *gb = FindGraphicBuffer(parcel_in.GetPointer(), parcel_in.GetSize()); gb != nullptr) {
                char tag[64];
                std::snprintf(tag, sizeof(tag), "ui binder %d (%016llx) setPreallocatedBuffer slot %d", binder_id, static_cast<unsigned long long>(tid),
                              ParseParcelSlot(parcel_in.GetPointer(), parcel_in.GetSize()));
                LogGraphicBuffer(tag, gb);
            }
        }
        if (code == 1) {
            R_RETURN(ForwardAndRead(tid, binder_id, code, parcel_in.GetPointer(), parcel_in.GetSize(), parcel_out.GetPointer(), parcel_out.GetSize(), [&] {
                return this->Transact(3, binder_id, code, flags, parcel_in.GetPointer(), parcel_in.GetSize(), parcel_out.GetPointer(), parcel_out.GetSize());
            }));
        }
        R_RETURN(sm::mitm::ResultShouldForwardToSession());
    }

    void UiNoteTransaction(u64 tid, s32 id, u32 code, const void *in, size_t in_size) {
        Note(tid, id, code, static_cast<const u8 *>(in), in_size);
        if (code == 14) {
            if (const auto *gb = FindGraphicBuffer(in, in_size); gb != nullptr) {
                char tag[64];
                std::snprintf(tag, sizeof(tag), "ui relay %d (%016llx) setPreallocatedBuffer slot %d", id, static_cast<unsigned long long>(tid),
                              ParseParcelSlot(static_cast<const u8 *>(in), in_size));
                LogGraphicBuffer(tag, gb);
            }
        }
    }

    /* ---- probe4: vi's shared-layer commands --------------------------- */

    constinit std::atomic<bool> g_ui_vi_mitm{false};
    constinit SharedPresentState g_shared_present = {};

    namespace {
        constinit std::atomic<u32> g_sl_logged{0};
        bool SlLog() { return g_sl_logged.fetch_add(1, std::memory_order_relaxed) < 60; }
    }

    Result UiSysDisplayMitm::CloseSharedLayer(u64 layer) {
        LogLine("shared layer %#llx (%016llx): CloseSharedLayer", static_cast<unsigned long long>(layer), static_cast<unsigned long long>(m_client_info.program_id.value));
        R_RETURN(sm::mitm::ResultShouldForwardToSession());
    }

    Result UiSysDisplayMitm::ConnectSharedLayer(u64 layer) {
        LogLine("shared layer %#llx (%016llx): ConnectSharedLayer", static_cast<unsigned long long>(layer), static_cast<unsigned long long>(m_client_info.program_id.value));
        R_RETURN(sm::mitm::ResultShouldForwardToSession());
    }

    Result UiSysDisplayMitm::DisconnectSharedLayer(u64 layer) {
        LogLine("shared layer %#llx (%016llx): DisconnectSharedLayer", static_cast<unsigned long long>(layer), static_cast<unsigned long long>(m_client_info.program_id.value));
        R_RETURN(sm::mitm::ResultShouldForwardToSession());
    }

    Result UiSysDisplayMitm::GetSharedFrameBufferAcquirableEvent(u64 layer) {
        LogLine("shared layer %#llx (%016llx): GetSharedFrameBufferAcquirableEvent", static_cast<unsigned long long>(layer), static_cast<unsigned long long>(m_client_info.program_id.value));
        R_RETURN(sm::mitm::ResultShouldForwardToSession());
    }

    /* forwarded by hand: the reply names the slot to draw into */
    Result UiSysDisplayMitm::AcquireSharedFrameBuffer(sf::Out<SharedAcquireOut> out, u64 layer) {
        SharedAcquireOut o = {};
        const Result rc = ::ams::Result(serviceDispatchInOut(m_forward_service.get(), 8254, layer, o));
        if (R_SUCCEEDED(rc)) {
            out.SetValue(o);
            g_shared_present.acquired_slot.store(static_cast<s32>(o.slot), std::memory_order_relaxed);
            const u32 n = g_shared_present.acquires.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 4 && SlLog()) {
                u32 fc = 0, f0 = 0, f1 = 0;
                std::memcpy(std::addressof(fc), o.fence, 4);
                std::memcpy(std::addressof(f0), o.fence + 4, 4);
                std::memcpy(std::addressof(f1), o.fence + 8, 4);
                LogLine("shared layer %#llx (%016llx): Acquire #%u -> slot %llu, the layer's slots %d %d %d %d, fence %u: %u=%u",
                        static_cast<unsigned long long>(layer), static_cast<unsigned long long>(m_client_info.program_id.value), n,
                        static_cast<unsigned long long>(o.slot), o.slot_indexes[0], o.slot_indexes[1], o.slot_indexes[2], o.slot_indexes[3], fc, f0, f1);
            }
        } else if (SlLog()) {
            LogLine("shared layer %#llx: Acquire rc=0x%x", static_cast<unsigned long long>(layer), rc.GetValue());
        }
        R_RETURN(rc);
    }

    /* forwarded by hand so the record follows vi's own state change */
    Result UiSysDisplayMitm::PresentSharedFrameBuffer(const SharedPresentIn &in) {
        const Result rc = ::ams::Result(serviceDispatchIn(m_forward_service.get(), 8255, in));
        if (R_SUCCEEDED(rc)) {
            g_shared_present.slot.store(static_cast<s32>(in.slot), std::memory_order_relaxed);
            g_shared_present.layer.store(in.layer, std::memory_order_relaxed);
            g_shared_present.tick.store(armGetSystemTick(), std::memory_order_relaxed);
            const u32 n = g_shared_present.count.fetch_add(1, std::memory_order_release) + 1;
            if ((n <= 4 || n % 600 == 0) && SlLog()) {
                u32 fc = 0, f0 = 0, f1 = 0;
                std::memcpy(std::addressof(fc), in.fence, 4);
                std::memcpy(std::addressof(f0), in.fence + 4, 4);
                std::memcpy(std::addressof(f1), in.fence + 8, 4);
                LogLine("shared layer %#llx (%016llx): Present #%u slot %llu, crop (%d,%d)-(%d,%d), transform 0x%x, swap %d, fence %u: %u=%u",
                        static_cast<unsigned long long>(in.layer), static_cast<unsigned long long>(m_client_info.program_id.value), n,
                        static_cast<unsigned long long>(in.slot), in.crop[0], in.crop[1], in.crop[2], in.crop[3], in.transform, in.swap_interval, fc, f0, f1);
            }
        } else if (SlLog()) {
            LogLine("shared layer %#llx: Present slot %llu rc=0x%x", static_cast<unsigned long long>(in.layer), static_cast<unsigned long long>(in.slot), rc.GetValue());
        }
        R_RETURN(rc);
    }

    Result UiBinderMitm::AdjustRefcount(s32 binder_id, s32 add, s32 type) {
        static constinit u32 s_logged = 0;
        if (!IsAlbumProgram(m_client_info.program_id.value) && s_logged++ < 48) {
            LogLine("ui binder %d (%016llx): AdjustRefcount %+d type %d", binder_id, static_cast<unsigned long long>(m_client_info.program_id.value), add, type);
        }
        R_RETURN(sm::mitm::ResultShouldForwardToSession());
    }

    Result UiBinderMitm::GetNativeHandle(s32 binder_id, u32 what) {
        static constinit u32 s_logged = 0;
        if (!IsAlbumProgram(m_client_info.program_id.value) && s_logged++ < 48) {
            LogLine("ui binder %d (%016llx): GetNativeHandle what %u", binder_id, static_cast<unsigned long long>(m_client_info.program_id.value), what);
        }
        R_RETURN(sm::mitm::ResultShouldForwardToSession());
    }

}
