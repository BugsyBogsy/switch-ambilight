/*
 * v0.7.14: memory for the opt-in features (the HOME menu), taken only when
 * one is on. A module's static memory is spent at every boot whether a
 * feature runs or not (the HOME menu's dispdrv server and scan buffers were
 * ~70 KB of it), and the heap moves in 2 MB steps (svcSetHeapSize). A
 * shared memory object of our own is page-sized and comes from the same
 * system pool.
 * It holds data and IPC receive buffers (the kernel only asks for
 * reference-counted memory there), never thread stacks: libnx mirrors a
 * stack with svcMapMemory, which shared memory does not allow.
 * Never given back - the features run until the console turns off.
 */
#pragma once
#include <stratosphere.hpp>

namespace ams::mitm::applet {

    /* size bytes, page-aligned and zeroed; nullptr (logged) if the system
     * has no room */
    void *FeatureMemory(size_t size, const char *what);

}
