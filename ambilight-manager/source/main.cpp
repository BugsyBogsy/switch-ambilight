// Switch Ambilight manager v0.6: compact controller-only NRO.
// Uses libnx's built-in console; no SDL, HTTP, USB, encoder or viewer.
#include <switch.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include "../../common/config_ini.hpp"

namespace {
constexpr const char* iniDir="sdmc:/config/switch-ambilight";
struct Status {
    u32 version,state,enabled,width,height,fps_x10,game_fps_x10,sessions;
    u32 shot_enabled,shots,shot_fails,reserved;
    u64 app_tid;
    u32 app_excluded,reserved3;
};
static_assert(sizeof(Status)==64);
Service svc{};
bool open=false;
bool Connect() {
    if(open)return true;
    u64 pid=0;
    if(R_FAILED(pmdmntGetProcessId(&pid,0x0100000000000C20ull)))return false;
    open=R_SUCCEEDED(smGetService(&svc,"sftap"));
    return open;
}
bool Get(Status *s) {
    if(!Connect())return false;
    if(R_SUCCEEDED(serviceDispatchOut(&svc,0,*s)))return true;
    serviceClose(&svc);open=false;return false;
}
void Reload() {if(Connect()) serviceDispatch(&svc,2);}
void Enable(bool on) {if(Connect()){u8 b=on?1:0;serviceDispatchIn(&svc,1,b);}}
unsigned U(const char* key,unsigned d,unsigned max) {
    std::string s;
    if(!ambiini::Get(key,&s) || s.empty())return d;
    unsigned v=0;
    for(char c:s) {if(c<'0'||c>'9')return d;v=v*10+unsigned(c-'0');if(v>max)return d;}
    return v;
}
bool SetNum(const char* key,unsigned v) {
    char b[16];std::snprintf(b,sizeof(b),"%u",v);
    if(!ambiini::Set(key,b))return false;
    Reload();return true;
}
unsigned Wrap(unsigned v,int delta,unsigned lo,unsigned hi) {
    int t=int(v)+delta;
    return unsigned(t<int(lo)?int(hi):t>int(hi)?int(lo):t);
}
}
int main(int,char**) {
    consoleInit(nullptr);
    pmdmntInitialize();
    fsdevMountSdmc();
    mkdir("sdmc:/config",0777);
    mkdir(iniDir,0777);
    padConfigureInput(1,HidNpadStyleSet_NpadStandard);
    PadState pad;padInitializeDefault(&pad);
    unsigned cur=0;
    std::string msg="Select with UP/DOWN, change with LEFT/RIGHT";
    while(appletMainLoop()) {
        padUpdate(&pad);
        const u64 key=padGetButtonsDown(&pad);
        if(key&(HidNpadButton_B|HidNpadButton_Plus))break;
        if(key&HidNpadButton_Up) cur=(cur+8)%9;
        if(key&HidNpadButton_Down) cur=(cur+1)%9;
        Status st{};bool have=Get(&st);
        std::string ip="192.168.1.50";
        ambiini::Get("ambi_ip",&ip);
        unsigned oct[4]={192,168,1,50};
        if(std::sscanf(ip.c_str(),"%u.%u.%u.%u",&oct[0],&oct[1],&oct[2],&oct[3])!=4 ||
           oct[0]>255 || oct[1]>255 || oct[2]>255 || oct[3]>255) {oct[0]=192;oct[1]=168;oct[2]=1;oct[3]=149;}
        unsigned brightness=U("ambi_brightness",190,255);
        unsigned smoothing=U("ambi_smoothing",160,255);
        unsigned fps=U("ambi_fps",25,30);
        int delta=(key&HidNpadButton_Right)?1:(key&HidNpadButton_Left)?-1:0;
        bool changed=false;
        if(cur==0 && (delta || (key&HidNpadButton_A))) {
            if(have) {Enable(!st.enabled);msg="Ambilight toggled";}
            else msg="Sysmodule absent; cannot toggle";
        }
        if(delta && cur>=1 && cur<=4) {
            oct[cur-1]=Wrap(oct[cur-1],delta,cur==1?1:0,cur==1?223:255);
            char b[20];std::snprintf(b,sizeof(b),"%u.%u.%u.%u",oct[0],oct[1],oct[2],oct[3]);
            changed=ambiini::Set("ambi_ip",b);
        }
        if(delta && cur==5)changed=SetNum("ambi_brightness",Wrap(brightness,delta*5,0,255));
        if(delta && cur==6)changed=SetNum("ambi_smoothing",Wrap(smoothing,delta*8,0,255));
        if(delta && cur==7)changed=SetNum("ambi_fps",Wrap(fps,delta,5,30));
        if(cur==8 && (key&HidNpadButton_A)) {
            changed=ambiini::Set("ambilight","1") && ambiini::Set("audio","0") && ambiini::Set("connection","network") && ambiini::Set("home_menu","0") && ambiini::Set("overlays","0");
        }
        if(changed) {Reload();msg="Saved. Sysmodule reloaded.";}
        else if(delta && cur!=0)msg="Save failed or no change";
        consoleClear();
        std::printf("SWITCH AMBILIGHT  v0.6\n");
        std::printf("Mariko | 114 LEDs: 36 / 21 / 36 / 21\n");
        std::printf("---------------------------------------\n");
        std::printf("%c Ambilight      : %s\n",cur==0?'>':' ',have?(st.enabled?"ON":"OFF"):"MODULE OFFLINE");
        std::printf("%c WLED IP byte 1 : %u\n",cur==1?'>':' ',oct[0]);
        std::printf("%c WLED IP byte 2 : %u\n",cur==2?'>':' ',oct[1]);
        std::printf("%c WLED IP byte 3 : %u\n",cur==3?'>':' ',oct[2]);
        std::printf("%c WLED IP byte 4 : %u\n",cur==4?'>':' ',oct[3]);
        std::printf("%c Brightness     : %u / 255\n",cur==5?'>':' ',brightness);
        std::printf("%c Smoothing      : %u / 255\n",cur==6?'>':' ',smoothing);
        std::printf("%c Target FPS     : %u\n",cur==7?'>':' ',fps);
        std::printf("%c Force Ambilight-only configuration (A)\n",cur==8?'>':' ');
        std::printf("---------------------------------------\n");
        std::printf("WLED destination: %u.%u.%u.%u:4048\n",oct[0],oct[1],oct[2],oct[3]);
        std::printf("Status: %s\n",msg.c_str());
        std::printf("B/PLUS exit; changes save immediately.\n");
        consoleUpdate(nullptr);
        svcSleepThread(50000000);
    }
    if(open)serviceClose(&svc);
    pmdmntExit();
    consoleExit(nullptr);
    return 0;
}
