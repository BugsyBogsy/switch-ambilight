// Switch Ambilight v0.5: independent, host-testable settings parser.
// Copyright GPL-2.0-only; derived work retains upstream licence.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
namespace ambiset {
struct Settings {
    uint32_t host = 0xC0A80132u; // 192.168.1.50 in host-order dotted-quad encoding
    uint8_t brightness = 190;
    uint8_t smoothing = 160;
    uint8_t fps = 25;
};
inline bool ParseIp(const char* s, uint32_t* result) {
    if (!s || !result) return false;
    uint32_t bytes[4]{};
    for (unsigned i=0;i<4;i++) {
        if (*s<'0' || *s>'9') return false;
        unsigned v=0, digits=0;
        while (*s>='0' && *s<='9') {
            if (++digits>3) return false;
            v=v*10+unsigned(*s++-'0');
            if (v>255) return false;
        }
        bytes[i]=v;
        if (i<3 && *s++!='.') return false;
    }
    if (*s) return false;
    // Reject unspecified/broadcast/multicast and loopback addresses.
    if (bytes[0]==0 || bytes[0]==127 || bytes[0]>=224 || (bytes[0]==169 && bytes[1]==254)) return false;
    *result=(bytes[0]<<24)|(bytes[1]<<16)|(bytes[2]<<8)|bytes[3];
    return true;
}
inline bool IniValue(const char* data,size_t n,const char* key,char* dst,size_t cap) {
    if (!data || !key || !dst || cap==0) return false;
    size_t keylen=std::strlen(key);
    for(size_t i=0;i<n;) {
        size_t e=i; while(e<n && data[e]!='\n' && data[e]!=0) ++e;
        size_t p=i; while(p<e && (data[p]==' ' || data[p]=='\t')) ++p;
        if(p<e && data[p]!='#' && data[p]!=';' && data[p]!='[') {
            size_t k=p; while(k<e && data[k]!='=' && data[k]!=' ' && data[k]!='\t') ++k;
            size_t q=k; while(q<e && (data[q]==' ' || data[q]=='\t')) ++q;
            if(k-p==keylen && std::memcmp(data+p,key,keylen)==0 && q<e && data[q]=='=') {
                ++q; while(q<e && (data[q]==' ' || data[q]=='\t')) ++q;
                size_t end=e; while(end>q && (data[end-1]==' ' || data[end-1]=='\t' || data[end-1]=='\r')) --end;
                if(end-q>=cap) return false;
                std::memcpy(dst,data+q,end-q); dst[end-q]=0; return true;
            }
        }
        i=e+1;
    }
    return false;
}
inline bool ParseNum(const char* s,unsigned* out,unsigned lo,unsigned hi) {
    if(!s || !*s || !out) return false;
    unsigned v=0;
    for(;*s;++s) { if(*s<'0' || *s>'9') return false; v=v*10+unsigned(*s-'0'); if(v>hi) return false; }
    if(v<lo) return false;
    *out=v;
    return true;
}
inline Settings Parse(const char* data,size_t n) {
    Settings s{}; char buf[64]; unsigned v;
    if(IniValue(data,n,"ambi_ip",buf,sizeof(buf))) {uint32_t ip; if(ParseIp(buf,&ip)) s.host=ip;}
    if(IniValue(data,n,"ambi_brightness",buf,sizeof(buf)) && ParseNum(buf,&v,0,255)) s.brightness=uint8_t(v);
    if(IniValue(data,n,"ambi_smoothing",buf,sizeof(buf)) && ParseNum(buf,&v,0,255)) s.smoothing=uint8_t(v);
    if(IniValue(data,n,"ambi_fps",buf,sizeof(buf)) && ParseNum(buf,&v,5,30)) s.fps=uint8_t(v);
    return s;
}
}
