#include "../source/applet_mitm_ambi_core.hpp"
#include "../source/applet_mitm_armfile.hpp"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
    using namespace sftambi;
    static_assert(Top + Right + Bottom + Left == Count);
    constexpr int w=320,h=180;
    std::vector<uint8_t> y(w*h, 128), uv(w*h/2, 128);
    Rgb leds[Count] = {};
    State st;
    assert(Sample(st,y.data(),uv.data(),w,h,w,leds,Count));
    for (auto c: leds) { assert(c.r >= 93 && c.r <= 107); assert(c.g >= 93 && c.g <= 107); assert(c.b >= 93 && c.b <= 107); }
    // Distinguishable edge Y signal: top bright, right medium, bottom dark, left black.
    // Avoid corners to prevent cross-edge sample overlap.
    for(int yy=0;yy<h;++yy) for(int x=0;x<w;++x) {
        if (yy < 18) y[yy*w+x]=235;
        else if (yy >= h-18) y[yy*w+x]=55;
        else if(x>=w-25) y[yy*w+x]=180;
        else if(x<25) y[yy*w+x]=20;
        else y[yy*w+x]=128;
    }
    st.alpha=255;
    assert(Sample(st,y.data(),uv.data(),w,h,w,leds,Count));
    auto mean=[&](int a,int b){int s=0;for(int i=a;i<b;++i)s+=leds[i].r;return s/(b-a);};
    assert(mean(0,36)>mean(36,57));
    assert(mean(36,57)>mean(57,93));
    assert(mean(57,93)>mean(93,114));
    assert(!Sample(st,y.data(),uv.data(),w,h,w,leds,113));
    uint8_t ddp[10 + Count*3] = {};
    assert(Packet(ddp, sizeof(ddp), leds, Count) == sizeof(ddp));
    assert(ddp[0] == 0x41 && ddp[1] == 0 && ddp[2] == 1 && ddp[3] == 1);
    assert(ddp[8] == 1 && ddp[9] == 86);  /* 114*3=342 bytes */
    assert(ddp[10] == leds[0].r && ddp[10+3*113+2] == leds[113].b);
    assert(Packet(ddp, sizeof(ddp)-1, leds, Count) == 0);
    namespace af=ams::mitm::applet::armfile;
    char buf[512] = {};
    const char *settings="ambilight = 1\nconnection = usb\naudio = 1\n";
    auto n=af::BuildReleaseArm(settings,std::strlen(settings),buf,sizeof(buf));
    assert(n>0 && af::Contains(buf,n,"ambi") && af::Contains(buf,n,"net") && !af::Contains(buf,n,"usb"));
    const char *off="ambilight = 0\n";
    n=af::BuildReleaseArm(off,std::strlen(off),buf,sizeof(buf));
    assert(n>0 && !af::Contains(buf,n,"ambi"));
    puts("OK: 114-led mapping, NV12 sampling, alpha and config gate");
}
