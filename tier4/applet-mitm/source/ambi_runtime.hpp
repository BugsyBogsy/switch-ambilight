// Shared atomic Ambilight parameters; kept separate from the proven VIC code.
#pragma once
#include <stratosphere.hpp>
#include <atomic>
#include "ambi_settings.hpp"
namespace ams::mitm::applet {
extern std::atomic<u32> g_ambi_ip_host;
extern std::atomic<u32> g_ambi_brightness;
extern std::atomic<u32> g_ambi_smoothing;
extern std::atomic<u32> g_ambi_fps;
inline void AmbiApply(const ambiset::Settings& s) {
    g_ambi_ip_host.store(s.host, std::memory_order_relaxed);
    g_ambi_brightness.store(s.brightness, std::memory_order_relaxed);
    g_ambi_smoothing.store(s.smoothing, std::memory_order_relaxed);
    g_ambi_fps.store(s.fps, std::memory_order_relaxed);
}
}
