/* Private implementation: included inside applet_mitm_nv.cpp anonymous namespace.
 * VIC capture common to Ambilight and legacy UVC. Do not include from other files.
 * No change to the proven framebuffer read, fence wait, slot matching or VIC path.
 */
        /* Shared, verified VIC capture owner. */
        struct VicCaptureSession {
            u32 vfd = 0, cmd_handle = 0, vsyncpt = 0, cfg_addr = 0;
            u32 sh = 0, slot_vic = 0, arena_vic = 0;
            NvfCtx x = {};
            bool efd_open = false, keep_open = false;
            SrcDesc src{ 1920, 1080, 1920, vic::BLK_KIND_GENERIC_16Bx2, 4, vic::PIXFMT_A8R8G8B8, vic::CACHE_WIDTH_64Bx4, 1920, 1080 };
            bool corner = false;       /* handheld: read and convert the 1280x720 corner 1:1 */
            u32 seen = 0;              /* g_queue_count at the last capture */
            u64 present_tick = 0;      /* M85: g_queue_tick of the present the last capture took */
            u32 present_flip = 0;      /* v0.4.1: its transform: bit 0 flip H, bit 1 flip V */
            /* M88: waits on the present's acquire fence before reading */
            u32 fence_waits = 0, fence_timeouts = 0, fence_unknown = 0;
            u32 reread = 0;            /* v0.7.1: reads thrown away - the slot may have been reused mid-read */
            u64 fence_wait_sum = 0, fence_wait_max = 0;
            size_t arena_base = NvfArenaBase;
            u32 arena_size = NvfArenaSize;
            const NvfLayout *lay = nullptr;    /* M90: nullptr = 720p */
            const NvfLayout &Lay() const { return lay != nullptr ? *lay : Layout720(); }
            bool vic_only = false;             /* v0.7.7: webcam-any - no NVENC channel */
            u32 nvfd = 0;                      /* v0.7.10: the nvmap fd our handles live on, to free them */
            /* M87: the game's swapchain geometry. The defaults are MK8's, which
             * every run before M87 hard-coded: three 1920x1080 A8B8G8R8 slots
             * in one nvmap object, 128-row blocks. */
            u32 slot_off[8] = { 0, 0x870000, 0x10E0000 };
            u32 nslots = 3;
            u32 buf_bytes = static_cast<u32>(FbSlotSize);
            bool mk8_layout = true;        /* MK8's layout: the corner can be read alone (CornerBlockRows) */
            bool size_check_ok = true;     /* v0.7.8: any 1920x1080 surface - the 720p/1080p check runs */
            bool CornerFast() const { return corner && mk8_layout; }

            /* One nvmap object per slot (GameSurface::separate, Smash
             * Ultimate): every slot_off is 0, and slot_base + slot_off read
             * ONE buffer for every slot - each picture sent three times, 20
             * fps of picture in a 60 fps stream. And with no region exactly
             * the swapchain's size, slot_base is only the smallest region
             * that holds it - in Moonlight its texture memory, in DuckStation
             * nothing on screen. Then each slot reads its own buffer
             * instead: cand[] from FindSlotCandidates, slot_cand[k] the
             * candidate slot k reads. Until Calibrate has matched them, slot
             * k reads candidate k (one object per slot) or what slot_base +
             * slot_off[k] read before (one object), or the match the last
             * session found if its buffers are all still there. */
            static constexpr u32 MaxSlotCand = slotmatch::MaxCand;
            static constexpr u32 SigPoints = 32;
            bool sep_layout = false;       /* the geometry says one object per slot */
            bool sep = false;              /* reading per-slot buffers */
            bool sep_locked = false;       /* matched (or given up): no more sampling */
            bool still_guessed = false;    /* slot_cand from StillGuess (a still picture), not yet matched */
            bool chain_display = false;    /* v0.7.17: slot_cand is a run of buffers the display maps too */
            u32 slot_nv[8] = {};           /* v0.7.17: each slot's nvmap id (the parcels) */
            u64 slot_cpu[8] = {};          /* v0.7.17: a homebrew's slots, from libnx's records (FindLibnxNvMaps) */
            bool guess_trusted = false;    /* v0.7.3: slot_cand is the last session's match */
            u32 held_ms = 0;               /* v0.7.3: how long the first frames waited for the match */
            bool hold_done = false;        /* v0.7.3: the wait happens once a session */
            u64 cand[MaxSlotCand] = {};
            u32 ncand = 0;
            u8 slot_cand[8] = {};
            slotmatch::Matcher matcher;

            u64 SlotAddr(u64 slot_base, s32 slot) const {
                const bool valid = slot >= 0 && static_cast<u32>(slot) < nslots;
                if (sep) { return cand[slot_cand[valid ? slot : 0]]; }
                return slot_base + (valid ? slot_off[slot] : 0);
            }

            /* after SetGeometry, while attached: the per-slot buffers -
             * unless slot_base is a region exactly the swapchain's size and
             * the slots are offsets in it (MK8) */
            void PrepareSlots(::ams::svc::Handle dbg, u64 slot_base, const char *who) {
                sep = sep_locked = still_guessed = false;
                u64 span = buf_bytes;
                for (u32 k = 0; k < nslots; ++k) { if (slot_off[k] + static_cast<u64>(buf_bytes) > span) { span = slot_off[k] + static_cast<u64>(buf_bytes); } }
                ::ams::svc::MemoryInfo mi = {};
                ::ams::svc::PageInfo   pi = {};
                const bool exact = DbgQuery(dbg, std::addressof(mi), std::addressof(pi), slot_base) &&
                                   mi.base_address == slot_base && mi.size == span;
                if (!sep_layout && exact) { return; }
                /* v0.7.17: a homebrew (vi:m) - its own records say where the
                 * slots are; nothing to match */
                u64 nv_base[8] = {};
                u32 nv_found = 0;
                if (g_vim_program.load(std::memory_order_relaxed) == g_app_tid.load(std::memory_order_relaxed) && nslots <= 8) {
                    u64 *const base = nv_base;
                    nv_found = FindLibnxNvMaps(dbg, slot_nv, nslots, sep_layout ? buf_bytes : buf_bytes / 2, base);
                    if (nv_found == nslots) {
                        ncand = nslots;
                        for (u32 k = 0; k < nslots; ++k) { cand[k] = base[k] + slot_off[k]; slot_cand[k] = static_cast<u8>(k); slot_cpu[k] = cand[k]; }
                        sep = true;
                        sep_locked = true;
                        guess_trusted = true;
                        char line[200];
                        int len = std::snprintf(line, sizeof(line), "   %s: slots from the app's nvmap records:", who);
                        for (u32 k = 0; k < nslots && len > 0 && static_cast<size_t>(len) < sizeof(line); ++k) {
                            len += std::snprintf(line + len, sizeof(line) - len, " %u->0x%010llx", k, static_cast<unsigned long long>(cand[k]));
                        }
                        LogLine("%s", line);
                        return;
                    }
                }
                u32 devc[MaxSlotCand] = {};
                /* v0.7.17: more than the matcher takes, then the likeliest first.
                 * Super Mario Sunshine (3D All-Stars) has dozens of 1280x720
                 * render targets below its two slots: the 48 the list held
                 * were all of them, the match never came and the stream
                 * showed whatever the first guess read. First the region the
                 * swapchain was found in, then those mapped for the display
                 * too (2 or more devices), then the rest in address order. */
                {
                    static constexpr u32 MaxScan = 192;
                    static u64 all[MaxScan];
                    static u32 all_devc[MaxScan];
                    const u32 nall = FindSlotCandidates(dbg, buf_bytes, slot_off, nslots, sep_layout, all, MaxScan, all_devc);
                    ncand = 0;
                    if (nall <= MaxSlotCand) {
                        for (u32 c = 0; c < nall; ++c) { cand[c] = all[c]; devc[c] = all_devc[c]; }
                        ncand = nall;
                    } else {
                        bool taken[MaxScan] = {};
                        const u64 sb_end = slot_base + mi.size;
                        for (u32 pass = 0; pass < 3; ++pass) {
                            for (u32 c = 0; c < nall && ncand < MaxSlotCand; ++c) {
                                if (taken[c]) { continue; }
                                const bool in_base = sep_layout && mi.base_address == slot_base && all[c] >= slot_base && all[c] < sb_end;
                                if ((pass == 0 && in_base) || (pass == 1 && all_devc[c] >= 2) || pass == 2) {
                                    cand[ncand] = all[c]; devc[ncand] = all_devc[c]; ++ncand; taken[c] = true;
                                }
                            }
                        }
                        /* address order again - the matcher and the logs expect it */
                        for (u32 i = 1; i < ncand; ++i) {
                            for (u32 j = i; j > 0 && cand[j - 1] > cand[j]; --j) { std::swap(cand[j - 1], cand[j]); std::swap(devc[j - 1], devc[j]); }
                        }
                        LogLine("   %s: %u candidate buffers in the game's memory - kept %u: the swapchain's region, then those the display maps too", who, nall, ncand);
                    }
                }
                /* v0.7.3: one object holding the slots, in more than one region
                 * that could hold it (Sonic: two pools laid out alike) - the
                 * swapchain's region is also mapped for the display, so drop
                 * the regions mapped for one device only. v0.7.9: keep every
                 * region mapped for 2 or more, not only the most - Fire
                 * Emblem: Three Houses has an unrelated region mapped for 3
                 * and its swapchain's for 2; keeping only the 3 left nothing
                 * to match (black menus, garbage). The matcher picks among
                 * the rest. */
                if (!sep_layout && ncand > nslots) {
                    u32 kept = 0;
                    for (u32 c = 0; c < ncand; ++c) { if (devc[c] >= 2) { ++kept; } }
                    if (kept >= nslots && kept < ncand) {
                        u32 k2 = 0;
                        for (u32 c = 0; c < ncand; ++c) { if (devc[c] >= 2) { cand[k2] = cand[c]; devc[k2] = devc[c]; ++k2; } }
                        LogLine("   %s: %u of %u candidates kept - the regions mapped for 2 or more devices (the display's too)", who, kept, ncand);
                        ncand = kept;
                    }
                }
                if (ncand == MaxSlotCand) { LogLine("   %s: the candidate list is full (%u) - the slots may be past it", who, ncand); }
                if (ncand < nslots) {
                    LogLine("   %s: %u slot(s)%s, but %u candidate buffer(s) of %u B in the game's memory - reading from 0x%010llx as before",
                            who, nslots, sep_layout ? ", each its own buffer" : "", ncand, buf_bytes, static_cast<unsigned long long>(slot_base));
                    return;
                }
                for (u32 k = 0; k < nslots; ++k) {
                    slot_cand[k] = static_cast<u8>(k);
                    /* v0.7.17: one object per slot - first guess the swapchain's region, slot k its k-th buffer */
                    const u64 want_addr = sep_layout ? slot_base + static_cast<u64>(k) * buf_bytes : slot_base + slot_off[k];
                    for (u32 c = 0; c < ncand; ++c) {
                        if (cand[c] == want_addr) { slot_cand[k] = static_cast<u8>(c); break; }
                    }
                }
                /* v0.7.17: the first guess for one object per slot - a run of
                 * nslots buffers end to end (gaps of at most 256 KB), the
                 * display's mapping first. Every match logged so far was one
                 * (slot k the k-th buffer of the run); homebrew (deko3d,
                 * libnx) often never gets matched - gen1recomp flickered for a
                 * minute on "the first guess", candidates 0-2 in address
                 * order, which were not its slots. */
                chain_display = false;
                bool anchored = false;   /* v0.7.17: slots from the app's records (some of them) */
                if (sep_layout && nslots >= 2 && ncand >= nslots) {
                    auto run_at = [&](u32 c0) -> bool {
                        for (u32 k = 1; k < nslots; ++k) {
                            const u64 gap = cand[c0 + k] - cand[c0 + k - 1];
                            if (gap < buf_bytes || gap > static_cast<u64>(buf_bytes) + 0x40000) { return false; }
                        }
                        return true;
                    };
                    s32 pick = -1, pick_base = -1, pick_any = -1;
                    for (u32 c0 = 0; c0 + nslots <= ncand; ++c0) {
                        if (!run_at(c0)) { continue; }
                        bool disp = true;
                        for (u32 k = 0; k < nslots; ++k) { disp &= devc[c0 + k] >= 2; }
                        if (disp && pick < 0) { pick = static_cast<s32>(c0); }
                        if (pick_base < 0 && cand[c0] <= slot_base && slot_base < cand[c0 + nslots - 1] + buf_bytes) { pick_base = static_cast<s32>(c0); }
                        if (pick_any < 0) { pick_any = static_cast<s32>(c0); }
                    }
                    chain_display = pick >= 0;
                    if (pick < 0) { pick = pick_base >= 0 ? pick_base : pick_any; }
                    if (pick >= 0) {
                        for (u32 k = 0; k < nslots; ++k) { slot_cand[k] = static_cast<u8>(pick + k); }
                        LogLine("   %s: first guess - %u buffers end to end at 0x%010llx%s", who, nslots, static_cast<unsigned long long>(cand[pick]),
                                chain_display ? ", mapped for the display too" : (pick == pick_base ? ", where the swapchain was found" : ""));
                    }
                    /* v0.7.17: some of the slots from the app's records - the run
                     * of buffers that holds them all, the rest end to end beside them */
                    if (nv_found > 0 && nv_found < nslots && sep_layout) {
                        for (u32 c0 = 0; c0 + nslots <= ncand; ++c0) {
                            if (!run_at(c0)) { continue; }
                            bool all = true;
                            for (u32 k = 0; k < nslots && all; ++k) { all = nv_base[k] == 0 || cand[c0 + k] == nv_base[k] + slot_off[k]; }
                            if (!all) { continue; }
                            for (u32 k = 0; k < nslots; ++k) { slot_cand[k] = static_cast<u8>(c0 + k); }
                            sep = true;
                            sep_locked = true;
                            guess_trusted = true;
                            chain_display = true;
                            anchored = true;
                            LogLine("   %s: %u of %u slots from the app's records, the rest the buffers end to end beside them (0x%010llx...)", who, nv_found, nslots,
                                    static_cast<unsigned long long>(cand[c0]));
                            break;
                        }
                    }
                    /* what the candidates are, for the next look at a game that does not match */
                    char line[240];
                    int len = std::snprintf(line, sizeof(line), "   %s: candidates (address/device maps):", who);
                    for (u32 c = 0; c < ncand && c < 12 && len > 0 && static_cast<size_t>(len) < sizeof(line); ++c) {
                        len += std::snprintf(line + len, sizeof(line) - len, " %llx/%u", static_cast<unsigned long long>(cand[c]), devc[c]);
                    }
                    LogLine("%s%s", line, ncand > 12 ? " ..." : "");
                }
                bool reused = false;
                if (!anchored && g_slot_match.n == nslots && g_slot_match.buf == buf_bytes) {
                    u8 m[8] = {};
                    u32 hits = 0;
                    for (u32 k = 0; k < nslots; ++k) {
                        for (u32 c = 0; c < ncand; ++c) {
                            if (cand[c] == g_slot_match.addr[k]) { m[k] = static_cast<u8>(c); ++hits; break; }
                        }
                    }
                    if (hits == nslots) { std::memcpy(slot_cand, m, sizeof(slot_cand)); reused = true; }
                    guess_trusted = reused;
                    /* v0.7.16: the same game and the same buffer set it matched
                     * last session - keep that match. Matching again from a
                     * still picture (Doom 3 after a save, back from a loading
                     * pause) took three other regions (875/1000) and the
                     * stream showed raw memory. */
                    if (reused && g_slot_match.matched && g_slot_match.score >= 900 && g_slot_match.pid == g_app_pid.load(std::memory_order_relaxed) &&
                        g_slot_match.gen == g_game_surface.generation) {
                        sep_locked = true;
                        LogLine("   %s: the same buffers as the last session - keeping its slot match", who);
                    }
                }
                matcher.Reset(nslots, ncand);
                sep = true;
                LogLine("   %s: %u slot(s), %s; %u candidate buffer(s) of %u B from 0x%010llx%s - matching them to slots while streaming",
                        who, nslots, sep_layout ? "each its own nvmap object" : "no region exactly the swapchain's size",
                        ncand, buf_bytes, static_cast<unsigned long long>(cand[0]),
                        reused ? " (starting from the last session's match)" : "");
            }

            /* a hash of SigPoints 64-byte samples spread over the buffer;
             * *varied false when every sample is one repeated 4-byte value
             * (a cleared buffer) */
            u32 SampleSig(::ams::svc::Handle dbg, u64 base, bool *varied) const {
                const u64 step = (buf_bytes / SigPoints) & ~UINT64_C(63);
                u32 h = 2166136261u, first = 0;
                bool var = false;
                for (u32 j = 0; j < SigPoints; ++j) {
                    alignas(64) u8 b[64];
                    if (!DbgRead(dbg, b, base + j * step + ((step / 2) & ~UINT64_C(63)), sizeof(b))) { *varied = false; return 0; }
                    if (j == 0) { std::memcpy(std::addressof(first), b, sizeof(first)); }
                    for (u32 i = 0; i < sizeof(b); i += 4) { var |= std::memcmp(b + i, std::addressof(first), 4) != 0; }
                    for (u32 i = 0; i < sizeof(b); ++i) { h = (h ^ b[i]) * 16777619u; }
                }
                *varied = var;
                return h;
            }

            /* once the present of `slot` (present number `count`) is
             * finished, before it is read: a sample for the slot matcher
             * (applet_mitm_slotmatch.hpp) until it has matched every slot */
            void Calibrate(::ams::svc::Handle dbg, u32 count, s32 slot) {
                if (!sep || sep_locked || slot < 0 || static_cast<u32>(slot) >= nslots) { return; }
                u32 sig[MaxSlotCand];
                bool varied[MaxSlotCand];
                for (u32 c = 0; c < ncand; ++c) { sig[c] = this->SampleSig(dbg, cand[c], std::addressof(varied[c])); }
                /* v0.7.9: the slots drawn since the last sample, from the present
                 * ring - a stream slower than the game skips presents */
                u32 drawn = 0;
                if (matcher.have && count > matcher.last && count - matcher.last <= PresentRingSize - 2) {
                    for (u32 c = matcher.last + 1; c <= count; ++c) {
                        PresentSnap p;
                        if (c == count) { drawn |= 1u << slot; continue; }
                        if (!ReadPresent(c, std::addressof(p)) || p.slot < 0 || static_cast<u32>(p.slot) >= nslots) { drawn = 0; break; }
                        drawn |= 1u << p.slot;
                    }
                }
                matcher.AddSpan(count, drawn, sig);
                u8 m[8] = {};
                s32 weakest = 0;
                if (matcher.Match(m, std::addressof(weakest))) {
                    std::memcpy(slot_cand, m, nslots);
                    sep_locked = true;
                    g_slot_match.n = nslots;
                    g_slot_match.buf = buf_bytes;
                    g_slot_match.pid = g_app_pid.load(std::memory_order_relaxed);
                    g_slot_match.gen = g_game_surface.generation;
                    g_slot_match.matched = true;
                    g_slot_match.score = weakest;   /* only a clear match is kept for the next session */
                    char line[200];
                    int len = std::snprintf(line, sizeof(line), "   swapchain: slots matched to their buffers after %u frame intervals (weakest score %d/1000):",
                                            matcher.intervals, weakest);
                    for (u32 s = 0; s < nslots && len > 0 && static_cast<size_t>(len) < sizeof(line); ++s) {
                        g_slot_match.addr[s] = cand[m[s]];
                        len += std::snprintf(line + len, sizeof(line) - len, " %u->0x%010llx", s, static_cast<unsigned long long>(cand[m[s]]));
                    }
                    LogLine("%s", line);
                } else if (u8 g[8] = {}; !still_guessed && !chain_display && slotmatch::StillGuess(sig, varied, ncand, nslots, !sep_layout, g)) {
                    /* v0.7.17: not over a run the display maps - a homebrew's
                     * still picture matched three other buffers (0138a83c...) */
                    /* the picture stands still: show it while waiting for motion */
                    std::memcpy(slot_cand, g, nslots);
                    still_guessed = true;
                    LogLine("   swapchain: the picture stands still - slot 0 shows 0x%010llx (the same picture in every slot) until motion tells them apart",
                            static_cast<unsigned long long>(cand[g[0]]));
                } else if (matcher.GaveUp()) {
                    sep_locked = true;
                    LogLine("   swapchain: slots not told apart in %u samples (%u with a change) - keeping the first guess", matcher.samples, matcher.intervals);
                }
            }

            bool SetGeometry(const GameSurface &g, const char *who) {
                if (g.num_slots == 0 || g.buf_size == 0 || g.width == 0 || g.height == 0) {
                    LogLine("   %s: no buffer geometry recorded for this game", who); return false;
                }
                if (g.layout != 3) {
                    LogLine("   %s: swapchain layout %u is not block-linear - not supported", who, g.layout); return false;
                }
                /* v0.1.1b: up to the 1080p slot plus the spare stage after it in
                 * g_ind_buf (32-row blocks make a 1080p buffer 9.8 MB, Run AE) */
                if (g.buf_size > MaxSlotBytes || g.width > 1920 || g.height > 1088) {
                    LogLine("   %s: buffer %ux%u, %u B - larger than %zu B, not supported", who, g.width, g.height, g.buf_size, MaxSlotBytes); return false;
                }
                for (u32 k = 0; k < 8; ++k) { slot_off[k] = g.slot_offset[k]; slot_nv[k] = g.slot_nvmap[k]; }
                nslots = g.num_slots;
                /* one nvmap object per slot (Smash, Minecraft): their ids differ */
                sep_layout = false;
                for (u32 k = 1; k < g.num_slots && k < 8; ++k) { if (g.slot_nvmap[k] != g.slot_nvmap[0]) { sep_layout = true; } }
                buf_bytes = g.buf_size;
                /* MK8's A8B8G8R8 surface reads correctly as the VIC's
                 * A8R8G8B8 (M36/M83: R,G,B,A in memory); the same is assumed
                 * for any other format and said in the log */
                /* v0.1.1b: NvColorFormat A8R8G8B8 (0x100d12120) is B, G, R, A in
                 * memory - the VIC's A8B8G8R8, the other way round */
                const bool bgra = g.color_format == 0x100d12120ull;
                /* v0.7.17: libnx's RGB565 framebuffer (DBI, #42): read as 32-bit, the picture came out garbled */
                const bool rgb565 = g.color_format == 0x10a881210ull;
                const bool known_fmt = g.color_format == 0x100532120ull || bgra;
                src = SrcDesc{ g.width, g.height, g.stride_px, vic::BLK_KIND_GENERIC_16Bx2, g.block_h_log2,
                               rgb565 ? vic::PIXFMT_B5G6R5 : bgra ? vic::PIXFMT_A8B8G8R8 : vic::PIXFMT_A8R8G8B8, vic::CACHE_WIDTH_64Bx4, g.width, g.height };
                mk8_layout = g.width == 1920 && g.height == 1080 && g.stride_px == 1920 && g.block_h_log2 == 4;
                size_check_ok = g.width == 1920 && g.height == 1080 && g.stride_px >= 1920 && g.block_h_log2 <= 5;
                LogLine("   %s: game swapchain %ux%u stride %u px, block height 2^%u, %u slot(s) of %u B, format %#llx%s",
                        who, g.width, g.height, g.stride_px, g.block_h_log2, nslots, buf_bytes,
                        static_cast<unsigned long long>(g.color_format), known_fmt ? "" : rgb565 ? " (16-bit RGB565)" : " (not MK8's - colours unverified)");
                return true;
            }

            ~VicCaptureSession() {
                if (efd_open && !keep_open) {
                    if (x.enc != 0) { UnmapCmdBuffer(x.efd, x.ah); }
                    NvClose(x.efd);
                }
                if (arena_vic != 0) { UnmapCmdBuffer(vfd, x.ah); }
                if (slot_vic != 0) { UnmapCmdBuffer(vfd, sh); }
                /* v0.7.10: unmapped everywhere - free the handles, so the
                 * capture region can go back to the system. Not when a job
                 * may still be in the channel (keep_open): the arena stays
                 * referenced and CaptureRelease keeps the memory. */
                if (!keep_open && nvfd != 0) {
                    if (x.ah != 0 && NvmapFree(nvfd, x.ah)) { --g_capture_handles; }
                    if (sh != 0 && NvmapFree(nvfd, sh)) { --g_capture_handles; }
                }
            }

            bool Open(u32 v, u32 nvmap_fd, u32 cmd, u32 vsp, u32 cfg, const char *who) {
                vfd = v; cmd_handle = cmd; vsyncpt = vsp; cfg_addr = cfg;
                if (g_engine_wedged) { LogLine("   %s: an earlier engine job never completed - not submitting", who); return false; }
                if (g_ind_buf == nullptr || g_ind_size < arena_base + arena_size || cfg_addr == 0) {
                    LogLine("   %s: capture region %zu KB, need %zu KB (cfg %#x) - not running",
                            who, g_ind_size / 1024, (arena_base + arena_size) / 1024, cfg_addr);
                    return false;
                }
                u32 si = 0;
                /* the slot copy, as the VIC sees it: one 1080p slot, or more for a
                 * larger buffer (v0.1.1b) */
                const u32 slot_map = static_cast<u32>(buf_bytes > FbSlotSize ? ((buf_bytes + 0xFFFu) & ~0xFFFu) : FbSlotSize);
                if (R_FAILED(NvmapOwn(nvmap_fd, g_ind_buf, slot_map, 0, std::addressof(sh), std::addressof(si), true))) {
                    LogLine("   %s: nvmap of the slot copy failed", who); return false;
                }
                nvfd = nvmap_fd;
                ++g_capture_handles;
                HoldDeviceZero(vfd);   /* v0.7.13.1 */
                MapCmdBuffer(vfd, sh, std::addressof(slot_vic), "nvf-slot", 0);
                if (slot_vic == 0) { LogLine("   %s: slot pin returned 0 - refusing", who); return false; }

                /* zeroed and flushed BEFORE any engine writes it, so no dirty
                 * line from the memset can be written back over engine output */
                x.a = g_ind_buf + arena_base;
                x.lay = lay;
                std::memset(x.a, 0, arena_size);
                if (Lay().idr_setup != nullptr) { std::memcpy(x.a + Lay().off_setup, Lay().idr_setup, 0x1000); }
                armDCacheFlush(x.a, arena_size);
                u32 aid = 0;
                if (R_FAILED(NvmapOwn(nvmap_fd, x.a, arena_size, 0, std::addressof(x.ah), std::addressof(aid), true))) {
                    LogLine("   %s: nvmap of the %u KB arena failed", who, arena_size / 1024); return false;
                }
                ++g_capture_handles;
                MapCmdBuffer(vfd, x.ah, std::addressof(arena_vic), "nvf-arena(vic)", 0);
                if (arena_vic == 0) { LogLine("   %s: arena pin (VIC) returned 0 - refusing", who); return false; }
                if (vic_only) {
                    LogLine("   %s: arena %u KB at +%zu KB; VIC sees it at %#x; picture %#x/%#x (no encoder)",
                            who, arena_size / 1024, arena_base / 1024, arena_vic, arena_vic + Lay().off_cur, arena_vic + Lay().off_cur_uv);
                    seen = g_queue_count.load(std::memory_order_relaxed);
                    return true;
                }

                u32 nverr = 0;
                if (R_FAILED(NvOpen("/dev/nvhost-msenc", std::addressof(x.efd), std::addressof(nverr))) || nverr != 0) {
                    LogLine("   %s: /dev/nvhost-msenc open FAILED nverr=%u", who, nverr); return false;
                }
                efd_open = true;
                {
                    struct { u32 module_id; u32 syncpt; } gs = { 0, 0 };
                    nverr = 0;
                    NvIoctl(x.efd, NvHostIocChannelGetSyncpoint, std::addressof(gs), sizeof(gs), std::addressof(nverr));
                    x.esyncpt = gs.syncpt;
                }
                if (x.esyncpt == 0 || x.esyncpt == 12) { LogLine("   %s: msenc syncpt=%u - REFUSING", who, x.esyncpt); return false; }
                { struct { u32 fd; } sn = { nvmap_fd }; nverr = 0;
                  NvIoctl(x.efd, NvHostIocChannelSetNvmapFd, std::addressof(sn), sizeof(sn), std::addressof(nverr)); }
                MapCmdBuffer(x.efd, x.ah, std::addressof(x.enc), "nvf-arena(enc)", 0);
                if (x.enc == 0) { LogLine("   %s: arena pin (NVENC) returned 0 - refusing", who); return false; }
                LogLine("   %s: arena %u KB at +%zu KB; VIC sees it at %#x, NVENC at %#x (syncpt %u); picture %#x/%#x, bits %#x",
                        who, arena_size / 1024, arena_base / 1024, arena_vic, x.enc, x.esyncpt,
                        x.enc + Lay().off_cur, x.enc + Lay().off_cur_uv, x.enc + Lay().off_bits);
                seen = g_queue_count.load(std::memory_order_relaxed);
                return true;
            }

            /* M88: the game's queueBuffer carries an acquire fence: the GPU may
             * still be drawing the slot when the game queues it (the
             * compositor waits on the fence; we did not). Reading early gave
             * Run N's two artifacts - a horizontal seam where the new frame
             * met the slot's previous contents, and whole "old frames" (the
             * slot as it was 2-3 presents ago). Wait for every syncpoint in
             * the fence, polling nvhost-ctrl, at most 30 ms. */
            /* M95: something to do while the GPU finishes (collecting the
             * previous encode); called between polls until it returns true */
            EncodeIdleWork idle_work;
            bool early_poll = false;   /* v0.7.12 (PR #12): poll it while waiting for a present too */
            u64 last_flush_ns = 0;

            /* v0.7.1: was 30 ms. Pokemon Sword queues each frame ~20 ms
             * before its GPU work ends (SooraMaru's log: avg 18 ms, max 68,
             * 136 timeouts in 15 minutes), and a read past the timeout took
             * a picture the GPU was still drawing - the dot-grid frames in
             * fades and menu transitions. The GPU finishes in order, so a
             * longer wait only delays the stream; it never deadlocks it. */
            static constexpr u64 FenceTimeoutNs = UINT64_C(250000000);

            void WaitPresentFence(u32 n, const u64 fence[4]) {
                if (n == 0 || n > 4) { ++fence_unknown; return; }
                const u32 cfd = CtrlFd();
                if (cfd == 0) { ++fence_unknown; return; }
                const u64 t0 = armTicksToNs(armGetSystemTick());
                bool timed_out = false;
                for (u32 k = 0; k < n; ++k) {
                    const u64 f = fence[k];
                    const u32 id = static_cast<u32>(f >> 32), want = static_cast<u32>(f);
                    if (id == 0xFFFFFFFFu || id >= 192) { continue; }   /* an unused entry */
                    for (;;) {
                        struct { u32 id; u32 value; } r = { id, 0 };
                        u32 e = 0;
                        if (R_FAILED(NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e))) || e != 0) { break; }
                        if (static_cast<s32>(r.value - want) >= 0) { break; }
                        if (armTicksToNs(armGetSystemTick()) - t0 > FenceTimeoutNs) { timed_out = true; break; }
                        if (idle_work.Poll()) { continue; }
                        os::SleepThread(TimeSpan::FromMicroSeconds(250));
                    }
                }
                const u64 w = armTicksToNs(armGetSystemTick()) - t0;
                ++fence_waits;
                fence_wait_sum += w;
                if (w > fence_wait_max) { fence_wait_max = w; }
                if (timed_out) { ++fence_timeouts; }
            }

            /* M96: 1 = every syncpoint reached, 0 = the GPU is still drawing,
             * -1 = no readable fence (treated as done, as M88 did) */
            static int FenceState(u32 n, const u64 fence[4]) {
                if (n == 0 || n > 4) { return -1; }
                const u32 cfd = CtrlFd();
                if (cfd == 0) { return -1; }
                for (u32 k = 0; k < n; ++k) {
                    const u32 id = static_cast<u32>(fence[k] >> 32), want = static_cast<u32>(fence[k]);
                    if (id == 0xFFFFFFFFu || id >= 192) { continue; }
                    struct { u32 id; u32 value; } r = { id, 0 };
                    u32 e = 0;
                    if (R_FAILED(NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e))) || e != 0) { return -1; }
                    if (static_cast<s32>(r.value - want) < 0) { return 0; }
                }
                return 1;
            }

            struct PresentSnap { u32 count; s32 slot; u32 fence_n; u64 fence[4]; u64 tick; u32 transform; u32 crop; u32 crop_c; };

            /* present c from the ring; false if it was overwritten (8+ behind) */
            static bool ReadPresent(u32 c, PresentSnap *out) {
                PresentRec &r = g_present_ring[c % PresentRingSize];
                for (u32 tries = 0; tries < 8; ++tries) {
                    const u32 s1 = r.seq.load(std::memory_order_acquire);
                    if (s1 & 1) { continue; }
                    out->count   = r.count.load(std::memory_order_relaxed);
                    out->slot    = r.slot.load(std::memory_order_relaxed);
                    out->fence_n = r.fence_n.load(std::memory_order_relaxed);
                    for (u32 k = 0; k < 4; ++k) { out->fence[k] = r.fence[k].load(std::memory_order_relaxed); }
                    out->tick    = r.tick.load(std::memory_order_relaxed);
                    out->transform = r.transform.load(std::memory_order_relaxed);
                    out->crop    = r.crop.load(std::memory_order_relaxed);
                    out->crop_c  = r.crop_c.load(std::memory_order_relaxed);
                    std::atomic_thread_fence(std::memory_order_acquire);
                    if (r.seq.load(std::memory_order_relaxed) == s1) { return out->count == c; }
                }
                return false;
            }

            /* M96: how each capture chose its present */
            u32 pick_ready = 0, pick_waited = 0, pick_legacy = 0;

            /* M96: read in 2 MB pieces. One 8 MB ReadDebugProcessMemory holds
             * the game's page-table lock for the whole ~8.5 ms copy
             * (KPageTableBase::ReadDebugMemory); pieces let the game in
             * between. A few extra SVCs, microseconds. */
            static constexpr u32 ReadPiece = 0x200000;

            /* wait for a new present (M88: up to 50 ms, so a 30 fps game is not
             * re-sent at ~45 fps), then its fence, then read the slot.
             *
             * M96: which present. M89 took the LATEST queued one and waited on
             * its fence; when the game queues ahead of the GPU (MK8 at 1080p)
             * that skipped the finished frame before it and waited ~5 ms for
             * the next (Run V: fence avg 6.7 ms, 2246 of ~11300 presents
             * skipped, 47 fps). Now: the newest present since the last capture
             * whose fence has been reached; if none has, the OLDEST one - the
             * GPU finishes presents in order, so that is the next to be done.
             * The slot is safe to read: a finished present stays on screen or
             * queued until the one after it is drawn, and its slot can only be
             * drawn into again after the present after THAT. */
            bool SlotMayBeReused(u32 c) const {
                PresentSnap next;
                if (!ReadPresent(c + 1, std::addressof(next))) { return false; }
                if (FenceState(next.fence_n, next.fence) != 1) { return false; }
                return nslots <= 2 || g_queue_count.load(std::memory_order_acquire) - c >= nslots - 1;
            }

            /* v0.7.3: before the first frame of a buffer set the matcher has
             * not seen, watch presents go by - every one, in order, once its
             * fence is reached - until it matches or sees a still picture, for
             * up to 2.5 s. Sonic Frontiers keeps both slots 32 MB into one
             * large object: the first guess was another region and the first
             * ~0.75 s of the stream (22 frames at 30 fps) was noise. */
            void HoldForMatch(::ams::svc::Handle dbg) {
                if (hold_done || !sep || sep_locked || still_guessed || guess_trusted) { return; }
                hold_done = true;
                const u64 t0 = armTicksToNs(armGetSystemTick());
                u32 c = g_queue_count.load(std::memory_order_acquire);
                while (!sep_locked && !still_guessed && armTicksToNs(armGetSystemTick()) - t0 < UINT64_C(2500000000)) {
                    if (g_queue_count.load(std::memory_order_acquire) == c) { os::SleepThread(TimeSpan::FromMilliSeconds(1)); continue; }
                    ++c;
                    PresentSnap p;
                    if (!ReadPresent(c, std::addressof(p))) { c = g_queue_count.load(std::memory_order_acquire); continue; }   /* fell behind the ring */
                    for (u32 w = 0; w < 100 && FenceState(p.fence_n, p.fence) == 0; ++w) { os::SleepThread(TimeSpan::FromMicroSeconds(500)); }
                    this->Calibrate(dbg, p.count, p.slot);
                }
                held_ms = static_cast<u32>((armTicksToNs(armGetSystemTick()) - t0) / 1000000);
                LogLine("   swapchain: the first frame waited %u ms for the slot match (%s)", held_ms,
                        sep_locked ? "matched" : still_guessed ? "a still picture" : "not yet - streaming the first guess");
                seen = g_queue_count.load(std::memory_order_acquire) - 1;   /* the latest present is new */
            }

            bool Capture(::ams::svc::Handle dbg, u64 slot_base, u64 *read_ns, u32 *sig, u32 *dropped = nullptr) {
                this->HoldForMatch(dbg);
                for (u32 spins = 0; g_queue_count.load(std::memory_order_acquire) == seen && spins < 50; ++spins) {
                    if (early_poll) { idle_work.Poll(); }
                    os::SleepThread(TimeSpan::FromMilliSeconds(1));
                }
                for (u32 retry = 0; ; ) {
                /* collect before picking a slot: the USB send can block, so
                 * never in the middle of a framebuffer copy (PR #12) */
                if (early_poll) { idle_work.Poll(); }
                const u32 qnow = g_queue_count.load(std::memory_order_acquire);
                if (qnow == seen) { break; }
                {
                    const u32 back = qnow - seen;
                    const u32 lo = back > PresentRingSize - 2 ? qnow - (PresentRingSize - 2) : seen;   /* candidates (lo, qnow] */
                    PresentSnap pick = {}, oldest = {};
                    bool have_pick = false, have_oldest = false;
                    for (u32 c = qnow; c != lo; --c) {
                        PresentSnap p;
                        if (!ReadPresent(c, std::addressof(p))) { continue; }
                        if (FenceState(p.fence_n, p.fence) != 0) { pick = p; have_pick = true; break; }
                        oldest = p; have_oldest = true;
                    }
                    if (!have_pick && have_oldest) {
                        this->WaitPresentFence(oldest.fence_n, oldest.fence);
                        pick = oldest; have_pick = true;
                        ++pick_waited;
                    } else if (have_pick) {
                        ++fence_waits;     /* a wait of 0, so the average stays per capture */
                        ++pick_ready;
                    }
                    if (have_pick) {
                        if (dropped != nullptr && pick.count - seen > 1) { *dropped += pick.count - seen - 1; }
                        seen = pick.count;
                        present_tick = pick.tick;
                        present_flip = pick.transform & 3;
                        cap_crop = pick.crop;
                        cap_crop_c = pick.crop_c;
                        const s32 slot = pick.slot;
                        this->Calibrate(dbg, pick.count, slot);
                        if (!this->ReadSlot(dbg, this->SlotAddr(slot_base, slot), read_ns, sig)) { return false; }
                        /* v0.7.1: the slot is the game's again once the present
                         * after it is on screen - and that needs its fence. If
                         * it was reached before the read ended (a slow read, a
                         * double-buffered game), the read may hold some of the
                         * NEXT picture: take that newer one instead. Triple
                         * buffering needs two presents queued after ours. */
                        if (retry < 2 && SlotMayBeReused(pick.count) && ChangedSinceRead(dbg)) {
                            ++reread;
                            ++retry;
                            continue;
                        }
                        return true;
                    }
                }
                break;
                }
                ++pick_legacy;
                /* M89: slot and fence as ONE snapshot, taken before the wait.
                 * M88 waited on the fence and only then read the slot; a
                 * present landing during the wait (avg 5.7 ms in MK8) swapped
                 * in a newer slot the GPU had not finished - still holding
                 * the picture from three presents back. Run O: 17 frames in
                 * 3000 matching the frame two before them (A, B, A, B). */
                u32 now = 0, fn = 0;
                u64 fence[4] = {};
                s32 slot = -1;
                for (u32 tries = 0; tries < 8; ++tries) {
                    const u32 s1 = g_queue_seq.load(std::memory_order_acquire);
                    if (s1 & 1) { continue; }
                    now  = g_queue_count.load(std::memory_order_acquire);
                    slot = g_queue_slot.load(std::memory_order_relaxed);
                    fn   = g_queue_fence_n.load(std::memory_order_relaxed);
                    for (u32 k = 0; k < 4; ++k) { fence[k] = g_queue_fence[k].load(std::memory_order_relaxed); }
                    present_tick = g_queue_tick.load(std::memory_order_relaxed);
                    present_flip = g_queue_transform.load(std::memory_order_relaxed) & 3;
                    cap_crop = g_queue_crop_origin0.load(std::memory_order_relaxed) ? g_queue_crop_wh.load(std::memory_order_relaxed) : 0;
                    cap_crop_c = g_queue_crop_c.load(std::memory_order_relaxed);
                    std::atomic_thread_fence(std::memory_order_acquire);
                    if (g_queue_seq.load(std::memory_order_relaxed) == s1) { break; }
                }
                if (now != seen) { this->WaitPresentFence(fn, fence); }
                if (dropped != nullptr && now - seen > 1) { *dropped += now - seen - 1; }
                seen = now;
                this->Calibrate(dbg, now, slot);
                return this->ReadSlot(dbg, this->SlotAddr(slot_base, slot), read_ns, sig);
            }

            /* v0.7.3, from WillMidia's PR #4: did the slot change after it
             * was copied? Eight 64-byte samples of the slot against the copy -
             * the cheap confirmation before a re-read (SlotMayBeReused only
             * knows the slot MAY be the game's again: Smash's menus re-read
             * 78 frames in one session, each a full copy). */
            u64 last_read = 0;

            bool ChangedSinceRead(::ams::svc::Handle dbg) const {
                for (u32 k = 0; k < 8; ++k) {
                    const u64 off = CornerFast() ? static_cast<u64>(k % CornerBlockRows) * FbBlockRow + ((static_cast<u64>(k) * 0x2340u % CornerBlockRowBytes) & ~UINT64_C(63))
                                           : ((static_cast<u64>(last_read_bytes) / 8 * k + 1024) & ~UINT64_C(63));
                    alignas(64) u8 b[64];
                    if (!DbgRead(dbg, b, last_read + off, sizeof(b))) { return false; }
                    if (std::memcmp(b, g_ind_buf + off, sizeof(b)) != 0) { return true; }
                }
                return false;
            }

            /* v0.7.13: a surface other than 1920x1080 is read only down to the
             * last block row holding what the game shows - SMO handheld: 6 of
             * its 8 (768 of 900 rows), a quarter less to copy. The rows below
             * stay stale in g_ind_buf; the VIC never samples them. */
            u32 ReadBytes() const {
                if (size_check_ok) { return buf_bytes; }
                const u32 rows = PresentRect(src, cap_crop) & 0xFFFF;
                const u32 blk_rows = 8u << src.blk_h_log2;
                const u32 row_bytes = src.stride_px * 4;
                const u64 blk_bytes = static_cast<u64>(row_bytes) * blk_rows;
                if (row_bytes % 64 != 0 || blk_bytes == 0 || buf_bytes % blk_bytes != 0) { return buf_bytes; }
                const u64 n = (static_cast<u64>(rows) + blk_rows - 1) / blk_rows * blk_bytes;
                return n < buf_bytes ? static_cast<u32>(n) : buf_bytes;
            }

            bool ReadSlot(::ams::svc::Handle dbg, u64 base, u64 *read_ns, u32 *sig) {
                const u64 t0 = armTicksToNs(armGetSystemTick());
                last_read = base;
                /* v0.7.14: never past the capture region (the first frame is
                 * read before Open checks the region's size) */
                {
                    const size_t need = CornerFast() ? (CornerBlockRows - 1) * static_cast<size_t>(FbBlockRow) + CornerBlockRowBytes
                                                     : static_cast<size_t>(ReadBytes());
                    if (g_ind_buf == nullptr || need > g_ind_size) {
                        static bool said = false;
                        if (!said) { LogLine("   slot read of %zu KB refused: the capture region is %zu KB", need / 1024, g_ind_size / 1024); said = true; }
                        return false;
                    }
                }
                if (CornerFast()) {
                    for (u32 br = 0; br < CornerBlockRows; ++br) {
                        u8 *dst = g_ind_buf + br * FbBlockRow;
                        if (!DbgRead(dbg, dst, base + br * FbBlockRow, CornerBlockRowBytes)) {
                            return false;
                        }
                        armDCacheFlush(dst, CornerBlockRowBytes);
                    }
                } else {
                    const u32 bytes = ReadBytes();
                    last_read_bytes = bytes;
                    for (u32 off = 0; off < bytes; off += ReadPiece) {
                        const u32 n = bytes - off < ReadPiece ? bytes - off : ReadPiece;
                        if (!DbgRead(dbg, g_ind_buf + off, base + off, n)) {
                            return false;
                        }
                    }
                    const u64 tf = armTicksToNs(armGetSystemTick());
                    armDCacheFlush(g_ind_buf, bytes);
                    last_flush_ns = armTicksToNs(armGetSystemTick()) - tf;
                }
                u32 h = 0;
                for (u32 k = 0; k < 8192; k += 8) { h = h * 31u + g_ind_buf[k]; }
                *sig = h;
                *read_ns = armTicksToNs(armGetSystemTick()) - t0;
                return true;
            }

            /* after the first full capture: handheld content -> corner mode */
            void Decide(const char *who) {
                if (!size_check_ok) {
                    corner = false;
                    LogLine("   %s: the VIC converts the whole %ux%u picture to 1280x720", who, src.w, src.h);
                    return;
                }
                u32 samples = 0;
                corner = SlotContentIs720(g_ind_buf, std::addressof(samples), src.stride_px, src.blk_h_log2);
                /* v0.7.3: a size check just restarted the stream - its finding
                 * stands (the first frame alone cannot see old docked content) */
                if (g_size_hint.corner >= 0 && g_size_hint.pid == g_app_pid.load(std::memory_order_relaxed) && g_size_hint.gen == g_game_surface.generation) {
                    if ((g_size_hint.corner != 0) != corner) {
                        LogLine("   %s: the last check found a %s picture - starting at that size", who, g_size_hint.corner ? "720p" : "1080p");
                    }
                    corner = g_size_hint.corner != 0;
                }
                /* v0.7.9: a game whose crop follows its mode - its crop decides */
                {
                    const u64 pid = g_app_pid.load(std::memory_order_relaxed);
                    const u32 crop = g_queue_crop_wh.load(std::memory_order_relaxed);
                    if (pid != 0 && g_crop_full_pid.load(std::memory_order_relaxed) == pid && crop != 0) {
                        const bool small = (crop >> 16) <= 1280 && (crop & 0xFFFF) <= 720;
                        if (small != corner) { LogLine("   %s: the game's picture size is %ux%u - starting at that size", who, crop >> 16, crop & 0xFFFF); }
                        corner = small;
                    }
                }
                if (corner) { src.rect_w = NvfW; src.rect_h = NvfH; }
                LogLine("   %s: content %s (%u samples outside the top-left 1280x720 %s) -> %s",
                        who, corner ? "1280x720 (handheld)" : "1920x1080 (docked)", samples, corner ? "all zero" : "not all zero",
                        corner ? (mk8_layout ? "the VIC converts the corner 1:1; later frames read only its 3.9 MB" : "the VIC converts the corner 1:1")
                               : "the VIC scales it to 1280x720");
            }

            /* v0.7.13: a surface other than 1920x1080 - the VIC samples only
             * what the game presents. Super Mario Odyssey draws into 1600x900
             * and presents its top-left 1280x720 in handheld (the whole 1600x900
             * docked); scaling the whole surface left the picture in the top-left
             * 80% of the stream (creeper3010, #7). Followed on every frame, so a
             * docked/handheld switch follows too. Only a 16:9 crop from (0,0)
             * that fits the surface: 1920x1080 surfaces keep the size check
             * (Smash presents 1280x720 while drawing 1920x1080). */
            u32 crop_used = 0;
            u32 cap_crop = 0;          /* v0.7.13: the captured present's crop (PresentRec::crop) */
            u32 cap_crop_c = 0;        /* v0.7.16: ... when it sits in the middle (PresentRec::crop_c) */
            u32 last_read_bytes = 0;   /* v0.7.13: how much of the slot ReadSlot copied */
            /* crop: the captured present's (cap_crop) */
            static u32 PresentRect(const SrcDesc &d, u32 crop) {
                const u32 cw = crop >> 16, ch = crop & 0xFFFF;
                const bool ok = crop != 0
                             && cw >= 320 && ch >= 180 && cw <= d.w && ch <= d.h
                             && (cw * 9 > ch * 16 ? cw * 9 - ch * 16 : ch * 16 - cw * 9) <= cw * 9 / 50;
                return ok ? crop : (d.w << 16 | d.h);
            }
            void CropToPresent(SrcDesc &d) {
                const u32 use = PresentRect(d, cap_crop);
                if (use != crop_used) {
                    if (crop_used != 0 || use != (d.w << 16 | d.h)) {
                        LogLine("   nvf: the game presents %ux%u of its %ux%u picture - the VIC %s", use >> 16, use & 0xFFFF, d.w, d.h,
                                use == (d.w << 16 | d.h) ? "converts all of it" : "converts only that part");
                    }
                    crop_used = use;
                }
                d.rect_w = use >> 16;
                d.rect_h = use & 0xFFFF;
            }

            /* v0.7.15: dynamic resolution on a 1920x1080 surface - Super Mario
             * Galaxy draws 992x704 ... 1280x720 into the handheld corner (and
             * less than 1920x1080 docked) and presents just that part, which
             * the screen stretches; the rest of the slot holds older, bigger
             * frames (stripes at the right and the bottom of the stream). The
             * VIC samples only the present's crop, scaled to the whole stream.
             * Only a crop inside what the size check picked, smaller than it,
             * of a sane shape (1.2 to 2.4 wide per tall): Smash presents
             * 1280x720 while drawing 1920x1080, so exactly 1280x720 is left
             * to the size check. 0: no such crop. */
            /* v0.7.16: who scales it. The PC viewer does, better: the VIC here
             * has no filter table (FilterLength 2 came out "very blurry",
             * ovl4), so its stretch is blocky, and with the crop changing every
             * frame the textures shimmer (video, 1080p). For the viewer the
             * picture stays 1:1 - as sharp as before - and the crop goes in
             * the frame's header (dyn_out); the viewer shows only that part,
             * scaled with its own filter / FSR. A webcam (UVC) cannot be told:
             * there the VIC scales (dyn_vic). */
            bool dyn_vic = true;
            u32 dyn_out = 0;           /* this frame's crop in stream pixels (w << 16 | h), 0: the whole picture */
            bool dyn_center = false;   /* v0.7.16: ... in the middle of the picture, not from its top left */
            u32 center_frames = 0;     /* v0.7.16: frames sent with a centred crop this session */
            u32 dyn_frames = 0;        /* frames sampled through a dynamic crop this session */
            u32 dyn_min = 0, dyn_max = 0;   /* their smallest and biggest crop (w << 16 | h), for the log */
            static u32 DynamicRect(const SrcDesc &d, u32 crop) {
                const u32 cw = crop >> 16, ch = crop & 0xFFFF;
                const bool ok = crop != 0 && cw >= 320 && ch >= 180
                             && cw <= d.rect_w && ch <= d.rect_h && (cw < d.rect_w || ch < d.rect_h)
                             && !(cw == 1280 && ch == 720)
                             && cw * 10 >= ch * 12 && cw * 10 <= ch * 24;
                return ok ? crop : 0;
            }
            /* v0.7.16: Kingdom Hearts presents a part in the MIDDLE of its
             * surface - (96,54)-(1824,1026) down to (480,270)-(1440,810) as it
             * zooms in, and the screen shows only that part, stretched; the
             * rest of the slot is old picture (the glitch at the edges). The
             * whole surface is still converted 1:1; the viewer is told the
             * middle part (header bit 6 with the crop) and shows only that. */
            void CropCentered(SrcDesc &d, const OutDesc &out) {
                if (dyn_out != 0 || cap_crop_c == 0 || d.rect_w != d.w || d.rect_h != d.h) { return; }
                const u32 cw = cap_crop_c >> 16, ch = cap_crop_c & 0xFFFF;
                if (cw < 320 || ch < 180 || cw > d.w || ch > d.h) { return; }
                /* v0.7.17: a viewer that does not draw crops (no SFTC - an older
                 * viewer, the Android app, webcam mode): the VIC samples only the
                 * middle part and scales it to the whole stream picture. Before,
                 * the whole surface went out, old picture round the edges
                 * (Kingdom Hearts II with an older viewer). */
                if (dyn_vic) {
                    if (cw >= d.w && ch >= d.h) { return; }
                    if (center_frames++ == 0) {
                        LogLine("   nvf: the game presents a part in the middle of its %ux%u picture (%ux%u) - the VIC scales that part to the stream", d.w, d.h, cw, ch);
                    }
                    d.rect_x = (d.w - cw) / 2;
                    d.rect_y = (d.h - ch) / 2;
                    d.rect_w = cw;
                    d.rect_h = ch;
                    return;
                }
                const u32 ow = static_cast<u32>(static_cast<u64>(cw) * out.w / d.w) & ~1u;
                const u32 oh = static_cast<u32>(static_cast<u64>(ch) * out.h / d.h) & ~1u;
                if (ow == 0 || oh == 0 || (ow >= out.w && oh >= out.h)) { return; }
                if (center_frames++ == 0) {
                    LogLine("   nvf: the game presents a part in the middle of its %ux%u picture (%ux%u) - the viewer shows only that part", d.w, d.h, cw, ch);
                }
                dyn_out = ow << 16 | oh;
                dyn_center = true;
            }

            void CropDynamic(SrcDesc &d, const OutDesc &out) {
                dyn_out = 0;
                const u32 use = DynamicRect(d, cap_crop);
                if (use == 0) { return; }
                if (dyn_frames++ == 0) {
                    LogLine("   nvf: dynamic resolution - the game presents %ux%u of the %ux%u it was sized for; %s",
                            use >> 16, use & 0xFFFF, d.rect_w, d.rect_h,
                            dyn_vic ? "the VIC scales the presented part to the stream" : "the viewer shows only that part (the crop goes in each frame's header)");
                }
                const u32 area = (use >> 16) * (use & 0xFFFF);
                if (dyn_min == 0 || area < (dyn_min >> 16) * (dyn_min & 0xFFFF)) { dyn_min = use; }
                if (area > (dyn_max >> 16) * (dyn_max & 0xFFFF)) { dyn_max = use; }
                if (!dyn_vic) {
                    /* in the stream's pixels (the VIC may still scale the whole
                     * picture: 1080p to 720p), even for the 4:2:0 chroma */
                    const u32 ow = static_cast<u32>(static_cast<u64>(use >> 16) * out.w / d.rect_w) & ~1u;
                    const u32 oh = static_cast<u32>(static_cast<u64>(use & 0xFFFF) * out.h / d.rect_h) & ~1u;
                    if (ow != 0 && oh != 0 && (ow < out.w || oh < out.h)) { dyn_out = ow << 16 | oh; }
                    return;
                }
                d.rect_w = use >> 16;
                d.rect_h = use & 0xFFFF;
            }

            /* RGB -> BT.709 NV12, 16-row block-linear, into the arena */
            bool Vic(const char *stage, u32 cur = 0) {
                g_strip_src = src;
                g_strip_out = Lay().out;
                dyn_out = 0;
                dyn_center = false;
                /* v0.7.16: a viewer that never said it draws crops (SFTC) gets them scaled here */
                dyn_vic = g_uvc_mode || !g_viewer_crop.load(std::memory_order_relaxed);
                if (!size_check_ok) { CropToPresent(g_strip_src); }
                else { CropDynamic(g_strip_src, g_strip_out); }   /* v0.7.15 */
                CropCentered(g_strip_src, g_strip_out);             /* v0.7.16 */
                g_strip_flip = present_flip;
                /* v0.7.14: the overlays showing now, over the game (not on a
                 * flipped present: the output flip would mirror them) */
                BlendSlot ov[MaxOverlaySlots];
                g_ovl_nslots = present_flip == 0 ? OverlaySlotsFor(g_strip_out, ov) : 0;
                g_ovl_slots = g_ovl_nslots != 0 ? ov : nullptr;
                ON_SCOPE_EXIT { g_strip_flip = 0; g_ovl_slots = nullptr; g_ovl_nslots = 0; };
                JobCtx vc{ vfd, cmd_handle, vsyncpt, cfg_addr, arena_vic + (cur != 0 ? cur : Lay().off_cur), slot_vic, 0, 0, 0, 0, SelfSrc };
                vc.out_is_dst_buf = false;
                g_csc_override = std::addressof(NvfBt709);
                const bool ok = RunOneJob(stage, VicJob::BlitStrip, true, vc);
                g_csc_override = nullptr;
                return ok;
            }

            void SetSetupByte(u32 off, u8 v) {
                x.a[NvfOffSetup + off] = v;
                armDCacheFlush(x.a + NvfOffSetup + (off & ~0x3Fu), 0x40);
            }

            /* true: the job finished after all - the engine is not wedged */
            bool Stall(const char *who, const char *what) {
                LogLine("   %s: %s: no status from our job in 1 s.", who, what);
                u32 now = 0;
                /* v0.7.1: late is not wedged. In Minecraft our job waited behind
                 * someone else's (grc's recorder, most likely: the syncpoint went
                 * 64 past our fence with nothing more of ours submitted) and was
                 * done within a second - but this used to end streaming for the
                 * whole boot. Give it 3 s more; if the fence is reached the job
                 * is over, the channel can close normally, and the next session
                 * starts afresh. */
                if (const u32 cfd = CtrlFd(); cfd != 0) {
                    const u64 w0 = armTicksToNs(armGetSystemTick());
                    for (;;) {
                        struct { u32 id; u32 value; } r = { x.esyncpt, 0 };
                        u32 e = 0;
                        NvIoctl(cfd, NvHostIocCtrlSyncptRead, std::addressof(r), sizeof(r), std::addressof(e));
                        now = r.value;
                        if (e == 0 && static_cast<s32>(now - g_nvf_last_fence) >= 0) {
                            LogLine("   %s: the job finished %llu ms later (syncpt %u now %u, fence %u) - NVENC was busy, not wedged; the stream restarts",
                                    who, static_cast<unsigned long long>((armTicksToNs(armGetSystemTick()) - w0) / 1000000), x.esyncpt, now, g_nvf_last_fence);
                            return true;
                        }
                        if (armTicksToNs(armGetSystemTick()) - w0 > UINT64_C(3000000000)) { break; }
                        os::SleepThread(TimeSpan::FromMilliSeconds(10));
                    }
                }
                g_engine_wedged = true;
                keep_open = true;
                LogLine("   %s: still not done after 3 s more. Leaving the NVENC channel OPEN (M74).", who);
                LogLine("   %s: syncpt %u now %u, fence %u %s; status picture index %#x %s", who, x.esyncpt, now,
                        g_nvf_last_fence, g_nvf_last_fence_done ? "REACHED" : "not reached",
                        g_nvf_last_pic, g_nvf_last_ours ? "(ours)" : "(not ours - the engine never wrote its status)");
                LogLine("   No further engine work this boot.");
                return false;
            }

            /* the clock, re-ensured: before the first submit, and in a long run
             * whenever a periodic read finds it below 400 MHz */
            bool EnsureClock(const char *who, u32 *hz) {
                const u32 ensured = ClockEnsure(PcvModule_NVENC, who);
                *hz = 0;
                return ensured != 0 && ClockRateOf(PcvModule_NVENC, hz) && *hz != 0;
            }
        };

