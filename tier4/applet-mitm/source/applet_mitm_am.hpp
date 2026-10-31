/*
 * v0.7.10: the game's am connection (appletOE), to stop streaming BEFORE a
 * browser applet starts.
 *
 * The browser applets (manuals, news, eShop) need nearly the whole Applet
 * pool, and while a viewer is connected the memory donor holds 42 MB of it.
 * Reacting once the applet is running was too late (v0.7.10: 62 ms after it
 * appeared, it had already failed its allocation - 2210-1024). The game asks
 * am for the applet first: ILibraryAppletCreator::CreateLibraryApplet. Here
 * that request waits until the stream has stopped and the memory is back,
 * then goes on to am unchanged.
 *
 * Wrapped: the application's appletOE root (OpenApplicationProxy, forwarded
 * with the game's pid), its IApplicationProxy (GetLibraryAppletCreator only)
 * and the creator (CreateLibraryApplet only). Every other command forwards
 * untouched (domain objects we do not own, undeclared commands).
 *
 * v0.7.18: "homebrew_docked" (config homebrew_docked = 1, arm word hbdocked):
 * homebrew started from the Homebrew Menu (hbl, holding R over a game) is
 * wrapped too, and its ICommonStateGetter says the console is docked -
 * GetOperationMode Console, GetPerformanceMode Boost, GetDefaultDisplayResolution
 * 1920x1080 - so an app that picks its resolution from that (SDL2 ports,
 * emulators) draws at 1080p; the console's screen scales it as it does a
 * game's 1080p in ReverseNX's docked mode. Retail games are never told.
 */
#pragma once
#include <stratosphere.hpp>

#define AMS_AM_STATE_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 5,  Result, GetOperationMode,            (sf::Out<u8> out), (out)) \
    AMS_SF_METHOD_INFO(C, H, 6,  Result, GetPerformanceMode,          (sf::Out<u32> out), (out)) \
    AMS_SF_METHOD_INFO(C, H, 60, Result, GetDefaultDisplayResolution, (sf::Out<s32> out_w, sf::Out<s32> out_h), (out_w, out_h), hos::Version_3_0_0)

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IAmStateMitm, AMS_AM_STATE_MITM_INTERFACE_INFO, 0x2AB1E153)

#define AMS_AM_CREATOR_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 0, Result, CreateLibraryApplet, (u32 applet_id, u32 mode), (applet_id, mode))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IAmCreatorMitm, AMS_AM_CREATOR_MITM_INTERFACE_INFO, 0x2AB1E150)

#define AMS_AM_PROXY_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 0,  Result, GetCommonStateGetter,    (sf::Out<sf::SharedPointer<ams::mitm::applet::IAmStateMitm>> out), (out)) \
    AMS_SF_METHOD_INFO(C, H, 11, Result, GetLibraryAppletCreator, (sf::Out<sf::SharedPointer<ams::mitm::applet::IAmCreatorMitm>> out), (out))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IAmProxyMitm, AMS_AM_PROXY_MITM_INTERFACE_INFO, 0x2AB1E151)

#define AMS_AM_OE_MITM_INTERFACE_INFO(C, H) \
    AMS_SF_METHOD_INFO(C, H, 0, Result, OpenApplicationProxy, (sf::Out<sf::SharedPointer<ams::mitm::applet::IAmProxyMitm>> out, const sf::ClientProcessId &client_pid, sf::CopyHandle &&process), (out, client_pid, std::move(process)))

AMS_SF_DEFINE_MITM_INTERFACE(ams::mitm::applet, IAmOeMitm, AMS_AM_OE_MITM_INTERFACE_INFO, 0x2AB1E152)

namespace ams::mitm::applet {

    /* v0.7.18: homebrew is told the console is docked (config homebrew_docked) */
    extern std::atomic<bool> g_hb_docked;

    /* Homebrew that runs as an application: the Homebrew Menu over a game
     * (hbl), or an installed homebrew title - a forwarder or an NSP - whose
     * program id is outside Nintendo's application range (all 0100...;
     * sphaira's forwarders are 05...). Forwarders with a 0100... id look like
     * games and are not caught. v0.7.18.1: the forwarders. */
    inline bool IsHomebrewApp(const sm::MitmProcessInfo &c) {
        return c.override_status.IsHbl() || c.program_id.value > ncm::ApplicationId::End.value;
    }

    class AmStateMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            static bool ShouldMitm(const sm::MitmProcessInfo &) { return true; }
        public:
            Result GetOperationMode(sf::Out<u8> out);
            Result GetPerformanceMode(sf::Out<u32> out);
            Result GetDefaultDisplayResolution(sf::Out<s32> out_w, sf::Out<s32> out_h);
    };
    static_assert(IsIAmStateMitm<AmStateMitm>);

    class AmCreatorMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            static bool ShouldMitm(const sm::MitmProcessInfo &) { return true; }
        public:
            Result CreateLibraryApplet(u32 applet_id, u32 mode);
    };
    static_assert(IsIAmCreatorMitm<AmCreatorMitm>);

    class AmProxyMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            static bool ShouldMitm(const sm::MitmProcessInfo &) { return true; }
        public:
            Result GetCommonStateGetter(sf::Out<sf::SharedPointer<IAmStateMitm>> out);
            Result GetLibraryAppletCreator(sf::Out<sf::SharedPointer<IAmCreatorMitm>> out);
    };
    static_assert(IsIAmProxyMitm<AmProxyMitm>);

    class AmOeMitm : public sf::MitmServiceImplBase {
        public:
            using MitmServiceImplBase::MitmServiceImplBase;
            static bool ShouldMitm(const sm::MitmProcessInfo &client_info) {
                /* every game (a browser applet stops the stream first); homebrew
                 * only when it is to be told the console is docked (v0.7.18) */
                if (IsHomebrewApp(client_info)) { return g_hb_docked.load(std::memory_order_relaxed); }
                return ncm::IsApplicationId(client_info.program_id);
            }
        public:
            Result OpenApplicationProxy(sf::Out<sf::SharedPointer<IAmProxyMitm>> out, const sf::ClientProcessId &client_pid, sf::CopyHandle &&process);
    };
    static_assert(IsIAmOeMitm<AmOeMitm>);

    /* main.cpp: register appletOE's mitm on its own server (domains) */
    void StartAmMitm();

}
