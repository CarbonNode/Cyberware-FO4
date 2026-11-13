// cw_escguard.cpp - see cw_escguard.h. MENU POLISH, 2026-11-08.
#include "PCH.h"
#include "cw_escguard.h"

#include "../core/cw_guardrestore.h"

#include <atomic>
#include <chrono>
#include <string>
#include <string_view>
#include <thread>

namespace escguard {
namespace {

    using clock = std::chrono::steady_clock;

    constexpr auto kRestoreDelay = std::chrono::milliseconds(350);   // the closing key press must have passed
    constexpr auto kWatchAfterClose = std::chrono::milliseconds(2000);

    std::atomic<bool> g_on{ false };             // the guard holds the handlers
    std::atomic<unsigned> g_gen{ 0 };            // bumps on every open/close, so a stale restore does nothing
    std::atomic<long long> g_closedAtMs{ 0 };    // steady clock ms of the last close, for the fallback's log
    std::atomic<long long> g_openedAtMs{ 0 };    // steady clock ms of the last arm, for HeldForMs (softlock 2026-11-13)
    // "IS A RELEASE IN FLIGHT?" - deliberately NOT the same variable as g_on ("am I held?"). Conflating the two
    // is what let a per-tick caller bump the generation ten times inside the 350 ms delay and starve every
    // restore it asked for: the 28 s / 57 s holds and 4,005 warnings of 2026-11-13. core/cw_guardrestore.h.
    std::atomic<bool> g_restoreArmed{ false };

    struct Held {
        RE::BSInputEventUser* handler = nullptr;
        bool was = true;
    };
    Held g_held[2];
    const char* const kNames[2] = { "MenuOpenHandler", "PipboyHandler" };

    long long NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(clock::now().time_since_epoch()).count();
    }

    // Both derive from BSInputEventUser alone (at offset 0); PipboyHandler is only forward-declared in commonlibf4.
    RE::BSInputEventUser* Handler(int i)
    {
        auto* mc = RE::MenuControls::GetSingleton();
        if (!mc) return nullptr;
        return i == 0 ? reinterpret_cast<RE::BSInputEventUser*>(mc->menuOpenHandler)
                      : reinterpret_cast<RE::BSInputEventUser*>(mc->pipboyHandler);
    }

    void Restore()
    {
        for (int i = 0; i < 2; ++i) {
            if (g_held[i].handler && g_held[i].handler == Handler(i)) g_held[i].handler->inputEventHandlingEnabled = g_held[i].was;
            g_held[i] = Held{};
        }
        g_on = false;
        REX::INFO("escguard: off (game menu keys restored)");
    }

    class MenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent> {
    public:
        RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& e, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            if (!e.opening) return RE::BSEventNotifyControl::kContinue;
            const std::string_view name(e.menuName.c_str());
            if (name != "PauseMenu" && name != "PipboyMenu") return RE::BSEventNotifyControl::kContinue;
            if (g_on) {
                if (auto* q = RE::UIMessageQueue::GetSingleton()) q->AddMessage(e.menuName, RE::UI_MESSAGE_TYPE::kHide);
                REX::WARN("escguard: {} opened while the ripperdoc menu holds the keys; closed it (the handler switch did not hold)", name);
            } else if (const long long since = NowMs() - g_closedAtMs.load(); g_closedAtMs.load() && since < kWatchAfterClose.count()) {
                REX::WARN("escguard: {} opened {} ms after the ripperdoc menu closed", name, since);
            } else {
                // Not ours: logged so an unexplained Pause / Pip-Boy (cw-int-2 saw two with no Esc sent) has a timestamp
                // to line up against cw-in.log. Rare event, one line.
                REX::INFO("escguard: {} opened, guard idle (last ripperdoc close {})", name,
                          g_closedAtMs.load() ? std::to_string(since / 1000) + " s ago" : std::string("never this session"));
            }
            return RE::BSEventNotifyControl::kContinue;
        }
    };
    MenuSink g_sink;

}  // namespace

void Register()
{
    if (auto* ui = RE::UI::GetSingleton()) {
        ui->RegisterSink<RE::MenuOpenCloseEvent>(&g_sink);
        REX::INFO("escguard: MenuOpenCloseEvent sink registered");
    }
}

void SetMenuOpen(bool open)
{
    if (open) {
        if (g_on) {
            // ALREADY HELD - but a restore may be IN FLIGHT (a menu reopened inside the 350 ms). Cancel it and keep
            // the guard: that is precisely the generation's job. Without this the stale give-back fires in the middle
            // of the new menu and the game's Pause menu opens under ours - which is what the operator saw on the
            // SECOND Varga prompt of 2026-11-13 ('escguard: PauseMenu opened, guard idle' while a prompt was up).
            if (g_restoreArmed.exchange(false)) {
                ++g_gen;
                REX::INFO("escguard: reopened inside the restore delay: the pending give-back is cancelled, the guard stays on");
            }
            return;
        }
        ++g_gen;                  // an OPEN cancels a pending restore: that is the generation's whole job
        g_restoreArmed = false;   // ... and a cancelled one must not block the next close from re-arming
        for (int i = 0; i < 2; ++i) {
            auto* h = Handler(i);
            if (!h) continue;
            g_held[i] = Held{ h, h->inputEventHandlingEnabled };
            h->inputEventHandlingEnabled = false;
        }
        g_on = true;
        g_openedAtMs = NowMs();
        REX::INFO("escguard: on ({} {}, {} {})", kNames[0], g_held[0].handler ? "held" : "missing", kNames[1],
                  g_held[1].handler ? "held" : "missing");
        return;
    }
    // IDEMPOTENT (core/cw_guardrestore.h): nothing held, or a restore already on its way, means do nothing and
    // above all DO NOT TOUCH THE GENERATION. A close invalidating a close is the defect.
    if (!cw::guardrestore::PlanClose(g_on.load(), g_restoreArmed.load()).schedule) return;
    if (g_restoreArmed.exchange(true)) return;   // lost the race to another thread: its restore serves us too
    const unsigned gen = ++g_gen;
    g_closedAtMs = NowMs();
    // The Esc / Tab that closed the menu is still on its way to the game's input: give it back only afterwards,
    // and only if the menu has not been reopened meanwhile (gen).
    std::thread([gen]() {
        std::this_thread::sleep_for(kRestoreDelay);
        auto* tasks = F4SE::GetTaskInterface();
        // Clear the arm on EVERY exit, or a later close could never re-arm and the guard would be held for good.
        if (!tasks) { g_restoreArmed = false; return; }
        tasks->AddTask([gen]() {
            g_restoreArmed = false;
            if (cw::guardrestore::MayRestore(gen, g_gen.load(), g_on.load())) Restore();
        });
    }).detach();
}

bool ReleasePending() { return g_restoreArmed.load(); }

void ReleaseNow(const char* why)
{
    if (!g_on) return;
    ++g_gen;                  // any restore still sleeping becomes a no-op
    g_restoreArmed = false;
    g_closedAtMs = NowMs();
    REX::WARN("escguard: releasing the handlers NOW ({}): no view of ours on screen and no key press to wait for", why ? why : "?");
    Restore();
}

bool MenuOpen() { return g_on.load(); }

std::uint64_t HeldForMs()
{
    if (!g_on.load()) return 0;
    const long long since = NowMs() - g_openedAtMs.load();
    return since > 0 ? (std::uint64_t)since : 0;
}

}  // namespace escguard
