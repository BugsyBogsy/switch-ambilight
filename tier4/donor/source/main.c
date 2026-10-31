/*
 * Switch Frame Tap memory donor (v0.7.10).
 *
 * The browser applets (manuals, news, eShop) need nearly the whole Applet
 * memory pool: any sysmodule resident there while idle makes them abort with
 * 2210-1024. Streaming needs ~42 MB that only the Applet pool has room for.
 * So the module lives elsewhere and launches this program only while a
 * viewer is connected: it takes the memory from the Applet pool and lends it
 * to the module as transfer memory, then waits. When the module gives it back
 * (unmaps it), this frees it and exits.
 *
 * v0.7.13: it is also the launcher (launcher.c) - at boot it starts the module
 * once the system has booted, so the module never holds System memory the
 * boot itself needs.
 */
#include <switch.h>

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;   /* the launcher's SD card */

/* libnx's own heap stays tiny and static, so svcSetHeapSize is ours alone */
#define INNER_HEAP_SIZE 0x4000
static char g_inner_heap[INNER_HEAP_SIZE];

void __libnx_initheap(void) {
    extern char *fake_heap_start;
    extern char *fake_heap_end;
    fake_heap_start = g_inner_heap;
    fake_heap_end   = g_inner_heap + sizeof(g_inner_heap);
}

void __appInit(void) {
    if (R_FAILED(smInitialize())) { diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM)); }
}

void __appExit(void) { smExit(); }

void Launcher(void);   /* launcher.c (v0.7.13) */
bool Running(u64 tid);

#define SFT_TID 0x0100000000000C20ull

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    /* v0.7.13: started at boot (this program's boot2.flag), or by hand with
     * the module not running: start the module once the system has booted.
     * Started by the module while a viewer connects: lend it the memory. */
    if (R_SUCCEEDED(pmdmntInitialize())) {
        const bool at_boot = armTicksToNs(armGetSystemTick()) < 15000000000ull;
        bool sft = Running(SFT_TID);
        /* at boot, a module from an older install's own boot2.flag gives the
         * start to this program and exits within a moment - let it; if it
         * stays (an older module without that), leave it alone */
        for (int i = 0; at_boot && sft && i < 40; ++i) {
            svcSleepThread(250000000ull);
            sft = Running(SFT_TID);
        }
        if (!sft) { Launcher(); }
        pmdmntExit();
        if (at_boot || !sft) { return 0; }
    }
    Service ctl;
    if (R_FAILED(smGetService(&ctl, "sftap"))) { return 1; }
    smExit();   /* sm only for this */

    /* v0.7.14: the module may ask for less (sftap 13: the HOME menu streaming
     * beside a big applet such as System Settings); a module before it: 42 */
    u32 cap_mb = 42;
    if (R_FAILED(serviceDispatchOut(&ctl, 13, cap_mb)) || cap_mb < 22 || cap_mb > 42) { cap_mb = 42; }

    /* the largest the Applet pool grants (2 MB steps, the heap unit). Not
     * below 22: the module streams from no less (MinCaptureBytes in
     * applet_mitm_nv.cpp), and a smaller loan only held memory the applets
     * could have had. */
    static const u32 sizes_mb[] = { 42, 38, 34, 30, 26, 22 };
    void *addr = NULL;
    u64 size = 0;
    for (u32 i = 0; i < sizeof(sizes_mb) / sizeof(sizes_mb[0]); ++i) {
        if (sizes_mb[i] > cap_mb) { continue; }
        if (R_SUCCEEDED(svcSetHeapSize(&addr, (u64)sizes_mb[i] << 20))) { size = (u64)sizes_mb[i] << 20; break; }
    }

    Handle tmem = INVALID_HANDLE, nv_tmem = INVALID_HANDLE;
    /* v0.7.13: the top 256 KB is nvdrv's transfer memory for the module's
     * engines (Donate2, cmd 12) - it was 256 KB of the module's own memory,
     * in the System group sysmodules share with am */
    const u64 nv_size = 0x40000;
    Result rc = size > nv_size ? svcCreateTransferMemory(&tmem, addr, size - nv_size, Perm_None) : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    if (R_SUCCEEDED(rc)) { rc = svcCreateTransferMemory(&nv_tmem, (u8 *)addr + size - nv_size, nv_size, Perm_None); }
    if (R_SUCCEEDED(rc)) {
        const struct { u64 size, nv_size; } in = { size - nv_size, nv_size };
        const Result drc = serviceDispatchIn(&ctl, 12, in, .in_num_handles = 2, .in_handles = { tmem, nv_tmem });
        svcCloseHandle(tmem);   /* the module holds its own copies; the memory stays lent */
        svcCloseHandle(nv_tmem);
        if (R_FAILED(drc)) {
            /* a module before v0.7.13: the whole heap, as one (cmd 10) */
            if (R_SUCCEEDED(svcCreateTransferMemory(&tmem, addr, size, Perm_None))) {
                serviceDispatchIn(&ctl, 10, size, .in_num_handles = 1, .in_handles = { tmem });
                svcCloseHandle(tmem);
            }
        }
    } else {
        if (tmem != INVALID_HANDLE) { svcCloseHandle(tmem); }
        const u32 urc = rc;
        serviceDispatchIn(&ctl, 11, urc);
    }
    serviceClose(&ctl);

    /* Hold it while it is lent. Shrinking the heap is refused while the pages
     * are transferred; once the module unmaps and closes its handle, the next
     * try succeeds and the memory is back in the Applet pool within ~5 ms -
     * pm:shell's TerminateProcess took ~200 ms, too slow for a browser applet
     * that starts while we stream. The module still terminates us as a
     * backstop. */
    if (size != 0) {
        while (R_FAILED(svcSetHeapSize(&addr, 0))) { svcSleepThread(5000000ull); }
    }
    return 0;
}
