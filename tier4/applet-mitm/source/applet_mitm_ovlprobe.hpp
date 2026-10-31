/*
 * v0.7.14 overlay probe (applet_mitm_ovlprobe.cpp): research only. Test builds
 * set SFT_OVL_PROBE=1; then sdmc:/config/switch-frame-tap/probe-ovl.txt turns
 * it on (and probe-blend.txt the VIC blend card, applet_mitm_nv.cpp).
 */
#pragma once
#include <stratosphere.hpp>
#include <atomic>

#ifndef SFT_OVL_PROBE
#define SFT_OVL_PROBE 0
#endif

namespace ams::mitm::applet {

    void OvlProbeStart();

}
