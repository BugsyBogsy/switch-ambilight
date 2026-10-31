// Switch Ambilight overlay v0.6. GPL-2.0-only.
#define TESLA_INIT_IMPL
#include <tesla.hpp>
#include <cstdio>
#include <string>
#include "../../common/config_ini.hpp"

namespace {
struct Status {
    u32 version, state, enabled, width, height, fps_x10, game_fps_x10, sessions;
    u32 shot_enabled, shots, shot_fails, reserved;
    u64 app_tid;
    u32 app_excluded, reserved3;
};
static_assert(sizeof(Status)==64);
Service g_service{};
bool g_open=false;
bool Connect() {
    if(g_open) return true;
    tsl::hlp::doWithSmSession([] {
        Handle h{};
        const SmServiceName name=smEncodeName("sftap");
        // Check presence before smGetService (otherwise the overlay may hang).
        if(R_SUCCEEDED(smRegisterService(&h,name,false,1))) {
            svcCloseHandle(h);
            smUnregisterService(name);
        } else {
            g_open=R_SUCCEEDED(smGetService(&g_service,"sftap"));
        }
    });
    return g_open;
}
bool Get(Status* out) {
    if(!Connect()) return false;
    if(R_SUCCEEDED(serviceDispatchOut(&g_service,0,*out))) return true;
    serviceClose(&g_service);g_open=false;return false;
}
void Reload() {if(Connect()) serviceDispatch(&g_service,2);}
void Enable(bool on) {if(Connect()) { const u8 v=on?1:0;serviceDispatchIn(&g_service,1,v);}}
int Number(const char* key,int def) {
    std::string v;
    tsl::hlp::doWithSDCardHandle([&]{ambiini::Get(key,&v);});
    if(v.empty())return def;
    int val=0;
    for(char c:v) {if(c<'0'||c>'9')return def; val=val*10+(c-'0');if(val>255)return def;}
    return val;
}
bool SaveNumber(const char* key,int value) {
    char s[12];std::snprintf(s,sizeof(s),"%d",value);
    bool ok=false;
    tsl::hlp::doWithSDCardHandle([&]{ok=ambiini::Set(key,s);});
    if(ok)Reload();
    return ok;
}
const char* Activity(u32 state) {
    switch(state) {
      case 0:return "Starting";
      case 1:return "Disabled";
      case 2:return "Waiting for game";
      case 3:return "Waiting for game";
      case 4:return "Capturing game";
      default:return "Idle / check log";
    }
}
class MainGui : public tsl::Gui {
public:
    tsl::elm::Element* createUI() override {
        auto *f=new tsl::elm::OverlayFrame("Switch Ambilight","v0.6");
        auto *list=new tsl::elm::List();
        Status st{};bool present=Get(&st);
        list->addItem(new tsl::elm::CategoryHeader("114 LEDs / WLED"));
        auto *enabled=new tsl::elm::ToggleListItem("Ambilight",present && st.enabled!=0);
        enabled->setStateChangedListener([present,enabled](bool on){
           if(present)Enable(on);else enabled->setState(false);
        });
        list->addItem(enabled);
        auto *activity=new tsl::elm::ListItem("Capture");
        activity->setValue(present?Activity(st.state):"Sysmodule absent",!present);
        list->addItem(activity);
        auto *bright=new tsl::elm::ListItem("Brightness (A to change)");
        const int brvals[]={64,96,128,160,190,220,255};
        int bv=Number("ambi_brightness",190);
        {char b[12];std::snprintf(b,sizeof(b),"%d/255",bv);bright->setValue(b,false);}
        bright->setClickListener([bright,bv](u64 keys) mutable {
           if(!(keys&HidNpadButton_A))return false;
           const int vals[]={64,96,128,160,190,220,255};
           int i=0;while(i<7 && vals[i]<=bv) ++i;
           bv=vals[i==7?0:i];
           if(SaveNumber("ambi_brightness",bv)) {char b[12];std::snprintf(b,sizeof(b),"%d/255",bv);bright->setValue(b,false);}
           return true;
        });
        list->addItem(bright);
        auto *smooth=new tsl::elm::ListItem("Smoothing (A to change)");
        int sv=Number("ambi_smoothing",160);
        {char b[12];std::snprintf(b,sizeof(b),"%d/255",sv);smooth->setValue(b,false);}
        smooth->setClickListener([smooth,sv](u64 keys) mutable {
           if(!(keys&HidNpadButton_A))return false;
           const int vals[]={0,64,128,160,192,224,255};
           int i=0;while(i<7 && vals[i]<=sv) ++i;
           sv=vals[i==7?0:i];
           if(SaveNumber("ambi_smoothing",sv)) {char b[12];std::snprintf(b,sizeof(b),"%d/255",sv);smooth->setValue(b,false);}
           return true;
        });
        list->addItem(smooth);
        auto *fps=new tsl::elm::ListItem("Max FPS (A to change)");
        int fv=Number("ambi_fps",25);
        {char b[12];std::snprintf(b,sizeof(b),"%d",fv);fps->setValue(b,false);}
        fps->setClickListener([fps,fv](u64 keys) mutable {
           if(!(keys&HidNpadButton_A))return false;
           const int vals[]={10,15,20,25,30};
           int i=0;while(i<5 && vals[i]<=fv) ++i;
           fv=vals[i==5?0:i];
           if(SaveNumber("ambi_fps",fv)){char b[12];std::snprintf(b,sizeof(b),"%d",fv);fps->setValue(b,false);}
           return true;
        });
        list->addItem(fps);
        list->addItem(new tsl::elm::CategoryHeader("Network"));
        auto *ip=new tsl::elm::ListItem("WLED IP (edit in NRO)");
        std::string ipstr="192.168.1.50";
        tsl::hlp::doWithSDCardHandle([&] {ambiini::Get("ambi_ip",&ipstr);});
        ip->setValue(ipstr.c_str(),false);
        list->addItem(ip);
        f->setContent(list);
        return f;
    }
};
class AmbilightOverlay:public tsl::Overlay {
public:
    void initServices() override {}
    void exitServices() override {if(g_open){serviceClose(&g_service);g_open=false;}}
    std::unique_ptr<tsl::Gui> loadInitialGui() override {return initially<MainGui>();}
};
}
int main(int argc,char** argv) {return tsl::loop<AmbilightOverlay>(argc,argv);}
