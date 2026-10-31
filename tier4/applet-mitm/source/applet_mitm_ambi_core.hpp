/* Switch Ambilight prototype: GPL-2.0-only, no platform deps. */
#pragma once
#include <cstddef>
#include <cstdint>

namespace sftambi {
    struct Rgb { uint8_t r, g, b; };
    constexpr int Top = 36, Right = 21, Bottom = 36, Left = 21, Count = 114;
    struct State {
        Rgb previous[Count] = {};
        bool started = false;
        uint8_t brightness = 190;
        uint8_t alpha = 160;    /* fraction of new colour (255: no smoothing) */
    };
    inline uint8_t Clip(int x) { return static_cast<uint8_t>(x < 0 ? 0 : x > 255 ? 255 : x); }
    inline int Clamp(int x, int low, int high) { return x < low ? low : x > high ? high : x; }
    /* 320x180, pitch-linear NV12, ITU BT.709 limited range. */
    inline Rgb At(const uint8_t *y, const uint8_t *uv, int w, int h, int stride, int x, int yy) {
        x = Clamp(x, 0, w - 1); yy = Clamp(yy, 0, h - 1);
        const int Y = (y[yy * stride + x] > 16 ? y[yy * stride + x] - 16 : 0);
        const int u = int(uv[(yy/2) * stride + (x/2)*2]) - 128;
        const int v = int(uv[(yy/2) * stride + (x/2)*2 + 1]) - 128;
        return {Clip((298*Y + 459*v + 128) >> 8),
                Clip((298*Y - 55*u - 136*v + 128) >> 8),
                Clip((298*Y + 541*u + 128) >> 8)};
    }
    /* LED 0 top-left, advances right, then down, left and up clockwise. */
    inline bool Sample(State &state, const uint8_t *y, const uint8_t *uv,
                       int w, int h, int stride, Rgb *out, size_t capacity) {
        if (!y || !uv || !out || capacity < Count || w < 16 || h < 16 || stride < w || (w & 1) || (h & 1)) { return false; }
        const int counts[4] = {Top, Right, Bottom, Left};
        int idx = 0;
        for (int side = 0; side < 4; ++side) {
            for (int i = 0; i < counts[side]; ++i, ++idx) {
                const int t = (2*i + 1);
                const int denom = 2*counts[side];
                const int xx = side == 0 ? t*(w-1)/denom : side == 1 ? 94*(w-1)/100
                              : side == 2 ? (denom-t)*(w-1)/denom : 6*(w-1)/100;
                const int yy = side == 0 ? 6*(h-1)/100 : side == 1 ? t*(h-1)/denom
                              : side == 2 ? 94*(h-1)/100 : (denom-t)*(h-1)/denom;
                int r=0,g=0,b=0;
                for (int sy=-2;sy<=2;++sy) for (int sx=-2;sx<=2;++sx) {
                    const Rgb pix = At(y,uv,w,h,stride,xx+sx*2,yy+sy*2);
                    r += pix.r; g += pix.g; b += pix.b;
                }
                const Rgb now = {uint8_t((r/25)*state.brightness/255),
                                 uint8_t((g/25)*state.brightness/255),
                                 uint8_t((b/25)*state.brightness/255)};
                const Rgb prev = state.previous[idx];
                const int a = state.started ? state.alpha : 255;
                out[idx] = {uint8_t((a*now.r+(255-a)*prev.r+127)/255),
                            uint8_t((a*now.g+(255-a)*prev.g+127)/255),
                            uint8_t((a*now.b+(255-a)*prev.b+127)/255)};
            }
        }
        for (int i=0;i<Count;++i) state.previous[i] = out[i];
        state.started = true;
        return true;
    }
    /* One DDP v1 RGB24 packet (10-byte header + 114 RGB LEDs). */
    inline size_t Packet(uint8_t *packet, size_t cap, const Rgb *colors, size_t n) {
        if (!packet || !colors || n != Count || cap < 10u + n*3u) { return 0; }
        packet[0] = 0x41;  /* v1 + PUSH */
        packet[1] = 0;     /* unnumbered */
        packet[2] = 1;     /* RGB24 */
        packet[3] = 1;     /* display ID */
        packet[4] = packet[5] = packet[6] = packet[7] = 0;
        packet[8] = uint8_t((n*3) >> 8); packet[9] = uint8_t(n*3);
        for (size_t i=0;i<n;++i) {
            packet[10 + 3*i] = colors[i].r;
            packet[11 + 3*i] = colors[i].g;
            packet[12 + 3*i] = colors[i].b;
        }
        return 10u + n*3u;
    }
    static_assert(sizeof(Rgb) == 3, "DDP requires contiguous RGB triplets");
}
