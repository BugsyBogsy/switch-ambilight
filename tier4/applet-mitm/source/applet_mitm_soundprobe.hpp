/*
 * v0.7.14 sound probe (applet_mitm_soundprobe.cpp): research only, parked
 * after probe 3 (tier4/mitm/STATUS.md "Sound outside games"). Test builds
 * set SFT_SOUND_PROBE=1; then sdmc:/config/switch-frame-tap/probe-sound.txt
 * turns it on.
 */
#pragma once
#include <stratosphere.hpp>

#ifndef SFT_SOUND_PROBE
#define SFT_SOUND_PROBE 0
#endif

namespace ams::mitm::applet {

    void SoundProbeStart();

}
