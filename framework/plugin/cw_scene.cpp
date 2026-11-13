// cw_scene.cpp - the ripperdoc INSTALL SCENE sequencer. The contract and every beat time: docs/INSTALL-SCENE.md.
// The laws this file obeys and why: cw_scene.h. Pattern: cw_clinic.cpp (its own translation unit, hooks injected
// from main.cpp) + cw_escguard.cpp (hold engine input flags, put back EXACTLY what was there).
#include "PCH.h"
#include "cw_scene.h"

// No cw_voice.h: the 3P switch and the SCN-9 restore are native PlayerCamera::SetState calls now, not Papyrus
// (docs/integration/SCENE-CAMERA-NATIVE-3P.md), so this file uses no Papyrus at all.
#include "cw_scenepkg.h"   // the ready guard's decision, header only and selftested (stale-but-due, 2026-11-10)
#include "../core/cw_evpgate.h"   // the one rule every <ref>.evp obeys: never on an actor whose 3D is away

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace scene
{
    namespace
    {
        // ONE clock for this file. The frame pump hands Tick a GetTickCount64 value, but that symbol is not
        // declared here (PCH.h pulls only RE and F4SE), and mixing two clocks would make Elapsed() nonsense, so the
        // file keeps its own steady_clock and Tick ignores the value it is passed. cw_escguard.cpp does the same.
        using sclock = std::chrono::steady_clock;
        std::uint64_t NowMs()
        {
            return (std::uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(sclock::now().time_since_epoch()).count();
        }

        constexpr float kPi = 3.14159265f;
        constexpr float kDeg = kPi / 180.0f;

        // ---------------------------------------------------------------- the beat table (docs/INSTALL-SCENE.md)
        // Absolute ms from the accepted commit. The full scene ends at 8500, inside the Quality Director's 9 s cap.
        // A player who was ALREADY seated (they activated the chair) skips the dip, and beats 2-3 shrink to the
        // camera move: kSeatedShift comes off every beat from the fade-up on, so that scene ends at 7800.
        struct Beats
        {
            std::uint64_t dip = 250, up = 700, stepIn = 1000, line = 2200, work = 2600;
            std::uint64_t cue1 = 2800, cue2 = 3600, cue3 = 4400;
            std::uint64_t flare = 5000, reveal = 6000, back = 8000, end = 8500;
        };
        constexpr std::uint64_t kSeatedShift = 700;
        constexpr std::uint64_t kDipMs = 450, kFadeUpMs = 600, kFlareMs = 1000, kBarsMs = 350;
        // The short beat and the 2D fallback are the SAME look, so the two can never disagree (docs/INSTALL-SCENE.md).
        constexpr std::uint64_t kShortFlareMs = 800, kShortEnd = 2000;
        // The beats HOLD before the work beat until the sit EVENT (Fix B, core/cw_scenepkg.h). This is only the
        // never-hang guard, not a timing estimate. It was a fixed 2200 ms: b5's player was sitting at the ~12 s read,
        // so the sit landed after the scene had stopped looking (2.9 s) and played on standing.
        constexpr std::uint64_t kSitWaitMs = cw::scenepkg::kSitMaxMs;
        // Her line starts with her wherever she is; the scene does not wait for her walk longer than this.
        constexpr std::uint64_t kHerWaitMs = 2500;
        // Skip is live only after 1.0 s (SCN-10), so the first frames of the scene cannot be eaten by a stray key.
        constexpr std::uint64_t kSkipArmMs = 1000;
        constexpr float kHostileRadius = 3000.0f;   // a hostile in combat this close: no chair scene
        constexpr float kHealthDrop = 1.0f;         // any real health loss mid-scene is an abort

        enum class Mode { None, Chair, Short, Fade };
        const char* ModeName(Mode m) { return m == Mode::Chair ? "chair" : m == Mode::Short ? "short" : m == Mode::Fade ? "fade" : "none"; }

        struct Shot { float yaw = 0, pitch = 0, zoom = 1.6f; bool ok = false; };

        struct DocCfg
        {
            std::string seatPlugin, markPlugin, globPlugin, pkgPlugin;
            std::uint32_t seatId = 0, markId = 0, globId = 0;
            std::vector<std::uint32_t> pkgIds;
            Shot shotA{ 150.0f, 15.0f, 1.6f, true }, shotB{ 210.0f, 10.0f, 0.9f, true };
            bool hasEntry = false;
            float ex = 0, ey = 0, ez = 0;
        };

        struct Cfg
        {
            bool chairScene = false;    // [Scene] bChairScene: OFF in code and Cyberware.ini, ON in Cyberware.Test.ini
            bool camPitch = false;      // [Scene] bCamPitch: ThirdPersonState freeRotation.y as pitch is UNPROVEN
            int flare = 0;              // [Scene] iFlare: 0 = the view's flat layer only; 1 = also the engine DOF A/B
            std::map<std::string, DocCfg> docs;
        } g_cfg;

        Hooks g_h;

        // ---------------------------------------------------------------- what we put back (nothing is in the save)
        struct CamSave
        {
            bool valid = false, was3rd = false, freeRotEnabled = false;
            int startState = -1;   // the camera state READ at the scene start (CameraStates; -1 = none matched)
            float freeX = 0, freeY = 0, targetZoom = 0, curZoom = 0;
        };

        struct HeldInput { void* handler = nullptr; bool was = true; };

        struct Live
        {
            Mode mode = Mode::None;
            bool active = false;
            std::string doc, kind, name, key, text;
            Beats t;
            std::uint64_t startMs = 0, pausedMs = 0, consoleSince = 0;
            std::uint64_t holdMs = 0;      // Fix B: beat-clock time held waiting for the sit (NOT console time)
            bool holding = false;          // the beats are held right now
            std::uint64_t lastPhaseAt = 0;
            // seatedNow / sitGaveUp mirror `sit` (they are read by the end beat and the hold). seatedNow is true from
            // the first sitting read at ANY time in the scene, late included: it is never frozen at the window.
            bool seatedAtStart = false, seatedNow = false, sitAsked = false, sitGaveUp = false;
            cw::scenepkg::SitWatch sit;    // every SIT_SLEEP_STATE change from the ask to the end, with its ms
            cw::scenepkg::CamWatch camw;   // the native 3P request, read back every frame from the request to the end
            bool dipDone = false, upDone = false, stepDone = false, lineDone = false, workDone = false;
            bool cue1 = false, cue2 = false, cue3 = false;
            bool flareDone = false, revealDone = false, backDone = false;
            bool skipped = false;
            std::uint32_t seatRef = 0, docRef = 0;
            float health0 = 0;
            CamSave cam;
            std::vector<HeldInput> input;
            bool globWritten = false;
            int shot = 0;                  // 0 = none held, 1 = shot A, 2 = shot B
            Shot held;
            float pushFrom = 1.6f, pushTo = 1.4f;
            std::string why;               // the fallback's reason, logged and answered to the view
            std::uint32_t staleFrom = 0;   // the stale package she ran at commit (stale-due), logged with her distance at start
        } g_live;

        int g_visitCommits = 0;            // commits in THIS visit: 1 = full scene, later = short beat
        std::string g_visitDoc;

        // The scene is over but the VISIT is not: the player sits behind the menu until it closes (SCN-9 stands them
        // up then, with the furniture's own exit animation, and puts the camera and view back). This outlives g_live
        // on purpose, because the next commit of the same visit calls ResetLive() for its short beat.
        std::uint32_t g_holdSeat = 0;
        CamSave g_holdCam;
        // After the visit: the stand-up is VERIFIED by a live read, never assumed from the request (QD-B, 2026-11-10:
        // "visit ended ..., player stood up" used to print the standUp PARAMETER, not what happened).
        struct StandWatch { std::uint32_t seat = 0; std::uint64_t since = 0; bool exitAsked = false; } g_stand;
        // {"cmd":"scene","what":"sitwatch","ms":N}: the SAME per-frame sit tracer (scenepkg::Observe) with no scene, so a
        // console activate can be the positive control for the scene's trace in the same boot. Console time excluded.
        // The SCN-9 restore's camera, read back after the visit ended (Tick's WatchCamRestore), on the wall clock.
        cw::scenepkg::CamWatch g_camRestore;
        struct ProbeWatch { bool on = false; std::uint64_t since = 0, ms = 0, paused = 0, consoleSince = 0; cw::scenepkg::SitWatch w; } g_probe;
        std::uint32_t g_holdDoc = 0;

        // ---------------------------------------------------------------- small helpers
        bool ParseRef(const std::string& s, std::string& plugin, std::uint32_t& id)
        {
            const auto bar = s.find('|');
            if (bar == std::string::npos) return false;
            plugin = s.substr(0, bar);
            id = (std::uint32_t)std::strtoul(s.c_str() + bar + 1, nullptr, 16) & 0x00FFFFFF;
            return !plugin.empty() && id != 0;
        }

        // "Plugin.esp|975,A4C": one plugin, then every local id that means "she is at her station and free".
        void ParsePkgs(const std::string& s, std::string& plugin, std::vector<std::uint32_t>& out)
        {
            out.clear();
            const auto bar = s.find('|');
            if (bar == std::string::npos) return;
            plugin = s.substr(0, bar);
            std::string rest = s.substr(bar + 1);
            std::size_t at = 0;
            while (at < rest.size()) {
                const auto comma = rest.find(',', at);
                const std::string tok = rest.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                if (const auto id = (std::uint32_t)std::strtoul(tok.c_str(), nullptr, 16) & 0x00FFFFFF) out.push_back(id);
                if (comma == std::string::npos) break;
                at = comma + 1;
            }
        }

        Shot ParseShot(const std::string& s, const Shot& def)
        {
            Shot sh = def;
            float y = 0, p = 0, z = 0;
            if (std::sscanf(s.c_str(), "%f,%f,%f", &y, &p, &z) == 3) { sh.yaw = y; sh.pitch = p; sh.zoom = z; sh.ok = true; }
            return sh;
        }

        RE::TESObjectREFR* RefOf(const std::string& plugin, std::uint32_t local)
        {
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh || !local || plugin.empty()) return nullptr;
            auto* form = dh->LookupForm(local, plugin);
            return form ? form->As<RE::TESObjectREFR>() : nullptr;
        }

        constexpr int kIsSitting = 3;   // ActorState::SIT_SLEEP_STATE::kIsSitting
        bool Sitting(RE::Actor* a)
        {
            if (!a) return false;
            return static_cast<int>(static_cast<RE::ActorState*>(a)->DoGetSitSleepState()) == kIsSitting;
        }
        // The camera state READ now: the first CameraState QCameraEquals answers true for (-1 = none). QCameraEquals is the
        // read the plugin already uses on OG 1.10.163.
        int CamStateNow()
        {
            auto* cam = RE::PlayerCamera::GetSingleton();
            if (!cam) return -1;
            for (int i = 0; i < static_cast<int>(RE::CameraStates::kTotal); ++i)
                if (cam->QCameraEquals(static_cast<RE::CameraState>(i))) return i;
            return -1;
        }
        // The native switch: PlayerCamera::SetState to the camera's own state object (commonlibf4 a08c731, OG id 858847).
        // Returns whether the call was ISSUED, never whether the camera moved: the CamWatch read says that.
        bool CamSet(int state)
        {
            auto* cam = RE::PlayerCamera::GetSingleton();
            if (!cam || state < 0 || state >= static_cast<int>(RE::CameraStates::kTotal)) return false;
            auto* st = cam->cameraStates[state].get();
            if (!st) return false;
            cam->SetState(st);
            return true;
        }
        // The raw SIT_SLEEP_STATE, -1 when there is no actor. Logged as a number AND a name (scenepkg::SitStateName).
        int SitState(RE::Actor* a)
        {
            if (!a) return -1;
            return static_cast<int>(static_cast<RE::ActorState*>(a)->DoGetSitSleepState());
        }

        // Console `evp`: the engine re-picks her package. The same path CLINIC's nudge uses, never Papyrus.
        //
        // GATED HERE, not at the call sites (2026-11-12, the navmesh-crash attribution). evp makes the engine pick a
        // package and so BUILD A PATH, and an unloaded actor's cell and navmesh are not in memory -- the law and the
        // measurement are in core/cw_evpgate.h and core/cw_clinicguard.h. Three of this file's four callers reach here
        // from a scene ENDING: EndRun runs on the menu closing, ON AN ABORT, AND ON A LOAD, and both it and the
        // back-beat end resolve the doctor from a stored FormID (g_live.docRef / g_holdDoc) through
        // TESForm::GetFormByID, which returns a reference whose 3D may be long gone. A load IS that moment. Putting
        // the gate in this one helper covers every present caller and every future one, and reads the 3D state at the
        // instant of the call rather than at some earlier check.
        //
        // Skipping costs nothing: a stale package on an unloaded actor is invisible, and the clinic nudge re-arms on
        // her unloaded -> loaded edge and evaluates her back onto the clock's package (cw_clinic.cpp).
        void Evaluate(RE::TESObjectREFR* ref)
        {
            const auto v = cw::evpgate::Check(ref != nullptr, ref && ref->Get3D() != nullptr);
            if (!cw::evpgate::Allowed(v)) {
                if (ref)
                    REX::INFO("scene: no package evaluation on {:08X} - {}", ref->GetFormID(), cw::evpgate::Why(v));
                return;
            }
            char cmd[32];
            std::snprintf(cmd, sizeof(cmd), "%08X.evp", ref->GetFormID());
            RE::Console::ExecuteCommand(cmd);
        }

        // PlayerControls' handlers are BSInputEventUser subobjects; commonlibf4 may hold them raw or in a smart
        // pointer, so one overload set covers both and the file compiles either way (cw_escguard.cpp's flag trick,
        // never BSInputEnableLayer: that one IS saved into the save).
        RE::BSInputEventUser* AsUser(RE::BSInputEventUser* p) { return p; }
        template <class T> RE::BSInputEventUser* AsUser(T* p) { return reinterpret_cast<RE::BSInputEventUser*>(p); }
        template <class T> RE::BSInputEventUser* AsUser(const RE::BSTSmartPointer<T>& p) { return reinterpret_cast<RE::BSInputEventUser*>(p.get()); }

        void HoldInput()
        {
            if (!g_live.input.empty()) return;
            auto* pc = RE::PlayerControls::GetSingleton();
            if (!pc) { REX::WARN("scene: no PlayerControls; the player keeps control through the scene"); return; }
            for (auto& e : pc->handlers) {
                auto* u = AsUser(e);
                if (!u) continue;
                g_live.input.push_back(HeldInput{ u, u->inputEventHandlingEnabled });
                u->inputEventHandlingEnabled = false;
            }
            REX::INFO("scene: input held ({} PlayerControls handlers; Esc/Tab are escguard's)", g_live.input.size());
        }

        void ReleaseInput()
        {
            if (g_live.input.empty()) return;
            // Put back EXACTLY what each handler had. A handler the game has since replaced is left alone.
            auto* pc = RE::PlayerControls::GetSingleton();
            int restored = 0;
            if (pc) {
                for (auto& held : g_live.input) {
                    for (auto& e : pc->handlers) {
                        auto* u = AsUser(e);
                        if (u && u == held.handler) { u->inputEventHandlingEnabled = held.was; ++restored; break; }
                    }
                }
            }
            REX::INFO("scene: input released ({} of {} handlers put back)", restored, g_live.input.size());
            g_live.input.clear();
        }

        void SaveCamera()
        {
            g_live.cam = CamSave{};
            auto* cam = RE::PlayerCamera::GetSingleton();
            if (!cam) return;
            g_live.cam.was3rd = cam->QCameraEquals(RE::CameraState::k3rdPerson);
            g_live.cam.startState = CamStateNow();
            auto sp = cam->GetState(RE::CameraState::k3rdPerson);
            if (auto* st = static_cast<RE::ThirdPersonState*>(sp.get())) {
                g_live.cam.freeRotEnabled = st->freeRotationEnabled;
                g_live.cam.freeX = st->freeRotation.x;
                g_live.cam.freeY = st->freeRotation.y;
                g_live.cam.targetZoom = st->targetZoomOffset;
                g_live.cam.curZoom = st->currentZoomOffset;
                g_live.cam.valid = true;
            }
            // "was" is the state READ now, by name (it used to print "1P" for anything that was not 3P)
            REX::INFO("scene: camera saved (was {} read now, freeRot {} x {:.3f} y {:.3f}, zoom target {:.2f} cur {:.2f})",
                      cw::scenepkg::CamStateName(g_live.cam.startState), g_live.cam.freeRotEnabled ? "on" : "off",
                      g_live.cam.freeX, g_live.cam.freeY, g_live.cam.targetZoom, g_live.cam.curZoom);
        }

        void RestoreCamera(bool restorePov)
        {
            auto* cam = RE::PlayerCamera::GetSingleton();
            if (cam && g_live.cam.valid) {
                auto sp = cam->GetState(RE::CameraState::k3rdPerson);
                if (auto* st = static_cast<RE::ThirdPersonState*>(sp.get())) {
                    st->freeRotationEnabled = g_live.cam.freeRotEnabled;
                    st->freeRotation.x = g_live.cam.freeX;
                    st->freeRotation.y = g_live.cam.freeY;
                    st->targetZoomOffset = g_live.cam.targetZoom;
                    st->currentZoomOffset = g_live.cam.curZoom;
                }
            }
            // SCN-9: the view the player had. Native SetState now (was Papyrus ForcePerson(false)); the restore is READ back
            // after the visit by WatchCamRestore. Never after a load: that player's view belongs to the save.
            if (restorePov && g_live.cam.valid && !g_live.cam.was3rd) {
                const int before = CamStateNow();
                const bool issued = CamSet(cw::scenepkg::kCamFirst);
                cw::scenepkg::CamAsk(g_camRestore, cw::scenepkg::kCamFirst, NowMs());
                REX::INFO("scene: camera firstperson requested natively for the SCN-9 restore (SetState {}; the scene started in {}, the camera is {} read now)",
                          issued ? "issued" : "NOT issued", cw::scenepkg::CamStateName(g_live.cam.startState), cw::scenepkg::CamStateName(before));
            }
            g_live.shot = 0;
            g_live.cam = CamSave{};
        }

        void ApplyShot(const Shot& sh)
        {
            auto* cam = RE::PlayerCamera::GetSingleton();
            if (!cam || !cam->QCameraEquals(RE::CameraState::k3rdPerson)) return;
            auto sp = cam->GetState(RE::CameraState::k3rdPerson);
            auto* st = static_cast<RE::ThirdPersonState*>(sp.get());
            if (!st) return;
            st->freeRotationEnabled = true;
            st->freeRotation.x = sh.yaw * kDeg;
            if (g_cfg.camPitch) st->freeRotation.y = sh.pitch * kDeg;   // UNPROVEN as pitch: [Scene] bCamPitch
            st->targetZoomOffset = sh.zoom;
            st->currentZoomOffset = sh.zoom;
        }

        void WriteGlobal(float v)
        {
            const auto it = g_cfg.docs.find(g_live.doc);
            if (it == g_cfg.docs.end() || !it->second.globId) return;
            auto* dh = RE::TESDataHandler::GetSingleton();
            if (!dh) return;
            auto* form = dh->LookupForm(it->second.globId, it->second.globPlugin);
            auto* glob = form ? form->As<RE::TESGlobal>() : nullptr;
            if (!glob) {
                if (v > 0.5f) REX::WARN("scene: [Scene] sSceneGlobal_{} does not resolve ({}|{:X}); she works from where she stands",
                                        g_live.doc, it->second.globPlugin, it->second.globId);
                return;
            }
            glob->value = v;                       // the DLL only ever WRITES it; her package conditions on it
            g_live.globWritten = v > 0.5f;
            REX::INFO("scene: {} = {:.0f}", glob->GetFormEditorID() ? glob->GetFormEditorID() : "scene global", v);
        }

        void Push(const char* phase, cw::json extra = cw::json::object())
        {
            if (!g_h.push) return;
            cw::json ev = extra;
            ev["type"] = "scene";
            ev["phase"] = phase;
            ev["mode"] = ModeName(g_live.mode);
            ev["kind"] = g_live.kind;
            ev["name"] = g_live.name;
            g_h.push(ev);
            if (g_live.mode != Mode::None) REX::INFO("scene: phase {} ({})", phase, ModeName(g_live.mode));
        }

        void ResetLive()
        {
            const Mode was = g_live.mode;
            g_live = Live{};
            (void)was;
        }

        // The talk-guard exemption (see Hooks::sceneBusy). One setter, so the flag cannot be set in one place and
        // forgotten in another: Start sets it for all three modes, EndRun and the chair end beat clear it, OnLoad
        // clears it unconditionally. A stranded true would leave CRASH's guard permanently exempt, which is why this
        // is a single function with an idempotence check rather than a line at each site.
        bool g_busy = false;
        void SetBusy(bool on)
        {
            if (g_busy == on) return;
            g_busy = on;
            if (g_h.sceneBusy) g_h.sceneBusy(on);
        }

        // TWO LEVELS, because a VISIT outlives a SCENE.
        //
        // EndRun ends the run that is playing now: the input lock, the effects, her global, her line. It never
        // touches the camera or the chair, so the short beat of a second commit cannot yank a seated player's
        // camera back or stand them up mid-visit (the bug this split exists to prevent).
        //
        // EndVisit ends the whole visit: EndRun, then the camera and view go back and the player gets out of the
        // chair. Only the menu closing, an abort and a load do that.
        void EndRun(const char* why)
        {
            if (g_live.globWritten) {
                const std::uint32_t docRef = g_live.docRef ? g_live.docRef : g_holdDoc;
                WriteGlobal(0.0f);
                if (auto* d = docRef ? RE::TESForm::GetFormByID(docRef) : nullptr)
                    if (auto* r = d->As<RE::TESObjectREFR>()) Evaluate(r);   // back to her own schedule at once
            }
            ReleaseInput();
            if (g_cfg.flare > 0) {   // the engine DOF A/B must never leave a trace (SCN-12)
                if (auto* f = RE::TESForm::GetFormByID(0x00024590))
                    if (auto* m = f->As<RE::TESImageSpaceModifier>()) RE::ImageSpaceModifierInstanceForm::Stop(m);
            }
            if (g_h.hush) g_h.hush();
            SetBusy(false);                      // the talk guard is live again the moment the run is over
            if (g_live.mode != Mode::None || g_live.active) REX::INFO("scene: run ended ({})", why);
            ResetLive();
        }

        void EndVisit(const char* why, bool standUp, bool restorePov)
        {
            const std::uint32_t seatRef = g_live.seatRef ? g_live.seatRef : g_holdSeat;
            // The live sit state, read FIRST, before the run ends, the camera moves or anything touches the player.
            auto* p = RE::PlayerCharacter::GetSingleton();
            const int live = SitState(p);
            const bool anything = g_live.active || g_live.mode != Mode::None || !g_live.input.empty() ||
                                  g_live.cam.valid || g_holdCam.valid || g_holdSeat != 0 || g_live.globWritten;
            if (!anything) return;                      // an ordinary menu close with no scene costs nothing
            if (!g_live.cam.valid && g_holdCam.valid) g_live.cam = g_holdCam;
            EndRun(why);
            RestoreCamera(restorePov);
            // What happens to the player is decided from the LIVE sit state, and the line below says what was DONE,
            // never what was asked for. A stand-up is only "verified" by a later live read of 0 (Tick's WatchStand).
            const auto plan = cw::scenepkg::PlanStand(standUp, seatRef != 0, live);
            g_stand = StandWatch{};
            std::string did;
            if (plan == cw::scenepkg::StandPlan::Exit) {
                // The furniture's own exit animation: activating the seat the player occupies stands them up.
                bool asked = false;
                if (auto* seat = RE::TESForm::GetFormByID(seatRef))
                    if (auto* sr = seat->As<RE::TESObjectREFR>()) {
                        asked = sr->ActivateRef(p, nullptr, 1, false, false, false);
                        REX::INFO("scene: stand up through seat {:08X} -> {}", seatRef, asked ? "accepted" : "refused");
                    }
                g_stand = StandWatch{ seatRef, NowMs(), true };
                did = asked ? "stand-up ASKED (exit accepted; standing is verified by a live read)" : "stand-up REFUSED by the seat";
            } else if (plan == cw::scenepkg::StandPlan::WatchPending) {
                g_stand = StandWatch{ seatRef, NowMs(), false };
                did = "sit still pending; watched, and the exit is asked if it lands";
            } else if (plan == cw::scenepkg::StandPlan::NotNeeded) {
                did = "no stand-up needed (not sitting)";
            } else {
                // The caller's choice is named as a choice, and the live state printed beside it can contradict it.
                did = standUp ? "no stand-up possible: no seat ref held (a visit with no chair)" : "the caller does not stand the player up here (a load)";
            }
            g_holdSeat = 0;
            g_holdDoc = 0;
            g_holdCam = CamSave{};
            g_live.cam = CamSave{};
            // The live state is read at the close, BEFORE anything touches the player (it is the read the stand-up
            // decision is made from). "Stand-up ASKED" is a request; only the later "stand-up VERIFIED" is an outcome.
            REX::INFO("scene: visit ended ({}): live sit state {} ({}) read at the close, seat ref {:08X}; {}", why, live,
                      cw::scenepkg::SitStateName(live), seatRef, did);
        }

        std::uint64_t Elapsed(std::uint64_t now)
        {
            if (now < g_live.startMs) return 0;
            const std::uint64_t raw = now - g_live.startMs;
            const std::uint64_t off = g_live.pausedMs + g_live.holdMs;  // console time and the sit hold do not count
            return raw > off ? raw - off : 0;
        }

        // Scene time WITHOUT the sit hold (console time still excluded): the sit watch and the lockout run on this.
        std::uint64_t SceneMs(std::uint64_t now)
        {
            if (now < g_live.startMs) return 0;
            const std::uint64_t raw = now - g_live.startMs;
            return raw > g_live.pausedMs ? raw - g_live.pausedMs : 0;
        }

        // Guards before the chair path. Returns "" when the chair scene may run, else the reason for the fallback.
        std::string ChairGuards(const DocCfg& dc, RE::Actor*& docActor, RE::TESObjectREFR*& seat)
        {
            auto* p = RE::PlayerCharacter::GetSingleton();
            if (!p) return "no player";
            if (!g_cfg.chairScene) return "[Scene] bChairScene=0";
            if (!dc.seatId) return "this doctor has no seat in [Scene]";
            seat = RefOf(dc.seatPlugin, dc.seatId);
            if (!seat) return "the seat record is not loaded (a Phase 1 clinic build?)";
            if (!seat->Get3D() || seat->GetParentCell() != p->GetParentCell()) return "the seat is not in the player's cell";
            if (RE::PowerArmor::ActorInPowerArmor(*p)) return "the player is in power armor and cannot sit";
            if (p->IsInCombat()) return "the player is in combat";
            if (g_h.hostileNear && g_h.hostileNear(kHostileRadius)) return "a hostile is in combat nearby";
            if (g_h.blockingMenu) { if (const char* m = g_h.blockingMenu()) return std::string("a game menu is open (") + m + ")"; }
            docActor = g_h.findDoc ? g_h.findDoc(g_live.doc) : nullptr;
            if (!docActor || !docActor->Get3D()) return "the doctor is not loaded here";
            if (docActor->lifeState != 0) return "the doctor is not alive";
            // CLINIC, 2026-11-10: before 10:00 she is in bed or at the office chair. Starting the scene then would
            // pull her out of bed, so unless one of her "at the station and free" packages is running, take the
            // fallback. No pkgs configured = no check.
            if (!dc.pkgIds.empty()) {
                // Stale-but-due (the lead, 2026-11-10, T19): with [Clinic] bSchedule=0 the engine does not re-pick
                // her package while she stays loaded, and CLINIC's nudge never runs in a talk, so a player who waits past
                // 10:00 and talks at once finds her on the office package with the clock already at the station. If the
                // running package is one of HER OWN and her clock wants a station package now, the chair scene runs: its
                // start writes the global and evaluates her package with the world UNPAUSED, which picks the step-in
                // (first in her list). The decision is core/cw_scenepkg.h, selftested. Bed and office time, and anyone
                // else's package, still take the fallback.
                auto* dh = RE::TESDataHandler::GetSingleton();
                auto* proc = docActor->currentProcess;
                auto* pkg = proc ? proc->currentPackage.package : nullptr;
                const std::uint32_t running = pkg ? pkg->GetFormID() : 0;
                std::vector<std::uint32_t> ready;
                for (const auto local : dc.pkgIds)
                    if (auto* f = dh ? dh->LookupForm(local, dc.pkgPlugin) : nullptr) ready.push_back(f->GetFormID());
                bool hers = false;
                const std::uint32_t due = g_h.due ? g_h.due(docActor->GetFormID(), running, hers) : 0;
                const auto verdict = cw::scenepkg::Judge(running, ready, hers, due);
                if (verdict == cw::scenepkg::Verdict::StaleDue) {
                    g_live.staleFrom = running;
                    REX::INFO("scene: her package {:08X} is stale: her clock wants {:08X}, a station package, so the chair scene runs and evaluates her onto the step-in",
                              running, due);
                } else if (verdict == cw::scenepkg::Verdict::NotReady) {
                    char buf[200];
                    std::snprintf(buf, sizeof(buf), "she is not at her station (package %08X is not one of her %zu station packages; clock wants %08X, %s)",
                                  running, dc.pkgIds.size(), due, hers ? "hers" : "not hers or no clinic opinion");
                    return buf;
                }
            }
            return "";
        }

        // Beat 2: put a standing player at the chair's entry, then let the furniture's own enter animation play.
        // The entry is [Scene] sEntry_<doc>, else 90 u off the seat's FOOT end: with heading h the seat's local +x is
        // (cos h, -sin h) and the head end lies that way (CLINIC: Varga's chair rz 1.571, head end south), so the
        // feet are at local -x = (-cos h, sin h).
        void MoveToEntry(RE::PlayerCharacter* p, RE::TESObjectREFR* seat, const DocCfg& dc)
        {
            if (!p || !seat) return;
            RE::NiPoint3 to{};
            if (dc.hasEntry) {
                to = RE::NiPoint3(dc.ex, dc.ey, dc.ez);
            } else {
                const auto sp = seat->GetPosition();
                const float h = seat->data.angle.z;   // OBJ_REFR::angle, not GetAngleZ (no such member)
                to = RE::NiPoint3(sp.x - 90.0f * std::cos(h), sp.y + 90.0f * std::sin(h), sp.z);
            }
            p->SetLocationOnReference(to);
            REX::INFO("scene: player moved to the chair entry ({:.0f},{:.0f},{:.0f}){}", to.x, to.y, to.z,
                      dc.hasEntry ? " [sEntry]" : " [90 u off the foot end]");
        }
    }  // namespace

    // ---------------------------------------------------------------- configuration
    void Configure(bool chairScene, bool camPitch, int flare, const std::vector<DocIni>& docs)
    {
        g_cfg.chairScene = chairScene;
        g_cfg.camPitch = camPitch;
        g_cfg.flare = std::clamp(flare, 0, 1);
        g_cfg.docs.clear();
        for (const auto& d : docs) {
            DocCfg c;
            ParseRef(d.seat, c.seatPlugin, c.seatId);
            ParseRef(d.mark, c.markPlugin, c.markId);
            ParseRef(d.global, c.globPlugin, c.globId);
            ParsePkgs(d.pkgs, c.pkgPlugin, c.pkgIds);
            c.shotA = ParseShot(d.shotA, c.shotA);
            c.shotB = ParseShot(d.shotB, c.shotB);
            if (std::sscanf(d.entry.c_str(), "%f,%f,%f", &c.ex, &c.ey, &c.ez) == 3) c.hasEntry = true;
            // Every value read is logged: a scene that silently took the fallback cost cycle 3 a whole boot.
            REX::INFO("ini: [Scene] {}: seat {}|{:X} mark {}|{:X} glob {}|{:X} pkgs {}({}) shotA {:.0f},{:.0f},{:.2f} shotB {:.0f},{:.0f},{:.2f} entry {}",
                      d.doc, c.seatPlugin, c.seatId, c.markPlugin, c.markId, c.globPlugin, c.globId,
                      c.pkgPlugin, c.pkgIds.size(), c.shotA.yaw, c.shotA.pitch, c.shotA.zoom,
                      c.shotB.yaw, c.shotB.pitch, c.shotB.zoom, c.hasEntry ? d.entry : std::string("foot end + 90 u"));
            g_cfg.docs[d.doc] = c;
        }
        REX::INFO("ini: [Scene] bChairScene={} bCamPitch={} iFlare={} ({} doctors)", chairScene ? 1 : 0, camPitch ? 1 : 0, g_cfg.flare, g_cfg.docs.size());
    }

    void SetHooks(Hooks h) { g_h = std::move(h); }

    bool Active() { return g_live.active; }

    // ---------------------------------------------------------------- start
    cw::json Start(const cw::json& args)
    {
        const std::string doc = args.value("doc", std::string());
        const std::string kind = args.value("kind", std::string("install")) == "remove" ? "remove" : "install";
        if (g_live.active) {
            // A second commit while the scene runs: the piece is already installed, so the honest answer is the
            // short beat's look with no second camera move, and her line is never cut off (SCN-6).
            Push("flare", cw::json{ { "ms", kShortFlareMs }, { "overlap", true } });
            return cw::json{ { "ok", true }, { "data", { { "mode", "short" }, { "ms", kShortEnd }, { "why", "a scene was already running" } } } };
        }
        if (doc != g_visitDoc) { g_visitDoc = doc; g_visitCommits = 0; }
        ++g_visitCommits;

        ResetLive();
        g_live.doc = doc;
        g_live.kind = kind;
        g_live.name = args.value("name", std::string());
        g_live.key = args.value("key", std::string());
        g_live.text = args.value("text", std::string());

        auto* p = RE::PlayerCharacter::GetSingleton();
        const auto it = g_cfg.docs.find(doc);
        const DocCfg dc = it == g_cfg.docs.end() ? DocCfg{} : it->second;

        // Second and later commits of a visit: the short beat, in place (the player stays seated the whole visit).
        if (g_visitCommits > 1) {
            g_live.mode = Mode::Short;
            g_live.active = true;
            SetBusy(true);
            g_live.startMs = NowMs();
            g_live.seatedAtStart = Sitting(p);
            Push("flare", cw::json{ { "ms", kShortFlareMs } });
            if (g_h.cue) g_h.cue("CWC_SND_InstallPart", kind == "remove" ? "UIModsBreakDown" : "UIModsBuild");
            REX::INFO("scene: commit {} of this visit at '{}': short beat", g_visitCommits, doc);
            return cw::json{ { "ok", true }, { "data", { { "mode", "short" }, { "ms", kShortEnd } } } };
        }

        RE::Actor* docActor = nullptr;
        RE::TESObjectREFR* seat = nullptr;
        const std::string why = ChairGuards(dc, docActor, seat);
        if (!why.empty()) {
            // The fallback is NOT the old black card: the world stays visible behind a 0.8 s flare (SCN-1).
            g_live.mode = Mode::Fade;
            g_live.active = true;
            SetBusy(true);
            g_live.startMs = NowMs();
            g_live.why = why;
            // Voiced if she is loaded and near, and never gating the beat (docs/INSTALL-SCENE.md, the fallback).
            const float voiceMs = (docActor && g_h.say && !g_live.key.empty()) ? g_h.say(doc, g_live.key) : 0.0f;
            Push("flare", cw::json{ { "ms", kShortFlareMs }, { "why", why }, { "voiceMs", voiceMs }, { "text", g_live.text } });
            if (g_h.cue) g_h.cue("CWC_SND_InstallPart", kind == "remove" ? "UIModsBreakDown" : "UIModsBuild");
            REX::INFO("scene: 2D fallback at '{}': {}", doc, why);
            return cw::json{ { "ok", true }, { "data", { { "mode", "fade" }, { "ms", kShortEnd }, { "why", why } } } };
        }

        // ---- the chair scene
        g_live.mode = Mode::Chair;
        g_live.active = true;
        SetBusy(true);
        g_live.startMs = NowMs();
        g_live.seatRef = seat->GetFormID();
        g_live.docRef = docActor->GetFormID();
        g_live.seatedAtStart = Sitting(p);
        g_live.seatedNow = g_live.seatedAtStart;
        g_live.health0 = g_h.playerHealth ? g_h.playerHealth() : 0.0f;
        g_live.pushFrom = dc.shotA.zoom;
        g_live.pushTo = dc.shotA.zoom * 0.88f;      // SCN-4: a push-in a viewer can see, at least 8% closer
        if (g_live.seatedAtStart) {                  // beats 2-3 shrink to the camera move
            g_live.t.up = g_live.t.up > kSeatedShift ? g_live.t.up - kSeatedShift : 0;
            g_live.t.stepIn -= kSeatedShift; g_live.t.line -= kSeatedShift; g_live.t.work -= kSeatedShift;
            g_live.t.cue1 -= kSeatedShift; g_live.t.cue2 -= kSeatedShift; g_live.t.cue3 -= kSeatedShift;
            g_live.t.flare -= kSeatedShift; g_live.t.reveal -= kSeatedShift;
            g_live.t.back -= kSeatedShift; g_live.t.end -= kSeatedShift;
            g_live.dipDone = true;
        }

        // A world scene cannot run in a paused game: unpause for the scene's length and re-pause on the way back
        // (docs/INSTALL-SCENE.md, the bPauseGame row). Voice also needs an unpaused view.
        if (g_h.menuPaused && g_h.menuPaused() && g_h.refocus) g_h.refocus(false);
        HoldInput();
        SaveCamera();
        if (!g_live.cam.was3rd) {                            // the scene is framed in 3P; SCN-9 puts 1P back
            const bool issued = CamSet(cw::scenepkg::kCamThird);
            cw::scenepkg::CamAsk(g_live.camw, cw::scenepkg::kCamThird, 0);   // scene clock: Start is 0 ms
            REX::INFO("scene: camera thirdperson requested natively at 0 ms (PlayerCamera::SetState {}; was {} read at the start); every frame reads it back",
                      issued ? "issued" : "NOT issued (no camera or no 3P state)", cw::scenepkg::CamStateName(g_live.cam.startState));
        }
        WriteGlobal(1.0f);
        Evaluate(docActor);                                 // she re-picks and walks to her mark, on foot

        Push("bars", cw::json{ { "ms", kBarsMs }, { "total", g_live.t.end }, { "seated", g_live.seatedAtStart } });
        REX::INFO("scene: chair scene at '{}' ({} '{}'), seat {:08X}, doc {:08X}, {} ms, player {}",
                  doc, kind, g_live.name, g_live.seatRef, g_live.docRef, g_live.t.end,
                  g_live.seatedAtStart ? "already seated" : "standing");
        if (g_live.staleFrom) {
            // She starts from wherever the stale package left her (the office chair is ~510 u from her mark), so her
            // walk can outlast beat 4 (kHerWaitMs): the line starts where she is and is never held. Logged so the
            // capture is read against the real distance, not the 29 u of the usual seat-to-mark walk.
            auto* mark = RefOf(dc.markPlugin, dc.markId);
            const auto dp = docActor->GetPosition();
            const float d = mark ? std::hypot(dp.x - mark->GetPosition().x, dp.y - mark->GetPosition().y) : -1.0f;
            REX::INFO("scene: stale start from package {:08X}, she is {:.0f} u from her mark", g_live.staleFrom, d);
        }
        return cw::json{ { "ok", true }, { "data", { { "mode", "chair" }, { "ms", g_live.t.end }, { "seated", g_live.seatedAtStart } } } };
    }

    // ---------------------------------------------------------------- skip and abort
    void Skip()
    {
        if (!g_live.active) return;
        const auto el = Elapsed(NowMs());
        if (g_live.mode == Mode::Chair && el < kSkipArmMs) return;      // SCN-10: live only after 1.0 s
        if (g_live.skipped) return;                                     // a second press does nothing harmful
        g_live.skipped = true;
        if (g_live.mode != Mode::Chair) { Push("end", cw::json{ { "skipped", true } }); EndRun("skipped"); return; }
        // Jump to the reveal's last second: the cut and the lower-third happen, the bars and the blur do not linger.
        const std::uint64_t jumpTo = g_live.t.back > 1000 ? g_live.t.back - 1000 : 0;
        if (el < jumpTo) g_live.startMs = NowMs() - jumpTo - g_live.pausedMs - g_live.holdMs;
        g_live.holding = false;                                          // a skip never waits for the sit
        g_live.cue1 = g_live.cue2 = g_live.cue3 = true;
        g_live.dipDone = g_live.upDone = g_live.stepDone = g_live.lineDone = g_live.workDone = true;
        g_live.flareDone = true;
        REX::INFO("scene: skipped at {} ms -> the reveal's last second", el);
    }

    void Abort(const char* why)
    {
        if (!g_live.active) return;
        const bool wasChair = g_live.mode == Mode::Chair;
        Push("abort", cw::json{ { "why", why ? why : "" } });
        // Control back in under 0.5 s and the install stays: it was committed before beat 1.
        // The install stays: it was committed before beat 1. Control is back inside 0.5 s.
        EndVisit(why ? why : "abort", wasChair || g_holdSeat != 0, true);
        if (g_h.menuPaused && g_h.menuPaused() && g_h.refocus) g_h.refocus(true);
    }

    void OnMenuClosed()
    {
        g_visitCommits = 0;
        g_visitDoc.clear();
        // The visit ends here whether or not a scene is still running: after a seated visit the player is standing in
        // the chair behind the menu (g_holdSeat), and THIS is the moment they get up. Teardown no-ops when there is
        // nothing held, so an ordinary menu close with no scene costs nothing.
        // chairVisit only says this visit used a chair, so EndVisit LOOKS: it reads the live sit state and decides from
        // that. (On 98a2dc9a this was `seated`, from the hold that a late sit left at 0, and it both decided the stand-up
        // and was printed as its outcome.)
        const bool chairVisit = g_live.mode == Mode::Chair || g_holdSeat != 0;
        if (g_live.active) Push("end", cw::json{ { "why", "the menu closed" } });
        EndVisit("the menu closed", chairVisit, true);
    }

    void OnLoad()
    {
        // A save taken mid-scene loads as a player sitting in a chair with full control: nothing of the scene is in
        // the save, so there is nothing to restore -- only to let go of. Never stand the player up here: the loaded
        // game's player is not the one who sat down.
        SetBusy(false);                      // a load drops everything, this flag before anything else
        g_stand = StandWatch{};              // a stand-up watch never crosses a load
        g_camRestore = cw::scenepkg::CamWatch{};
        const bool had = g_live.active || !g_live.input.empty() || g_live.cam.valid || g_holdSeat || g_holdCam.valid;
        g_visitCommits = 0;
        g_visitDoc.clear();
        if (!had) return;
        REX::INFO("scene: a game was loaded while a scene or a seated visit was live; dropping it (handlers, camera, view and effects restored, nothing carried in)");
        EndVisit("a game was loaded", false, false);
    }

    // The post-visit stand-up watch: a stand-up is reported as DONE only on a live read of 0 after the exit was asked,
    // and a sit still pending at the visit's end is stood up if it lands (the b5 player stayed in the chair).
    namespace
    {
        void WatchProbe(RE::PlayerCharacter* p, std::uint64_t now)
        {
            if (!g_probe.on || !p) return;
            if (auto* ui = RE::UI::GetSingleton(); ui && ui->GetMenuOpen(RE::BSFixedString("Console"))) {
                if (!g_probe.consoleSince) g_probe.consoleSince = now;
                return;
            }
            if (g_probe.consoleSince) { g_probe.paused += now - g_probe.consoleSince; g_probe.consoleSince = 0; }
            const std::uint64_t raw = now > g_probe.since ? now - g_probe.since : 0;
            const std::uint64_t t = raw > g_probe.paused ? raw - g_probe.paused : 0;
            const int st = SitState(p);
            const auto step = cw::scenepkg::Observe(g_probe.w, st, t);
            if (step.changed)
                REX::INFO("scene: sitwatch (probe) sit state {} ({}) -> {} ({}) at {} ms", step.from,
                          cw::scenepkg::SitStateName(step.from), st, cw::scenepkg::SitStateName(st), t);
            if (step.seatedNow) REX::INFO("scene: sitwatch (probe) SEATED at {} ms", t);
            if (t >= g_probe.ms) {
                REX::INFO("scene: sitwatch (probe) ended at {} ms: {}; last state {} ({}), console time excluded {} ms", t,
                          g_probe.w.seated ? ("seated at " + std::to_string(g_probe.w.seatedAt) + " ms") : std::string("NEVER seated"),
                          st, cw::scenepkg::SitStateName(st), g_probe.paused);
                g_probe = ProbeWatch{};
            }
        }

        void WatchCamRestore(std::uint64_t now)
        {
            if (!g_camRestore.asked) return;
            const int cs = CamStateNow();
            const auto c = cw::scenepkg::CamObserve(g_camRestore, cs, now);
            const std::uint64_t age = now - g_camRestore.askedAt;
            if (c.changed) REX::INFO("scene: restore camera {} -> {} at +{} ms (read now)", cw::scenepkg::CamStateName(c.from), cw::scenepkg::CamStateName(cs), age);
            if (c.seenNow) {
                REX::INFO("scene: camera OBSERVED firstperson {} ms / {} frame(s) after the SCN-9 restore request (read now)", age, g_camRestore.framesToSeen);
                g_camRestore = cw::scenepkg::CamWatch{};
            } else if (c.guardNow) {
                REX::WARN("scene: camera NOT back in firstperson {} ms after the SCN-9 restore request; it is {} read now (SCN-9)", age, cw::scenepkg::CamStateName(cs));
                g_camRestore = cw::scenepkg::CamWatch{};
            }
        }

        void WatchStand(RE::PlayerCharacter* p, std::uint64_t now)
        {
            if (!g_stand.seat || !p) return;
            const int live = SitState(p);
            const std::uint64_t age = now > g_stand.since ? now - g_stand.since : 0;
            using cw::scenepkg::StandEvent;
            switch (cw::scenepkg::WatchStand(g_stand.exitAsked, live, age)) {
            case StandEvent::ExitVerified:
                REX::INFO("scene: stand-up VERIFIED: live sit state 0 {} ms after the exit was asked", age);
                g_stand = StandWatch{};
                break;
            case StandEvent::ExitNotVerified:
                REX::WARN("scene: stand-up NOT verified: live sit state {} ({}) {} ms after the exit was asked (the never-hang guard); the player is still in the chair (SCN-9)",
                          live, cw::scenepkg::SitStateName(live), age);
                g_stand = StandWatch{};
                break;
            case StandEvent::PendingLanded:
                if (auto* seat = RE::TESForm::GetFormByID(g_stand.seat))
                    if (auto* sr = seat->As<RE::TESObjectREFR>()) {
                        const bool asked = sr->ActivateRef(p, nullptr, 1, false, false, false);
                        REX::WARN("scene: the pending sit landed {} ms after the visit ended; stand up through seat {:08X} -> {}",
                                  age, g_stand.seat, asked ? "accepted" : "refused");
                    }
                g_stand.exitAsked = true;
                g_stand.since = now;
                break;
            case StandEvent::PendingDropped:
                REX::INFO("scene: the pending sit ended without a sit (live sit state {} ({}) read {} ms after the visit ended); nothing to stand up from",
                          live, cw::scenepkg::SitStateName(live), age);
                g_stand = StandWatch{};
                break;
            default: break;
            }
        }
    }

    // ---------------------------------------------------------------- the beats
    void Tick(std::uint64_t /*pumpNow*/)
    {
        const std::uint64_t now = NowMs();   // see NowMs: one clock, not the pump's
        auto* p = RE::PlayerCharacter::GetSingleton();
        WatchStand(p, now);                  // runs with no scene active: it is about the visit that just ended
        WatchProbe(p, now);                  // the positive-control tracer (the sitwatch seam)
        WatchCamRestore(now);                // the SCN-9 camera restore, read back after the visit
        if (!g_live.active) return;
        if (!p) { EndRun("no player"); return; }

        // The console pauses the game: hold the clock while it is open, then carry on (never a half-scene).
        if (auto* ui = RE::UI::GetSingleton(); ui && ui->GetMenuOpen(RE::BSFixedString("Console"))) {
            if (!g_live.consoleSince) g_live.consoleSince = now;
            return;
        }
        if (g_live.consoleSince) { g_live.pausedMs += now - g_live.consoleSince; g_live.consoleSince = 0; }

        // Watchdog. Any of these and the scene ends cleanly; the install is already the player's.
        if (g_live.mode == Mode::Chair) {
            if (p->IsInCombat()) { Abort("combat started"); return; }
            if (g_h.playerHealth && g_live.health0 > 0.0f && g_h.playerHealth() < g_live.health0 - kHealthDrop) { Abort("the player was hit"); return; }
            if (g_h.blockingMenu) {
                if (const char* m = g_h.blockingMenu()) {
                    // escguard holds Esc and Tab while the menu is open; anything that opens anyway ends the scene.
                    Abort((std::string("a game menu opened (") + m + ")").c_str());
                    return;
                }
            }
        }

        std::uint64_t el = Elapsed(now);
        const std::uint64_t sceneMs = SceneMs(now);

        // The camera, READ every frame from the native request (CamWatch): when 3P is first seen, every change, and any
        // frame it leaves 3P after reaching it, which is a FIGHT (the open question: does a talk-menu camera switch it back?).
        if (g_live.camw.asked) {
            const int cs = CamStateNow();
            const auto c = cw::scenepkg::CamObserve(g_live.camw, cs, sceneMs);
            if (c.changed)
                REX::INFO("scene: camera {} -> {} at {} ms (read now)", cw::scenepkg::CamStateName(c.from), cw::scenepkg::CamStateName(cs), sceneMs);
            if (c.seenNow)
                REX::INFO("scene: camera OBSERVED thirdperson at {} ms, {} ms / {} frame(s) after the native request (read now: QCameraEquals)",
                          sceneMs, sceneMs - g_live.camw.askedAt, g_live.camw.framesToSeen);
            if (c.guardNow)
                REX::WARN("scene: camera NOT in thirdperson {} ms after the native request (the never-hang guard); it is {} read now",
                          sceneMs - g_live.camw.askedAt, cw::scenepkg::CamStateName(cs));
            if (c.lostNow)
                REX::WARN("scene: camera LEFT thirdperson at {} ms (now {} read now) after reaching it at {} ms: something switched it back (fight #{})",
                          sceneMs, cw::scenepkg::CamStateName(cs), g_live.camw.seenAt, g_live.camw.lost);
        }

        if (g_live.mode == Mode::Short || g_live.mode == Mode::Fade) {
            if (el >= kShortFlareMs && !g_live.revealDone) { g_live.revealDone = true; Push("reveal", cw::json{ { "ms", kShortEnd - kShortFlareMs } }); }
            if (el >= kShortEnd) { Push("end", cw::json::object()); EndRun("the short beat ended"); }
            return;
        }

        const Beats& t = g_live.t;

        // Beat 2, the dip: 0.45 s of black over moving a standing player to the chair's entry, then the sit.
        if (!g_live.dipDone && el >= t.dip) {
            g_live.dipDone = true;
            Push("dip", cw::json{ { "ms", kDipMs } });
            if (g_h.cue) g_h.cue("CWC_SND_ChairOn", "UIModsMenuEnter");   // the chair servo starts in the dark
            const auto it = g_cfg.docs.find(g_live.doc);
            auto* seat = g_live.seatRef ? RE::TESForm::GetFormByID(g_live.seatRef) : nullptr;
            auto* sr = seat ? seat->As<RE::TESObjectREFR>() : nullptr;
            if (sr && it != g_cfg.docs.end()) MoveToEntry(p, sr, it->second);
        }

        // Beat 3: fade up on the framed chair, and ask the engine to seat the player. The furniture's own enter
        // animation IS the "gets into the chair" beat -- we never pose the player ourselves.
        if (!g_live.upDone && el >= t.up) {
            g_live.upDone = true;
            g_live.shot = 1;
            const auto it = g_cfg.docs.find(g_live.doc);
            if (it != g_cfg.docs.end()) g_live.held = it->second.shotA;
            Push("up", cw::json{ { "ms", kFadeUpMs } });
            if (!g_live.seatedAtStart && !g_live.sitAsked) {
                g_live.sitAsked = true;
                g_live.sit.askedAt = sceneMs;
                g_live.sit.windowMs = kSitWaitMs;
                if (auto* seat = g_live.seatRef ? RE::TESForm::GetFormByID(g_live.seatRef) : nullptr)
                    if (auto* sr = seat->As<RE::TESObjectREFR>()) {
                        const bool asked = sr->ActivateRef(p, nullptr, 1, false, false, false);
                        REX::INFO("scene: seat {:08X}.ActivateRef(player) -> {} at {} ms (a request, not a sit; the sit is read below)",
                                  g_live.seatRef, asked ? "accepted" : "refused", sceneMs);
                    }
            }
        }

        // The sit watch (Fix A): EVERY frame from the ask to the end, never only inside a window. Each SIT_SLEEP_STATE
        // change is logged with its ms, so a capture can be read against what the engine did and when.
        if (g_live.sitAsked && !g_live.seatedAtStart) {
            const int st = SitState(p);
            const auto step = cw::scenepkg::Observe(g_live.sit, st, sceneMs);
            const std::uint64_t sinceAsk = sceneMs > g_live.sit.askedAt ? sceneMs - g_live.sit.askedAt : 0;
            if (step.changed)
                REX::INFO("scene: sit state {} ({}) -> {} ({}) at {} ms, {} ms after the ask", step.from,
                          cw::scenepkg::SitStateName(step.from), st, cw::scenepkg::SitStateName(st), sceneMs, sinceAsk);
            if (step.seatedNow) {
                g_live.seatedNow = true;
                if (!g_live.sit.late) REX::INFO("scene: the player is seated at {} ms ({} ms after the ask)", sceneMs, sinceAsk);
                else REX::WARN("scene: the player is seated LATE at {} ms ({} ms after the ask, past the {} ms wait): the scene had already played on standing",
                               sceneMs, sinceAsk, kSitWaitMs);
            }
            if (step.windowNow) {
                g_live.sitGaveUp = true;
                if (step.droppedNow)
                    REX::WARN("scene: the player was not seated within {} ms of the fade-up: the engine DROPPED the sit ({} -> 0 at {} ms after the ask); continuing from where they stand (SCN-2 will fail this capture), still watched to the end",
                              sinceAsk, step.from, sinceAsk);
                else
                    REX::WARN("scene: the player was not seated within {} ms of the fade-up (the never-hang guard); live sit state {} ({}); continuing from where they stand (SCN-2 will fail this capture), still watched to the end",
                              kSitWaitMs, st, cw::scenepkg::SitStateName(st));
            }
        }

        // Fix B: the beats HOLD just before the work beat until the sit is read (or the wait above runs out). Her
        // step-in and her line are before it and run on. Every ms held moves every later beat, the end included.
        if (cw::scenepkg::GateHolds(g_live.sitAsked && !g_live.seatedAtStart, g_live.sit, g_live.skipped) && el + 1 >= t.work) {
            const std::uint64_t at = t.work > 0 ? t.work - 1 : 0;
            if (el > at) { g_live.holdMs += el - at; el = at; }
            if (!g_live.holding) {
                g_live.holding = true;
                REX::INFO("scene: beats held at {} ms for the sit EVENT (asked at {} ms; live sit state {}); her line runs on",
                          el, g_live.sit.askedAt, g_live.sit.last);
            }
        } else if (g_live.holding) {
            g_live.holding = false;
            // holdMs is MEASURED (the beat-clock time clamped, summed frame by frame). The lockout here is a PROJECTION
            // (end beat + held), printed as one; the MEASURED lockout is the "finished at ... lockout <t> ms" line.
            std::string why;
            if (g_live.sit.seated) why = "first sitting read at " + std::to_string(g_live.sit.seatedAt) + " ms";
            else if (g_live.skipped) why = "skipped";
            else if (g_live.sit.dropped) why = "the engine dropped the sit at " + std::to_string(g_live.sit.endedAt) + " ms";
            else why = "the never-hang guard at " + std::to_string(g_live.sit.endedAt) + " ms";
            REX::INFO("scene: beats resume after {} ms held (measured; {}); CAPTURE lockout PROJECTED {} ms (end beat {} + held), the measured lockout is the finished line's",
                      g_live.holdMs, why, t.end + g_live.holdMs, t.end);
        }

        // Beat 4: she is on her mark (or wherever she got to: the line never waits more than kHerWaitMs), turned to
        // the player, one gesture, and her line starts. The line is never gated by a beat and is never cut off.
        if (!g_live.stepDone && el >= t.stepIn) {
            g_live.stepDone = true;
            // v1: she does not walk (no CW_ClinicSceneActive yet), so the scene turns her to the player here and she
            // works from where she stands. The turn takes 0.45 s and is finished long before her line at beat 4, so no
            // beat time changes. Without this a chair-opened visit has her facing away through the whole scene.
            if (g_h.turnTo && g_live.docRef) g_h.turnTo(g_live.docRef);
            Push("step", cw::json::object());
        }
        if (!g_live.lineDone && el >= t.line) {
            g_live.lineDone = true;
            if (g_h.gesture) g_h.gesture();
            const float voiceMs = (g_h.say && !g_live.key.empty()) ? g_h.say(g_live.doc, g_live.key) : 0.0f;
            // voiceMs tells the view how long to hold the subtitle: past the scene's own end when the clip is longer.
            Push("line", cw::json{ { "text", g_live.text }, { "voiceMs", voiceMs },
                                   { "waited", el >= t.stepIn + kHerWaitMs } });
            // The CAPTURE WINDOW, printed so a cycle does not have to compute it (Quality Director, 2026-11-10).
            // Her line starts HERE and runs past the end of the scene: an install clip of 8.71 s from this beat is
            // still audible ~2.4 s after control comes back. A burst that stops at the lockout end truncates exactly
            // the part where lip sync drifts and the level dips, and SCN-6 would then be graded on a cut-off line.
            // Two different numbers, never to be confused: LOCKOUT is how long the player cannot act; AUDIBLE is how
            // long she can still be heard. The 9 s cap is a lockout budget, not a scene length.
            if (voiceMs > 0.0f)
                REX::INFO("scene: CAPTURE lockout {} ms, audible {} ms (line at {} + clip {:.0f}); run the burst to about {} ms",
                          t.end, (std::uint64_t)(t.line + voiceMs), t.line, voiceMs, (std::uint64_t)(t.line + voiceMs) + 500);
            else
                REX::INFO("scene: CAPTURE lockout {} ms, no clip for this line (text only): run the burst to about {} ms",
                          t.end, t.end + 500);
        }

        if (!g_live.workDone && el >= t.work) { g_live.workDone = true; Push("work", cw::json::object()); }
        if (!g_live.cue1 && el >= t.cue1) { g_live.cue1 = true; if (g_h.cue) g_h.cue("CWC_SND_Surgery1", "UIModsBuild"); }
        if (!g_live.cue2 && el >= t.cue2) { g_live.cue2 = true; if (g_h.cue) g_h.cue("CWC_SND_Surgery2", "UIModsPartSelect"); }
        if (!g_live.cue3 && el >= t.cue3) { g_live.cue3 = true; if (g_h.cue) g_h.cue("CWC_SND_Surgery3", "UIModsBuild"); }

        // Beat 6: the flare. The view's flat layer is the baseline; the engine DOF is an A/B behind [Scene] iFlare
        // and stays inside vanilla ranges (SCN-12: the Sandevistan loop was one value 75x past vanilla's maximum).
        if (!g_live.flareDone && el >= t.flare) {
            g_live.flareDone = true;
            Push("flare", cw::json{ { "ms", kFlareMs } });
            if (g_h.cue) g_h.cue("CWC_SND_InstallPart", g_live.kind == "remove" ? "UIModsBreakDown" : "UIModsBuild");
            if (g_cfg.flare > 0) {
                if (auto* f = RE::TESForm::GetFormByID(0x00024590))   // VATSScanImod: a vanilla record, vanilla values
                    if (auto* m = f->As<RE::TESImageSpaceModifier>()) RE::ImageSpaceModifierInstanceForm::Trigger(m, 1.0f, nullptr);
            }
        }

        // Beat 7: the cut to the second angle, closer, on the side of the new part, and the lower-third.
        if (!g_live.revealDone && el >= t.reveal) {
            g_live.revealDone = true;
            g_live.shot = 2;
            const auto it = g_cfg.docs.find(g_live.doc);
            if (it != g_cfg.docs.end()) g_live.held = it->second.shotB;
            if (g_cfg.flare > 0) {
                if (auto* f = RE::TESForm::GetFormByID(0x00024590))
                    if (auto* m = f->As<RE::TESImageSpaceModifier>()) RE::ImageSpaceModifierInstanceForm::Stop(m);
            }
            Push("reveal", cw::json{ { "ms", t.back - t.reveal } });
        }

        // Beat 8: bars open, the menu comes back, the player is STILL IN THE CHAIR behind it. She may still be
        // finishing her line, and nothing here stops it.
        if (!g_live.backDone && el >= t.back) {
            g_live.backDone = true;
            Push("back", cw::json{ { "ms", t.end - t.back } });
            ReleaseInput();                       // the menu is usable again within 0.6 s of the reveal (SCN-9)
            if (g_h.menuPaused && g_h.menuPaused() && g_h.refocus) g_h.refocus(true);
        }

        if (el >= t.end) {
            Push("end", cw::json::object());
            // Every field below is read NOW, at the end: "seated" means a sitting read at some point in the scene
            // (late included), and the live state is this frame's.
            const int stEnd = SitState(p);
            if (g_live.camw.asked)
                REX::INFO("scene: camera at the end is {} read now; thirdperson {} ; left it {} time(s)",
                          cw::scenepkg::CamStateName(CamStateNow()),
                          g_live.camw.seen ? ("first observed at " + std::to_string(g_live.camw.seenAt) + " ms") : std::string("NEVER observed"),
                          g_live.camw.lost);
            std::string seatWord;
            if (g_live.seatedAtStart) seatWord = "seated at start (read at the commit)";
            else if (g_live.sit.seated)
                seatWord = std::string(g_live.sit.late ? "seated LATE" : "seated") + " (first sitting read at " + std::to_string(g_live.sit.seatedAt) + " ms)";
            else seatWord = "NOT seated (no sitting read from the ask to now)";
            REX::INFO("scene: finished at {} ms, lockout {} ms ({} ms held for the sit), player {}; live sit state {} ({}) read now; the chair is held until the menu closes",
                      el, sceneMs, g_live.holdMs, seatWord, stEnd, cw::scenepkg::SitStateName(stEnd));
            // The player stays in the chair for the rest of the visit. OnMenuClosed() stands them up and puts the
            // camera and view back; the hold survives the next commit's ResetLive().
            // The seat ref is CARRIED to the menu close whenever this scene sent the player to it, whatever any flag
            // says: on 98a2dc9a the frozen seatedNow decided this (g_holdSeat = seatedNow ? seatRef : 0) and then
            // g_live.seatRef was cleared below, so after b5's late sit BOTH refs were 0 at the close and the live
            // Sitting() check in EndVisit was never reached. Whether to stand the player up is decided THERE, by a
            // live read at the close, never here.
            g_holdSeat = (g_live.seatedAtStart || g_live.sitAsked) ? g_live.seatRef : 0;
            g_holdDoc = g_live.docRef;
            g_holdCam = g_live.cam;
            ReleaseInput();                      // belt and braces: the back beat already did it
            SetBusy(false);                      // the scene is over; the seated hold is not exempt (29 u apart)
            g_live.active = false;
            g_live.mode = Mode::None;
            g_live.cam = CamSave{};              // the hold owns it now, so Teardown cannot restore it twice
            g_live.seatRef = 0;
            g_live.docRef = 0;
            if (g_live.globWritten) {            // she goes back to her own schedule now, not when the menu closes
                WriteGlobal(0.0f);
                if (g_holdDoc)
                    if (auto* d = RE::TESForm::GetFormByID(g_holdDoc))
                        if (auto* r = d->As<RE::TESObjectREFR>()) Evaluate(r);
            }
            g_live.globWritten = false;
            return;
        }

        // Held every frame: the shot, with the slow push-in that runs until the cut (the cam3p seam's lesson -- a
        // single write is undone by the next camera update).
        if (g_live.shot == 1) {
            const float span = (float)(t.flare > t.up ? t.flare - t.up : 1);
            const float k = std::clamp((float)(el > t.up ? el - t.up : 0) / span, 0.0f, 1.0f);
            Shot sh = g_live.held;
            sh.zoom = g_live.pushFrom + (g_live.pushTo - g_live.pushFrom) * k;
            ApplyShot(sh);
        } else if (g_live.shot == 2) {
            ApplyShot(g_live.held);
        }
    }

    // ---------------------------------------------------------------- status and the tuning seams
    std::string StatusLine()
    {
        char buf[1400];
        auto* p = RE::PlayerCharacter::GetSingleton();
        const auto el = g_live.active ? Elapsed(NowMs()) : 0;
        const int live = SitState(p);
        std::snprintf(buf, sizeof(buf),
                      "mode %s active %d doc '%s' %s '%s' at %llu/%llu ms, visit commits %d, seat %08X (seated at start %d, seen seated %d at %llu ms late %d, wait ended %d at %llu ms, held %llu ms%s; LIVE sit state %d %s read now), camera %s read now (3P observed %d at %llu ms, left %d), doc %08X, shot %d (yaw %.0f pitch %.0f zoom %.2f), input held %zu, global %d, talk-guard busy %d, skipped %d, player sitting %d, chairScene %d camPitch %d flare %d, why '%s'",
                      ModeName(g_live.mode), g_live.active ? 1 : 0, g_live.doc.c_str(), g_live.kind.c_str(), g_live.name.c_str(),
                      (unsigned long long)el, (unsigned long long)g_live.t.end, g_visitCommits, g_live.seatRef,
                      g_live.seatedAtStart ? 1 : 0, g_live.sit.seated ? 1 : 0, (unsigned long long)g_live.sit.seatedAt,
                      g_live.sit.late ? 1 : 0, g_live.sit.windowPassed ? 1 : 0, (unsigned long long)g_live.sit.endedAt,
                      (unsigned long long)g_live.holdMs,
                      g_live.holding ? ", HOLDING" : "", live, cw::scenepkg::SitStateName(live),
                      cw::scenepkg::CamStateName(CamStateNow()), g_live.camw.seen ? 1 : 0, (unsigned long long)g_live.camw.seenAt, g_live.camw.lost,
                      g_live.docRef,
                      g_live.shot, g_live.held.yaw, g_live.held.pitch, g_live.held.zoom, g_live.input.size(),
                      g_live.globWritten ? 1 : 0, g_busy ? 1 : 0, g_live.skipped ? 1 : 0, Sitting(p) ? 1 : 0,
                      g_cfg.chairScene ? 1 : 0, g_cfg.camPitch ? 1 : 0, g_cfg.flare, g_live.why.c_str());
        return buf;
    }

    std::string NudgeSeat(const std::string& doc, float dx, float dy, float dz, float drz)
    {
        const auto it = g_cfg.docs.find(doc);
        if (it == g_cfg.docs.end() || !it->second.seatId) return "no seat configured for '" + doc + "'";
        auto* seat = RefOf(it->second.seatPlugin, it->second.seatId);
        if (!seat) return "the seat record is not loaded";
        const auto pos = seat->GetPosition();
        const float rz = seat->data.angle.z + drz;
        const RE::NiPoint3 to(pos.x + dx, pos.y + dy, pos.z + dz);
        seat->SetLocationOnReference(to);
        seat->SetAngleOnReference(RE::NiPoint3(0.0f, 0.0f, rz));
        char buf[256];
        // The line CLINIC bakes into patientSeat.pos / rotZ once a still proves the pose.
        std::snprintf(buf, sizeof(buf), "seat %08X now (%.1f, %.1f, %.1f) rotZ %.4f  ->  patientSeat.pos [%.1f, %.1f, %.1f], rotZ %.4f",
                      seat->GetFormID(), to.x, to.y, to.z, rz, to.x, to.y, to.z, rz);
        REX::INFO("scene: {}", buf);
        return buf;
    }

    std::string WatchSit(std::uint64_t ms)
    {
        if (ms < 500) ms = 500;
        if (ms > 30000) ms = 30000;
        g_probe = ProbeWatch{};
        g_probe.on = true;
        g_probe.since = NowMs();
        g_probe.ms = ms;
        g_probe.w.windowMs = ms;
        auto* p = RE::PlayerCharacter::GetSingleton();
        const int st = SitState(p);
        char buf[200];
        std::snprintf(buf, sizeof(buf), "sitwatch (probe) armed for %llu ms; live sit state %d %s now (the baseline)",
                      (unsigned long long)ms, st, cw::scenepkg::SitStateName(st));
        REX::INFO("scene: {}", buf);
        return buf;
    }

    std::string NudgeCam(int shot, float yawDeg, float pitchDeg, float zoom)
    {
        if (shot != 1 && shot != 2) return "shot must be 1 (beat 3) or 2 (the reveal)";
        g_live.shot = shot;
        g_live.held = Shot{ yawDeg, pitchDeg, zoom, true };
        g_live.pushFrom = zoom;
        g_live.pushTo = zoom;          // a nudge holds still, so a still can be judged
        ApplyShot(g_live.held);
        char buf[200];
        // The line that goes into [Scene] sShotA_<doc> / sShotB_<doc> once a burst passes SCN-3 and SCN-4.
        std::snprintf(buf, sizeof(buf), "shot %d held at yaw %.0f pitch %.0f zoom %.2f  ->  sShot%c_<doc>=%.0f,%.0f,%.2f",
                      shot, yawDeg, pitchDeg, zoom, shot == 1 ? 'A' : 'B', yawDeg, pitchDeg, zoom);
        REX::INFO("scene: {}", buf);
        return buf;
    }
}  // namespace scene
