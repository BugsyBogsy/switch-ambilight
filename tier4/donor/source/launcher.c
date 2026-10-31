/*
 * v0.7.13: the launcher. The module (0100000000000C20) used to start at boot
 * through its own boot2.flag, and its ~2.6-3.4 MB came out of the System
 * resource-limit group before the system had finished booting. About 22 s in,
 * the system launches a program that needs ~7.2 MB of that group (Run
 * v0713-ballast4-late); on a console where other sysmodules leave less, that
 * launch fails and am fatals with 2001-0132 (#15, GBAtemp; reproduced here
 * with 4 MB held from boot).
 *
 * So this program holds the boot2.flag instead. It lives in the Applet group
 * (nothing of the System group), waits until the HOME menu is up and the
 * System group has settled, and only then starts the module - when there is
 * room for it and 1 MB to spare. Otherwise the module does not start this
 * boot, and config/switch-frame-tap/launcher.txt says why. Either way the
 * console boots as it would without Switch Frame Tap.
 */
#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SFT_TID      0x0100000000000C20ull
#define QLAUNCH_TID  0x0100000000001000ull
#define SPARE_BYTES  (1024ull * 1024)
/* used when exefs.nsp cannot be read: v0.7.13's measured footprint */
#define FALLBACK_FOOTPRINT (2700ull * 1024)

static u64 UptimeMs(void) { return armTicksToNs(armGetSystemTick()) / 1000000ull; }

/* pm:dmnt GetProcessId by its id on 5.0.0+ (2). Not libnx's
 * pmdmntGetProcessId: this program skips libnx's usual start-up, so the
 * firmware version is unknown and libnx would send the pre-5.0 id - which is
 * HookToCreateProcess now (test6: the donor took itself for the launcher). */
bool Running(u64 tid) {
    u64 pid = 0;
    return R_SUCCEEDED(serviceDispatchInOut(pmdmntGetServiceSession(), 2, tid, pid)) && pid != 0;
}

/* the System group's memory (Atmosphère's pm:dmnt extension) */
static bool SystemMemory(s64 *cur, s64 *lim) {
    const struct { u32 group, resource; } in = { 0 /* System */, 0 /* PhysicalMemoryMax */ };
    struct { s64 cur, lim; } out = { 0, 0 };
    if (R_FAILED(serviceDispatchInOut(pmdmntGetServiceSession(), 65001, in, out))) { return false; }
    *cur = out.cur;
    *lim = out.lim;
    return true;
}

static u32 Rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static u64 Rd64(const u8 *p) { u64 v; memcpy(&v, p, 8); return v; }

/* The module's memory at start: its main NSO's segments and .bss, read from
 * exefs.nsp (PFS0), page-rounded - so the check follows every new version. */
static u64 ModuleFootprint(FsFileSystem *sd) {
    FsFile f;
    if (R_FAILED(fsFsOpenFile(sd, "/atmosphere/contents/0100000000000C20/exefs.nsp", FsOpenMode_Read, &f))) { return 0; }
    static u8 hdr[0x200];
    u64 n = 0;
    u64 size = 0;
    if (R_FAILED(fsFileRead(&f, 0, hdr, sizeof(hdr), FsReadOption_None, &n)) || n < 0x10 || memcmp(hdr, "PFS0", 4) != 0) { fsFileClose(&f); return 0; }
    const u32 num = Rd32(hdr + 4), strsz = Rd32(hdr + 8);
    const u64 names = 0x10 + (u64)num * 0x18, data = names + strsz;
    for (u32 i = 0; i < num && names + strsz <= n; ++i) {
        const u8 *e = hdr + 0x10 + i * 0x18;
        const u32 name_off = Rd32(e + 0x10);
        if (names + name_off + 5 > n || memcmp(hdr + names + name_off, "main", 5) != 0) { continue; }
        u8 nso[0x40];
        if (R_FAILED(fsFileRead(&f, data + Rd64(e), nso, sizeof(nso), FsReadOption_None, &n)) || n < sizeof(nso) || memcmp(nso, "NSO0", 4) != 0) { break; }
        const u64 data_end = (u64)Rd32(nso + 0x34) + Rd32(nso + 0x38);   /* data segment: memory offset + size */
        const u64 bss = Rd32(nso + 0x3C);
        size = ((data_end + 0xFFF) & ~0xFFFull) + ((bss + 0xFFF) & ~0xFFFull);
        break;
    }
    fsFileClose(&f);
    return size;
}

static void WriteText(FsFileSystem *sd, const char *path, const char *text) {
    fsFsCreateDirectory(sd, "/config");
    fsFsCreateDirectory(sd, "/config/switch-frame-tap");
    fsFsDeleteFile(sd, path);
    const size_t len = strlen(text);
    if (R_FAILED(fsFsCreateFile(sd, path, len, 0))) { return; }
    FsFile f;
    if (R_FAILED(fsFsOpenFile(sd, path, FsOpenMode_Write, &f))) { return; }
    fsFileWrite(&f, 0, text, len, FsWriteOption_Flush);
    fsFileClose(&f);
}

/* test only: config/switch-frame-tap/launcher-test.txt = N makes the check
 * see N MB less free, to try the refusal on a console with room */
static u64 TestLessBytes(FsFileSystem *sd) {
    FsFile f;
    if (R_FAILED(fsFsOpenFile(sd, "/config/switch-frame-tap/launcher-test.txt", FsOpenMode_Read, &f))) { return 0; }
    char b[16] = { 0 };
    u64 n = 0;
    fsFileRead(&f, 0, b, sizeof(b) - 1, FsReadOption_None, &n);
    fsFileClose(&f);
    return (u64)strtoul(b, NULL, 10) << 20;
}

/* v0.7.13: the sysmodules from the SD card (atmosphere/contents/<id>/exefs.nsp)
 * that are running, and the memory each uses - for "turn one off" to have a
 * name. Biggest first, appended to text as "others=name KB; ...". */
typedef struct { u64 id; u64 bytes; char name[40]; } Other;

static void ToolboxName(FsFileSystem *sd, u64 id, char *out, size_t cap) {
    snprintf(out, cap, "%016llx", (unsigned long long)id);
    char path[96];
    snprintf(path, sizeof(path), "/atmosphere/contents/%016llx/toolbox.json", (unsigned long long)id);
    FsFile f;
    if (R_FAILED(fsFsOpenFile(sd, path, FsOpenMode_Read, &f))) { return; }
    char b[512] = { 0 };
    u64 n = 0;
    fsFileRead(&f, 0, b, sizeof(b) - 1, FsReadOption_None, &n);
    fsFileClose(&f);
    const char *k = strstr(b, "\"name\"");
    const char *q = k != NULL ? strchr(k + 6, '"') : NULL;
    const char *e = q != NULL ? strchr(q + 1, '"') : NULL;
    if (q == NULL || e == NULL || e - q - 1 <= 0) { return; }
    size_t len = (size_t)(e - q - 1);
    if (len >= cap) { len = cap - 1; }
    memcpy(out, q + 1, len);
    out[len] = 0;
}

static int AppendOthers(FsFileSystem *sd, char *text, size_t cap) {
    static u64 pids[96];
    s32 np = 0;
    if (R_FAILED(svcGetProcessList(&np, pids, 96))) { return 0; }
    static Other o[24];
    int no = 0;
    for (s32 i = 0; i < np && no < 24; ++i) {
        Handle h = INVALID_HANDLE;
        struct { u64 program_id; u8 storage; u8 pad[7]; u64 keys; u64 flags; } out;
        memset(&out, 0, sizeof(out));
        if (R_FAILED(serviceDispatchInOut(pmdmntGetServiceSession(), 65000, pids[i], out,
                .out_handle_attrs = { SfOutHandleAttr_HipcCopy }, .out_handles = &h))) { continue; }
        char path[96];
        snprintf(path, sizeof(path), "/atmosphere/contents/%016llx/exefs.nsp", (unsigned long long)out.program_id);
        FsDirEntryType t;
        if (out.program_id != 0x0100000000000C21ull && R_SUCCEEDED(fsFsGetEntryType(sd, path, &t))) {
            u64 used = 0;
            if (R_SUCCEEDED(svcGetInfo(&used, InfoType_UsedMemorySize, h, 0))) {
                o[no].id = out.program_id;
                o[no].bytes = used;
                if (out.program_id == SFT_TID) { snprintf(o[no].name, sizeof(o[no].name), "Switch Frame Tap"); }
                else { ToolboxName(sd, out.program_id, o[no].name, sizeof(o[no].name)); }
                ++no;
            }
        }
        svcCloseHandle(h);
    }
    /* biggest first */
    for (int a = 0; a < no; ++a) {
        for (int b = a + 1; b < no; ++b) {
            if (o[b].bytes > o[a].bytes) { const Other x = o[a]; o[a] = o[b]; o[b] = x; }
        }
    }
    int len = snprintf(text, cap, "others=");
    for (int a = 0; a < no && len > 0 && (size_t)len < cap; ++a) {
        len += snprintf(text + len, cap - len, "%s%s %llu KB", a ? "; " : "", o[a].name, (unsigned long long)(o[a].bytes / 1024));
    }
    if (len > 0 && (size_t)len < cap) { len += snprintf(text + len, cap - len, "\n"); }
    return len;
}

void Launcher(void) {
    FsFileSystem sd;
    const bool have_sd = R_SUCCEEDED(fsInitialize()) && R_SUCCEEDED(fsOpenSdCardFileSystem(&sd));

    /* the HOME menu up for 5 s, and the System group within 256 KB of where
     * it was 3 s ago (the boot program's ~7.2 MB is in) - or 2 minutes */
    const u64 t0 = UptimeMs();
    u64 t_menu = 0, t_still = t0;
    s64 cur = 0, lim = 0, last = -1;
    for (;;) {
        const u64 now = UptimeMs();
        if (SystemMemory(&cur, &lim) && (last < 0 || llabs(cur - last) >= 256 * 1024)) { last = cur; t_still = now; }
        if (t_menu == 0 && Running(QLAUNCH_TID)) { t_menu = now; }
        if (t_menu != 0 && now - t_menu >= 5000 && now - t_still >= 3000) { break; }
        if (now - t0 >= 120000) { break; }
        svcSleepThread(250000000ull);
    }

    u64 need = have_sd ? ModuleFootprint(&sd) : 0;
    const bool measured = need != 0;
    if (!measured) { need = FALLBACK_FOOTPRINT; }
    const u64 less = have_sd ? TestLessBytes(&sd) : 0;
    const s64 free_b = (lim - cur) - (s64)less;
    const u64 now = UptimeMs();

    char text[1536];
    int len = snprintf(text, sizeof(text),
                       "Switch Frame Tap launcher, %llu.%llu s after boot: HOME menu %s; system memory %lld KB free of %lld KB%s; "
                       "the sysmodule needs %llu KB (%s) and %llu KB to spare.\n",
                       (unsigned long long)(now / 1000), (unsigned long long)((now % 1000) / 100), t_menu != 0 ? "up" : "not seen in 2 minutes",
                       (long long)(free_b / 1024), (long long)(lim / 1024), less != 0 ? " (test: less)" : "",
                       (unsigned long long)(need / 1024), measured ? "from its exefs.nsp" : "estimated", (unsigned long long)(SPARE_BYTES / 1024));

    if (Running(SFT_TID)) {
        snprintf(text + len, sizeof(text) - len, "result=running\nIt was already running (started another way).\n");
    } else if (free_b < 0 || (u64)free_b < need + SPARE_BYTES) {
        snprintf(text + len, sizeof(text) - len,
                 "result=low-memory\nNot started this boot: other sysmodules use the memory it needs. "
                 "Turning one of them off frees some (the console boots normally either way).\n");
    } else {
        /* the module checks this to know it was started here, not at boot */
        char go[32];
        snprintf(go, sizeof(go), "%llu\n", (unsigned long long)UptimeMs());
        if (have_sd) { WriteText(&sd, "/config/switch-frame-tap/launch.txt", go); }
        Result rc = pmshellInitialize();
        u64 pid = 0;
        if (R_SUCCEEDED(rc)) {
            const NcmProgramLocation loc = { .program_id = SFT_TID, .storageID = NcmStorageId_None };
            rc = pmshellLaunchProgram(0, &loc, &pid);
            pmshellExit();
        }
        snprintf(text + len, sizeof(text) - len, "result=%s\nStarted (rc=0x%x).\n", R_SUCCEEDED(rc) ? "started" : "failed", rc);
    }
    if (have_sd) {
        const size_t used = strlen(text);
        AppendOthers(&sd, text + used, sizeof(text) - used);
        WriteText(&sd, "/config/switch-frame-tap/launcher.txt", text);
        fsFsClose(&sd);
    }
    fsExit();
}
