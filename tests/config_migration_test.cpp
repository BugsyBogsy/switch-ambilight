// Test migration of v0.5 settings to v0.6 without changing the legacy file.
#include "../common/config_ini.hpp"
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
int main() {
    namespace fs = std::filesystem;
    fs::create_directories("sdmc:/config/switch-frame-tap");
    FILE* f=std::fopen(ambiini::LegacyPath,"wb"); assert(f);
    std::string old="ambilight = 1\nambi_ip = 192.168.1.149\nstart_delay = 20\n";
    assert(std::fwrite(old.data(),1,old.size(),f)==old.size()); std::fclose(f);
    std::string ip;
    assert(ambiini::Get("ambi_ip",&ip) && ip=="192.168.1.149");
    assert(ambiini::Set("ambi_brightness","210"));
    assert(ambiini::Get("start_delay",&ip) && ip=="20");
    assert(ambiini::Get("ambi_brightness",&ip) && ip=="210");
    assert(fs::exists(ambiini::Path) && fs::exists(ambiini::LegacyPath));
    f=std::fopen(ambiini::LegacyPath,"rb");assert(f);
    char buf[256]{};auto n=std::fread(buf,1,sizeof(buf),f);std::fclose(f);
    assert(std::string(buf,n).find("ambi_brightness")==std::string::npos);
    puts("OK: v0.6 migration retains original keys and file");
}
