#include "../tier4/applet-mitm/source/ambi_settings.hpp"
#include <cassert>
#include <cstring>
#include <cstdio>
int main() {
    uint32_t ip=0;
    assert(ambiset::ParseIp("192.168.1.50",&ip) && ip==0xC0A80132u);
    assert(!ambiset::ParseIp("192.168.1.999",&ip));
    assert(!ambiset::ParseIp("192.168.1.50something",&ip));
    assert(!ambiset::ParseIp("224.0.0.1",&ip));
    assert(!ambiset::ParseIp("192.168.1",&ip));
    const char *ini="# comment\nambi_ip = 192.168.1.70\nambi_brightness = 128\nambi_smoothing = 64\nambi_fps = 20\nambilight = 1\n";
    auto s=ambiset::Parse(ini,std::strlen(ini));
    assert(s.host==0xC0A80146u && s.brightness==128 && s.smoothing==64 && s.fps==20);
    const char *bad="ambi_ip = 300.1.2.3\nambi_fps = 0\nambi_brightness = -1\n";
    s=ambiset::Parse(bad,std::strlen(bad));
    assert(s.host==0xC0A80132u && s.fps==25 && s.brightness==190);
    puts("OK: v0.5 settings parser, IPv4 validation, defaults");
}
