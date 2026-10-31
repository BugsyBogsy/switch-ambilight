#include "ambi_runtime.hpp"
namespace ams::mitm::applet {
std::atomic<u32> g_ambi_ip_host{0xC0A80132u};
std::atomic<u32> g_ambi_brightness{190};
std::atomic<u32> g_ambi_smoothing{160};
std::atomic<u32> g_ambi_fps{25};
}
