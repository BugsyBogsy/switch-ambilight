#include "applet_mitm_featmem.hpp"
#include <cstring>
#include "applet_mitm_log.hpp"

namespace ams::mitm::applet {

    namespace {

        constinit os::SharedMemoryType g_blocks[4] = {};
        constinit u32 g_nblocks = 0;
        constinit os::SdkMutex g_lock;

    }

    void *FeatureMemory(size_t size, const char *what) {
        std::scoped_lock lk(g_lock);
        size = util::AlignUp(size, os::MemoryPageSize);
        if (g_nblocks >= std::size(g_blocks)) {
            LogLine("%s: no memory block left for it", what);
            return nullptr;
        }
        os::SharedMemoryType &b = g_blocks[g_nblocks];
        if (const Result rc = os::CreateSharedMemory(std::addressof(b), size, os::MemoryPermission_ReadWrite, os::MemoryPermission_ReadOnly); R_FAILED(rc)) {
            LogLine("%s: the system has no %zu KB for it (rc=0x%x)", what, size / 1024, rc.GetValue());
            return nullptr;
        }
        void *p = os::MapSharedMemory(std::addressof(b), os::MemoryPermission_ReadWrite);
        if (p == nullptr) {
            os::DestroySharedMemory(std::addressof(b));
            LogLine("%s: its %zu KB could not be mapped", what, size / 1024);
            return nullptr;
        }
        std::memset(p, 0, size);
        ++g_nblocks;
        LogLine("%s: %zu KB of memory taken (it is on)", what, size / 1024);
        return p;
    }

}
