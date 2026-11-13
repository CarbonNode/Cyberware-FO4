// main.cpp - Cyberware.dll: F4SE plugin (commonlibf4, Dear-Modding-FO4 fork) that hosts the cw:: core in Fallout 4.
//
// What lives here, and only here (everything else is in ../core, which has no game dependency):
//   GameEnv     - cw::Env on the real game: Caps, player level, vanilla perk ranks, the perk reconcile, loot chips.
//   FormMap     - EditorID -> local FormID of Cyberware.esp records, read from the generator's formmap.json
//                 (EditorIDs are not kept at runtime in FO4, so the generator writes this file next to the ESP).
//   PrismaUI    - one view, Data/PrismaUI_F4/views/Cyberware/index.html; window.cwRequest -> Api::Handle ->
//                 window.cwOnResponse; pushes window.cwOnEvent (docs/UI-API.md).
//   Activation  - TESActivateEvent: activating a ripperdoc chair (a placed chair ref, or a ref of an ACTI, in formmap "ripperdocChairs")
//                 opens the menu for that ripperdoc. Optional debug hotkey (Cyberware.ini iOpenKey, VK code).
//   Cosave      - the whole cw::State as one JSON record ('CYBR'/'STAT'), plus the external perks we added ('EXTP').
//   Proof       - Cyberware.log: CYBERWARE_BUILDID, catalog + formmap summary, resolution misses, every reconcile.
#include "PCH.h"
#include <charconv>

#include "PrismaUI_F4_API.h"
#include "cw_core.h"
#include "cw_talkprompt.h" // VARGA'S PROMPT: the DLL's own two-choice talk prompt (Builder 3 2026-11-13, docs/VARGA-TALK.md)
#include "cw_armhint.h"    // ARMS: WHEN the arm-use hint is shown (ask F) - raised at install, shown when readable
#include "cw_stuck.h"      // ARMS: the move-stuck watchdog + recovery policy (beta update 1, docs/integration/BETA-UPDATE1-GOOD-20261112.md)
#include "tint.h"
#include "cw_smart_game.h"
#include "cw_escguard.h"   // MENU POLISH: Esc / Tab stay in the ripperdoc menu
#include "cw_clinic.h"     // CLINIC: Varga's off-screen schedule mover ([Clinic] bSchedule)
#include "cw_scene.h"      // INSTALL SCENE: the ripperdoc chair sequence (docs/INSTALL-SCENE.md, [Scene] bChairScene)
#include "cw_pkgdiag.h"    // TEST HARNESS: {"cmd":"packages"} seam, an actor's AI packages read from the forms
#include "cw_confirm.h"    // TEST HARNESS: seam actions wait for the graph event that proves they played
#include "cw_hudlayout.h"  // MENU POLISH: the player's HUD layout (drag / resize, views/CyberwareHUD/layout.js)

namespace cwqa {   // plugin/cw_qa_audit.cpp: the in-game 1:1 audit ({"cmd":"qaaudit"}, BETA QA 2026-11-08)
void Start(cw::Engine* eng, cw::Env* env);
void Tick();
}  // namespace cwqa

namespace cwcomp {   // plugin/cw_components_plugin.cpp: FO4-crafting component counting/scrapping (BETA QA)
int Count(RE::PlayerCharacter* p, RE::TESForm* comp);
bool Consume(RE::PlayerCharacter* p, RE::TESForm* (*resolve)(const std::string&), const std::vector<std::pair<std::string, int>>& need);
}  // namespace cwcomp

namespace cwsnd { void UiCue(const std::string& sound, const std::string& fallback); }   // main.cpp bottom: the menu's uiSound op
namespace cwset {  // plugin/cw_settings_plugin.cpp: player Settings glue (BETA QA, 2026-11-08)
void Init(cw::Engine* e, std::function<void(const cw::Settings&)> apply, const std::string& settingsDir);
void LoadDefaults();
void Apply();
void AfterOp(const std::string& req, const std::string& res);
bool MaybeUiSound(const std::string& req);
}  // namespace cwset
#include "cw_voice.h"      // RIPPERDOC VOICES: the doctor speaks the line (docs/RIPPERDOC-VOICE.md)
#include "mantis_fold3p.inl"   // ARMS LOOK: 3P Mantis blade fold keys (arms/tools/export_fold3p.py)
#include "mantis_stow3p.inl"   // ARMS LOOK: 3P Mantis blade stowed state (arms/tools/fold3p_stow.py)

#ifdef ERROR
#    undef ERROR  // wingdi.h (pulled in by PrismaUI_F4_API.h) defines ERROR 0, which breaks REX::ERROR
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <random>
#include <set>

static constexpr const char* kBuildId = __DATE__ " " __TIME__;
static std::optional<REL::Version> g_queryRuntime;   // set by F4SEPlugin_Query = the OG loader (F4SE 0.6.x) loaded us
namespace cwsys { std::uintptr_t FollowJumps(std::uintptr_t a); std::string ModuleOf(std::uintptr_t a); }   // plugin/modname.cpp
static constexpr const char* kVersion = "0.1.0";

#include <cstring>
#if __has_include("tint.h")
#include "tint.h"
#define CW_HAS_TINT 1
#else
#define CW_HAS_TINT 0
#endif

namespace fs = std::filesystem;
using cw::json;

namespace {

// ---------------------------------------------------------------- settings
struct Settings {
    bool debug = false;
    std::uint32_t openKey = 0;          // [General] iOpenKey: the SHOP hotkey, Cyberware.Test.ini only (the test harness); 0 = off
    std::uint32_t setupKey = 0x77;      // [General] iSetupKey: the read-only "your setup" view, anywhere (F8); 0 = off
    std::uint32_t keyOS = 0, keyCamo = 0, keyScan = 0, keyHeal = 0, keyHack = 0, keyHackNext = 0, keyPacify = 0, keySkippy = 0;   // native ability keys (VK codes); 0 = off
    std::string openKeyDoc;             // ripperdoc the hotkey opens (default: first in catalog)
    bool pauseGame = true;
    bool stateLock = false;  // [General] bStateLock: tasks vs load/save (StateLock below). OFF for players until a cycle proves it
    bool escGuard = true;    // [Menu] bEscGuard: on; proven in game cw-int-2 T9-T11 (cw_escguard.cpp)
    bool talkIdles = false;      // [Menu] bTalkIdles: in the unpaused talk mode the doctor gestures on each reply (vanilla Shrug/PointForward through PlayIdle). OFF until a capture passes
    bool talkUnpaused = false;   // [Menu] bTalkUnpaused: the ripperdoc conversation leaves the game running. OFF until a cycle proves combat near the NPC and being attacked mid-talk
    int talkBranchFlags = -1;    // [Menu] iTalkBranchFlags: override CW_RipperdocBlockBranch's DLBR flags at run time. -1 = leave the record's own value (1, Top-Level). 3 = Top-Level|Blocking. docs/VARGA-TALK.md
    bool finishers = false;             // [Arms] bFinishers: CP finishers (dynamic idles). Passed in game in cycles 1 + 2; Cyberware.ini ships 1 for play (the lead, 2026-11-09); 0 here only when no ini says otherwise
    bool kiroshiEyes = false;           // [Customize] bKiroshiEyeGlow: the player's eyes take the Kiroshi colour. Off by default; ON only in Cyberware.Test.ini until proven
    bool mantisFold3P = false;          // [Arms] bMantisFold3P: in 3P the plugin writes the Mantis blade fold (AnimObjectR1/L1) through the draw and the holster, because the 3P graph never applies those clip tracks and the blades popped to full length (QD block 20, P12 node read). OFF until QD passes it on a capture; the test ini sets 1
    bool mantisStow3P = false;          // [Arms] bMantisStow3P (needs bMantisFold3P + bVisualKeep): in 3P the blades grow out of the forearm housing on the draw and draw back into it on the holster, and the shapes are shown only once our fold writes hold (AAA row 5: P15 draw = open blade flash, holster = cull 220 ms into the fold). OFF until QD passes it; the test ini sets 1
    int foldStowAtMs = 900, foldStowInMs = 300, foldStowOutMs = 200;   // [Arms] iFoldStowAtMs / iFoldStowInMs / iFoldStowOutMs: holster draws the blade in from At over In ms; the draw lets it out over Out ms from the moment it is shown
    bool keepHousing = false;           // [Arms] bKeepHousing: with bVisualKeep, the keep's sheathe / stow cull spares the Mantis housing, so a stowed housing stays on the forearm (QD block 26 s11, AAA row 5: "no stowed state on screen")
    bool visualReattach = false;        // [Arms] bVisualReattach: an installed visual (PLS launcher, Gorilla knuckles) whose 3D left the biped while installed is re-equipped (QD block 26 s8: the launcher gone after the first throw)
    bool plsHidePipboy3P = false;       // [Arms] bPlsHidePipboy3P: while the PLS is installed, the 3P Pip-Boy's shapes are culled so the launcher reads as the forearm (the lead, 2026-11-09, QD block 23). Test build only; the play default is the author's call (it changes how his character looks in 3P)
    bool plsChargeGlow = false;         // [Arms] bPlsChargeGlow: the launcher's glow lines build through the throw's wind-up and flash on the shot (the lead, 2026-11-09, block 18). OFF until QD passes it on a capture; the test ini sets 1
    // [Arms] sPlsRoundsShipped: WHICH launcher round mods this build offers at all, comma separated. Empty = NONE, and
    // empty is the default, so a missing key fails CLOSED (the lead, 2026-11-10: a round ships only if every material it swaps
    // is graded, and an ungraded round ships OFF). Accepts the catalog suffix or the element name. It gates AVAILABILITY:
    // an unlisted round's ability and mod are dropped from the catalog before the engine sees it, so cw::PlsRounds never
    // enumerates it and it is never offered, bought or installed. incendiary wears the THERMAL materials, so neither of that
    // pair ships without the other. The test ini lists all six.
    std::string plsRoundsShipped;
    bool plsRepick = false;             // [Arms] bPlsRepick: re-pick the weapon graph when the PLS keyword changes, so the launcher clips play at once. OFF until a cycle proves it
    bool leapRepick = false;            // [Arms] bLeapRepick: EVERY weapon-graph re-pick (the Mantis leap and the Gorilla ground slam). OFF for players (the lead, 2026-11-09): the re-pick itself was cleared (cw-int-3, the freeze was the save's SpeedMult), but the leap and slam stay out of play until their real versions pass the Quality Director on a capture. The test ini sets 1; the seam {"cmd":"leaprepick","on":0|1} flips it
    bool knockback = true;              // [Arms] bKnockback: Gorilla power-punch / slam ragdoll knockback (combat feel)
    int leapHangMs = 0;                 // [Arms] iLeapHangMs: with bLeapLaunch, 0 = the hang grows with the travel (250-600 ms: landing 0.87-1.22 s after weaponSwing); 250-600 = one hang for every distance, so the landing is always 0.62 s + this after weaponSwing and a clip can be authored against it (MANTIS MOTION, AAA row 6)
    int secondHeartImod = 0x00064C27;   // [Abilities] iSecondHeartImod: the Fallout4.esm imagespace modifier flashed when Second Heart saves the player (AAA-GAPS row 25). Default ExplosionFlashLarge (0.5 s); 0 = no flash. The same value in both ini files
    bool hackEffects = false;           // [Quickhacks] bHackEffects: Reboot Optics blinds its target (it stops fighting and detects nothing) and Cyberware Malfunction staggers it, as their item text says (READY-abilities-aaa-1 D4). 0 until a cycle proves both; the test ini sets 1
    bool leapLaunch = false;            // [Arms] bLeapLaunch: the Mantis leap is a real ballistic launch through the character controller (arc ~60-140 u, ~1 s) with a landing strike, instead of the SetPosition ground slide. 0 until a cycle proves it; the test ini sets 1
    bool clearEquipLocks = false;       // [Arms] bClearEquipLocks: on load, clear the equip lock from the player's weapons, once per save (saves made before force-equips were removed). OFF until a cycle proves it
    bool visualKeep = false;            // [Arms] bVisualKeep: the arm visual ARMO stays worn while its arm weapon is equipped and its shapes are culled when sheathed, so the clip's unfold / retract is seen (anim-3 draw + holster). 0 until a cycle proves it; the test ini sets 1
    int sheathHideMs = 220;             // [Arms] iSheathHideMs: with bVisualKeep, how long after weaponSheathe the shapes stay visible (the retract finishing on screen)
    int sheathHide1PMaxMs = 0;          // [Arms] iSheathHide1PMaxMs: with bVisualKeep, first person, Mantis: the sheathe hide also waits for the blade bones to show the
                                        // retract done (90 %), at most this long after weaponSheathe. 0 = off (the hide at iSheathHideMs only; P15b T3b: full blades cut)
    bool slamNoRepick = false;          // [Arms] bSlamNoRepick: with bLeapRepick=0, the Gorilla ground slam still fires (shockwave + knockback) on the vanilla jump attack, with no graph re-pick and no slam clip. 0 until a cycle proves it; the test ini sets 1
    bool stuckUndrive = false;          // [Arms] bStuckUndrive: on a move/sprint STUCK with the locomotion graph left animation-driven, clear those flags (cw-int-2 T4 diagnosis). Test ini only
    bool hitFromDoHitMe = false;        // [Arms] bHitFromDoHitMe: drive the player hit pipeline (CueHit, finishers, knockback, cleave, weak points, poison, Microgenerator, and the player-hurt family) from the DoHitMe hook, one call per applied hit, instead of TESHitEvent, which FO4 sends for only a few melee hits (P13c: 1 event for 13 DoHitMe player hits). Installs the DoHitMe hook like bDismember. DEFAULT 0 until the stability gate passes; the test ini sets 1
    // ---- FINISHERS 2026-11-10 (docs/FINISHER-ROWS.md). The old hold wrote HitData::totalDamage and trusted the engine to
    // scale it down; p15b-1 proved the engine does not read that field for the Health it applies (the SAME written 39.8 on
    // one 200 hp raider gave applied 0.0 once and 3222.1 once) and that writing it skews the engine's own arithmetic.
    bool finisherFloor = false;         // [Arms] bFinisherFloor: the hold writes NO damage field. Instead the victim's Health is lifted for the duration of the ONE engine call (a scope guard undoes it in the same call, so no save, cell change or interruption can ever see it) and the victim is then put back on the health it had when the finisher was decided, and held there for the set piece. 0 = the old totalDamage hold; the test ini sets 1
    int finisherSlideMs = 0;            // [Arms] iFinisherSlideMs: the victim is slid into the finisher's placement over this many ms instead of teleported there in one frame (QD FN1: frame 42 to 43, ~270 px and ~90 degrees in 37 ms). 0 = today's single step; the test ini sets 160
    bool finisherPin = false;           // [Arms] bFinisherPin: the victim is held at its placement for the set piece, so it cannot drift out of the blade (QD FN1: pushed 75 u to 285 u during one finisher). 0 until QD passes it; the test ini sets 1
    bool finisherTrace = false;         // [Arms] bFinisherTrace: one line per GAME frame of the victim's x, y, yaw and its distance from the choreographed offset, for the whole set piece. The QUALITY DIRECTOR requires it because a 28 fps capture can miss a one-frame jump, so the frames cannot prove FIN-1 or FIN-5 on their own. Diagnostic, test ini only
    bool hitFieldLog = false;           // [Arms] bHitFieldLog (needs bDebug): log every HitData damage field before and after the engine call, so the limb-1 5x factor can be attributed (row E1; the lead, 2026-11-10 condition 2). Diagnostic only, test ini sets 1
    bool dismember = false;             // [Arms] bDismember: Mantis/Monowire clean dismember. DEFAULT 0 until the 50+ hit stability gate passes (the DoHitMe hook sits on the engine hit path); 0 = the hook is NOT installed at all. The test profile sets 1 in Cyberware.Test.ini
    std::string catalogPath = "Data\\F4SE\\Plugins\\Cyberware\\cyberware.json";
    std::string formmapPath = "Data\\F4SE\\Plugins\\Cyberware\\formmap.json";
    std::string viewPath = "Cyberware/index.html";
    bool sandyEchoes = false;                              // [Sandevistan] bAfterimages: real pose clones behind moving actors (risky default: off until a cycle passes)
    bool hud = true;                                       // [HUD] bEnabled: the CyberwareHUD overlay (HUD stream)
    std::string hudView = "CyberwareHUD/index.html";      // [HUD] sView
    bool hudMessages = false;                              // [HUD] bMessages: 1 = the HUD text lines too, even while the view is up
    std::uint32_t hudEditKey = 0x71;                       // [HUD] iEditKey: VK code that opens the HUD layout editor (F2, the SAME key on both builds); 0 = off (the menu's Settings / L still open it)
    std::string hudLayoutDir;                              // [HUD] sLayoutDir: where HudLayout.json lives ("" = Documents\My Games\Fallout4\F4SE\Cyberware)
                                                           // (they are the automatic fallback when the view file is missing)
    bool stageBAnims = false;                              // [Smart] bStageBAnims: keyword that selects the Yukimura's CP 1P clips (ARMORY-STAGEB.md)
    bool smartLock = false;                                // [Smart] bSmartLock: ARMORY smart-gun lock + steering (cw_smart_game.cpp). OFF by default; ON only in Cyberware.Test.ini until a cycle proves it
    bool smartDiag = false;                                // [Smart] bDiag: smart-lock diagnostics once a second while a smart gun is drawn (test profile)
    int smartSteer = 2;                                    // [Smart] iSteerMode: 0 lock + log only, 1 + the round's desiredTarget, 2 + velocity steering
    bool armorySellUnready = false;     // [Armory] bSellUnready: also sell gear whose CP mesh is not staged (stand-in model). Test profile only
    std::string armoryFormmapPath = "Data\\F4SE\\Plugins\\Cyberware\\armory-formmap.json";   // [Paths] sArmoryFormMap (ARMORY espgen output)
    bool voice = false;                                    // [Voice] bEnabled: the ripperdoc speaks her lines (Cyberware - Voices.esp). OFF for players; ON only in Cyberware.Test.ini until proven
    bool voiceProbe = false;                               // [Voice] bProbe: trace the voice files the engine opens after each Say. Test ini only
    std::string voicesPath = "Data\\F4SE\\Plugins\\Cyberware\\voices.json";   // [Voice] sVoices
    std::string cmdFile;                                   // [Debug] sCmdFile: test seam (bDebug=1 only), see testcmd below
    int keyLogMax = 40;                                    // [Debug] iKeyLogMax: how many "input: key" lines a boot logs (bDebug=1 only). 40 as shipped; Cyberware.Test.ini raises it so every capture burst has its input line
    std::string settingsDir;                               // [Settings] sSettingsDir: where CyberwareSettings.ini persists.
                                                           // Empty = Documents\My Games\Fallout4\F4SE\Cyberware. The TEST
                                                           // build points it at a test folder so a test run's settings can never
                                                           // seed the play build's new games (the lead, cw-int-2).
    bool sceneChair = false;            // [Scene] bChairScene: the ripperdoc CHAIR scene (docs/INSTALL-SCENE.md). Risky default OFF in code and Cyberware.ini (it seats the player and takes the camera); the test ini sets 1. Off = the 2D fallback beat, world visible
    bool sceneCamPitch = false;         // [Scene] bCamPitch: ThirdPersonState freeRotation.y as camera PITCH is unproven (only .x as yaw is, by the cam3p seam). OFF until a capture proves it; the test ini sets 1
    int sceneFlare = 0;                 // [Scene] iFlare: 0 = the view's flat-fill flare only (the baseline); 1 = also trigger the vanilla imagespace A/B at beat 6, inside vanilla ranges (SCN-12)
    bool sndCues = false;               // [Sound] bCues: the wired cue layer (sound/WIRING-SPEC.md). 0 in Cyberware.ini: vanilla sounds stay exactly as today
    double sndMasterDb = 0.0;           // [Sound] fMasterDb: trim on Cyberware cues, in dB, 0 or negative only (handle path)
    bool snd3d = false;                 // [Sound] bSnd3D: positioned cues through BSAudioManager (cw_sound.inl). OFF until a cycle proves the usage flags; the test ini sets 1; 2D fallback always sounds
} g_ini;

void ReadIni()
{
    // Two layers (VISUAL POLISH 2026-11-08, the lead's config split). Cyberware.ini in the Cyberware mod = the player's
    // defaults (debug off, no test seam, keys bound). Cyberware.Test.ini, shipped ONLY by the MO2 mod "Cyberware - Test
    // Config" that only the test profile ticks, overrides any key it names (bDebug, sCmdFile, ...). A key the
    // overlay does not name keeps the main ini's value; with no overlay file the main ini is all there is.
    const char* path = "Data\\F4SE\\Plugins\\Cyberware.ini";
    const char* over = "Data\\F4SE\\Plugins\\Cyberware.Test.ini";
    std::error_code ec;
    const bool hasOver = fs::exists(over, ec);
    auto Int = [&](const char* sec, const char* key, int def) {
        const int v = (int)GetPrivateProfileIntA(sec, key, def, path);
        return hasOver ? (int)GetPrivateProfileIntA(sec, key, v, over) : v;
    };
    char buf[512];
    auto Str = [&](const char* sec, const char* key, const std::string& def) {
        GetPrivateProfileStringA(sec, key, def.c_str(), buf, sizeof(buf), path);
        std::string v = buf;
        if (hasOver) { GetPrivateProfileStringA(sec, key, v.c_str(), buf, sizeof(buf), over); v = buf; }
        return v;
    };
    g_ini.debug = Int("General", "bDebug", 0) != 0;
    g_ini.sandyEchoes = Int("Sandevistan", "bAfterimages", 0) != 0;
    // The author, 2026-11-08 (F8): "no hotkey, only openable by the doctor. Maybe config or hotkey anywhere to see your setup."
    // The shop opens only by talking to a ripperdoc (or its chair). iOpenKey is read from the TEST overlay alone, so no
    // player ini can bring the shop hotkey back; iSetupKey opens the read-only view. An older player Cyberware.ini that
    // still says iOpenKey=<key> and has no iSetupKey keeps that key, which now opens the read-only view (migration;
    // nothing is written to the file, MO2 would put the write in overwrite).
    g_ini.openKey = hasOver ? (std::uint32_t)GetPrivateProfileIntA("General", "iOpenKey", 0, over) : 0;
    {
        const int oldOpen = (int)GetPrivateProfileIntA("General", "iOpenKey", -1, path);
        const int mainSetup = (int)GetPrivateProfileIntA("General", "iSetupKey", -1, path);
        int v = mainSetup >= 0 ? mainSetup : (oldOpen >= 0 ? oldOpen : 0x77);   // F8; Insert never reaches MenuControls
        if (mainSetup < 0 && oldOpen >= 0)
            REX::INFO("ini: migrated [General] iOpenKey=0x{:X} -> iSetupKey (that key now opens your setup, read only; the shop is at a ripperdoc)", oldOpen);
        if (hasOver) v = (int)GetPrivateProfileIntA("General", "iSetupKey", v, over);
        g_ini.setupKey = (std::uint32_t)std::max(0, v);
    }
    g_ini.pauseGame = Int("General", "bPauseGame", 1) != 0;
    g_ini.stateLock = Int("General", "bStateLock", 0) != 0;
    g_ini.kiroshiEyes = Int("Customize", "bKiroshiEyeGlow", 0) != 0;
    g_ini.escGuard = Int("Menu", "bEscGuard", 1) != 0;
    g_ini.talkUnpaused = Int("Menu", "bTalkUnpaused", 0) != 0;
    g_ini.talkIdles = Int("Menu", "bTalkIdles", 0) != 0;
    g_ini.talkBranchFlags = Int("Menu", "iTalkBranchFlags", -1);
    g_ini.keyOS = Int("Keys", "iKeyOS", 0xDE);   // apostrophe '. NOT Home (0x24): AAF's default admin hotkey is Home, so a Home press also started an AAF scene over the Sandevistan (QD block 24, P14).
    g_ini.keyCamo = Int("Keys", "iKeyCamo", 0x23);
    g_ini.keyScan = Int("Keys", "iKeyScan", 0x21);
    g_ini.keyHeal = Int("Keys", "iKeyHeal", 0x22);
    g_ini.keyHack = Int("Keys", "iKeyHack", 0x2E);
    g_ini.keyHackNext = Int("Keys", "iKeyHackNext", 0xDD);
    g_ini.keyPacify = Int("Keys", "iKeyPacify", 0xDC);
    g_ini.keySkippy = Int("Keys", "iKeySkippyMode", 0xDB);
    g_ini.finishers = Int("Arms", "bFinishers", 0) != 0;
    g_ini.plsRoundsShipped = Str("Arms", "sPlsRoundsShipped", cw::kPlsRoundsShippedDefault);   // EMPTY = no round ships: a missing key fails closed (one default, shared with cwtest --ini)
    REX::INFO("ini: [Arms] sPlsRoundsShipped='{}'{}", g_ini.plsRoundsShipped,
              g_ini.plsRoundsShipped.empty() ? " (no launcher round mod is offered by this build)" : "");
    g_ini.plsRepick = Int("Arms", "bPlsRepick", 0) != 0;
    REX::INFO("ini: [Arms] bPlsRepick={}", g_ini.plsRepick);
    g_ini.plsChargeGlow = Int("Arms", "bPlsChargeGlow", 0) != 0;   // default OFF until QD passes it
    g_ini.mantisFold3P = Int("Arms", "bMantisFold3P", 0) != 0;     // default OFF until QD passes it
    REX::INFO("ini: [Arms] bMantisFold3P={}", g_ini.mantisFold3P);
    g_ini.mantisStow3P = Int("Arms", "bMantisStow3P", 0) != 0;     // default OFF until QD passes it
    g_ini.foldStowAtMs = std::clamp(Int("Arms", "iFoldStowAtMs", 900), 0, 1700);
    g_ini.foldStowInMs = std::clamp(Int("Arms", "iFoldStowInMs", 300), 50, 1000);
    g_ini.foldStowOutMs = std::clamp(Int("Arms", "iFoldStowOutMs", 200), 50, 1000);
    REX::INFO("ini: [Arms] bMantisStow3P={} iFoldStowAtMs={} iFoldStowInMs={} iFoldStowOutMs={}", g_ini.mantisStow3P, g_ini.foldStowAtMs, g_ini.foldStowInMs, g_ini.foldStowOutMs);
    REX::INFO("ini: [Arms] bPlsChargeGlow={}", g_ini.plsChargeGlow);
    g_ini.plsHidePipboy3P = Int("Arms", "bPlsHidePipboy3P", 0) != 0;   // default OFF: the author decides the play default
    g_ini.visualReattach = Int("Arms", "bVisualReattach", 0) != 0;
    g_ini.keepHousing = Int("Arms", "bKeepHousing", 0) != 0;
    REX::INFO("ini: [Arms] bKeepHousing={}", g_ini.keepHousing);
    REX::INFO("ini: [Arms] bVisualReattach={}", g_ini.visualReattach);
    REX::INFO("ini: [Arms] bPlsHidePipboy3P={}", g_ini.plsHidePipboy3P);
    g_ini.finisherFloor = Int("Arms", "bFinisherFloor", 0) != 0;   // default OFF: the old totalDamage hold until a cycle proves the lift
    // QD 2026-11-10: the ease must run for at least 0.2 s, so a non-zero value is floored there rather than silently honoured
    g_ini.finisherSlideMs = std::clamp(Int("Arms", "iFinisherSlideMs", 0), 0, 600);
    if (g_ini.finisherSlideMs > 0 && g_ini.finisherSlideMs < 200) g_ini.finisherSlideMs = 200;
    g_ini.finisherPin = Int("Arms", "bFinisherPin", 0) != 0;
    g_ini.finisherTrace = Int("Arms", "bFinisherTrace", 0) != 0;
    g_ini.hitFieldLog = Int("Arms", "bHitFieldLog", 0) != 0;
    REX::INFO("ini: [Arms] bFinisherFloor={} iFinisherSlideMs={} bFinisherPin={} bFinisherTrace={} bHitFieldLog={}", g_ini.finisherFloor, g_ini.finisherSlideMs, g_ini.finisherPin, g_ini.finisherTrace, g_ini.hitFieldLog);
    g_ini.leapRepick = Int("Arms", "bLeapRepick", 0) != 0;   // OFF for players until the real leap/slam pass QUALITY (the lead)
    g_ini.knockback = Int("Arms", "bKnockback", 1) != 0;
    g_ini.dismember = Int("Arms", "bDismember", 0) != 0;   // default OFF until the stability gate passes
    g_ini.hitFromDoHitMe = Int("Arms", "bHitFromDoHitMe", 0) != 0;   // default OFF until the stability gate passes (same hook)
    REX::INFO("ini: [Arms] bHitFromDoHitMe={}", g_ini.hitFromDoHitMe);
    g_ini.stuckUndrive = Int("Arms", "bStuckUndrive", 0) != 0;   // default OFF: a diagnosis aid, the test ini sets 1
    g_ini.slamNoRepick = Int("Arms", "bSlamNoRepick", 0) != 0;   // default OFF until a cycle proves the effect-only slam
    g_ini.leapLaunch = Int("Arms", "bLeapLaunch", 0) != 0;       // default OFF until a cycle proves the launch
    g_ini.leapHangMs = Int("Arms", "iLeapHangMs", 0);            // default 0: the distance-scaled hang, as before the key existed
    if (g_ini.leapHangMs != 0) g_ini.leapHangMs = std::clamp(g_ini.leapHangMs, 250, 600);
    REX::INFO("ini: [Arms] iLeapHangMs={}", g_ini.leapHangMs);
    g_ini.hackEffects = Int("Quickhacks", "bHackEffects", 0) != 0;   // default OFF until a cycle proves the blind and the stagger
    g_ini.secondHeartImod = Int("Abilities", "iSecondHeartImod", 0x00064C27);
    REX::INFO("ini: [Abilities] iSecondHeartImod=0x{:X}", (std::uint32_t)g_ini.secondHeartImod);
    REX::INFO("ini: [Quickhacks] bHackEffects={}", g_ini.hackEffects);
    g_ini.clearEquipLocks = Int("Arms", "bClearEquipLocks", 1) != 0;   // default ON: the build door keeps an existing Cyberware.ini, so the key may be absent; 0 in an ini turns it off
    REX::INFO("ini: [Arms] bClearEquipLocks={}", g_ini.clearEquipLocks);
    g_ini.visualKeep = Int("Arms", "bVisualKeep", 0) != 0;       // default OFF until a cycle proves the keep-and-cull visual
    g_ini.sheathHideMs = std::clamp(Int("Arms", "iSheathHideMs", 220), 0, 1500);
    g_ini.sheathHide1PMaxMs = std::clamp(Int("Arms", "iSheathHide1PMaxMs", 0), 0, 1500);
    g_ini.hud = Int("HUD", "bEnabled", 1) != 0;
    g_ini.hudView = Str("HUD", "sView", g_ini.hudView.c_str());
    g_ini.hudMessages = Int("HUD", "bMessages", 0) != 0;
    // CLINIC: the schedule mover. OFF by default (new risky default law); Cyberware.Test.ini turns it on.
    clinic::Configure(Int("Clinic", "bSchedule", 0) != 0,
                      Str("Clinic", "sClinicCell", "Cyberware - Clinic.esp|800"),
                      Str("Clinic", "sMarketWorldspace", "Fallout4.esm|F94"),
                      Str("Clinic", "sZoneShop", "1800,2065,567.5,1.57"),
                      Str("Clinic", "sZoneMarket", "2250,1500,-1569,3.5"));
    // INSTALL SCENE (docs/INSTALL-SCENE.md). Every value is data: no record id is hard-coded in cw_scene.cpp.
    // The code defaults carry Varga's clinic records (CLINIC's frozen seat A4E / mark A4F at 1262600f and her two
    // station packages), because the build door never adds keys to an existing Cyberware.ini: the code default is
    // what a player's build reads. They are harmless with bChairScene=0, which is the player default.
    g_ini.sceneChair = Int("Scene", "bChairScene", 0) != 0;
    g_ini.sceneCamPitch = Int("Scene", "bCamPitch", 0) != 0;
    g_ini.sceneFlare = Int("Scene", "iFlare", 0);
    {
        struct Def { const char* doc; const char* seat; const char* mark; const char* glob; const char* pkgs; };
        static constexpr Def kDefs[] = {
            // Varga: CLINIC's seat A4E, surgeon mark A4F, the scene global A50 and her two "at the station and free"
            // packages (cw-clinic-seat @ 5ca0a39c). CW_ClinicSceneActive (A50) is a short GLOB starting at 0; writing 1
            // and evp-ing her starts CW_ClinicStepIn (PACK A51, first in her list, conditioned
            // GetInCell clinic == 1 AND GetGlobalValue A50 == 1), which travels her to A4F and takes its heading 148.
            // Writing 0 hands her back to the clock packages at cw_clinic.cpp's next 5 s poll. With the global absent
            // or unresolvable the DLL logs a WARN and falls back to turning her in place, so a wrong id degrades
            // honestly instead of silently doing nothing.
            { "diamondcity", "Cyberware - Clinic.esp|A4E", "Cyberware - Clinic.esp|A4F", "Cyberware - Clinic.esp|A50", "Cyberware - Clinic.esp|975,A4C" },
            { "goodneighbor", "", "", "", "" },   // Kessler's own lounger is a real FURN in the formmap; set sSeat_ to use it
            { "institute", "", "", "", "" },
        };
        std::vector<scene::DocIni> docs;
        for (const auto& d : kDefs) {
            scene::DocIni si;
            si.doc = d.doc;
            const std::string n = d.doc;
            si.seat = Str("Scene", ("sSeat_" + n).c_str(), d.seat);
            si.mark = Str("Scene", ("sMark_" + n).c_str(), d.mark);
            si.global = Str("Scene", ("sSceneGlobal_" + n).c_str(), d.glob);
            si.pkgs = Str("Scene", ("sReadyPkgs_" + n).c_str(), d.pkgs);
            si.shotA = Str("Scene", ("sShotA_" + n).c_str(), "");
            si.shotB = Str("Scene", ("sShotB_" + n).c_str(), "");
            si.entry = Str("Scene", ("sEntry_" + n).c_str(), "");
            docs.push_back(si);
        }
        scene::Configure(g_ini.sceneChair, g_ini.sceneCamPitch, g_ini.sceneFlare, docs);
    }
    pkgdiag::SetClinicPlugin(Str("Clinic", "sClinicCell", "Cyberware - Clinic.esp|800"));   // names Varga's packages in the packages seam
    g_ini.hudEditKey = Int("HUD", "iEditKey", 0x71);
    g_ini.hudLayoutDir = Str("HUD", "sLayoutDir", "");
    hudlayout::Init(g_ini.hudLayoutDir);
    if (const std::string prefs = hudlayout::PrefsIni(); !prefs.empty()) {   // the Settings tab's key choice (op setKey)
        const int saved = (int)GetPrivateProfileIntA("Keys", "iSetupKey", -1, prefs.c_str());
        if (saved >= 0) { g_ini.setupKey = (std::uint32_t)saved; REX::INFO("ini: iSetupKey=0x{:X} from {}", saved, prefs); }
    }
    if (g_ini.openKey && g_ini.openKey == g_ini.setupKey) {
        REX::WARN("ini: iOpenKey and iSetupKey are both 0x{:X}: the shop key wins, the setup view has no key", g_ini.openKey);
        g_ini.setupKey = 0;
    }
    if (g_ini.hudEditKey && g_ini.hudEditKey == g_ini.setupKey) {   // the player chose the setup key in Settings; the editor is still on Settings and L
        REX::WARN("ini: [HUD] iEditKey and iSetupKey are both 0x{:X}: the setup view keeps the key, the HUD layout editor has no key", g_ini.hudEditKey);
        g_ini.hudEditKey = 0;
    }
    REX::INFO("ini: [HUD] iEditKey=0x{:X} sLayoutDir='{}' -> {}", g_ini.hudEditKey, g_ini.hudLayoutDir, hudlayout::Path());
    g_ini.voice = Int("Voice", "bEnabled", 0) != 0;   // default OFF (RIPPERDOC VOICES): unproven in game
    g_ini.voicesPath = Str("Voice", "sVoices", g_ini.voicesPath.c_str());
    g_ini.voiceProbe = Int("Voice", "bProbe", 0) != 0;
    REX::INFO("ini: [Voice] bEnabled={} sVoices='{}'", g_ini.voice ? 1 : 0, g_ini.voicesPath);
    g_ini.snd3d = Int("Sound", "bSnd3D", 0) != 0;
    g_ini.sndCues = Int("Sound", "bCues", 0) != 0;
    g_ini.sndMasterDb = std::min(0.0, std::strtod(Str("Sound", "fMasterDb", "0").c_str(), nullptr));
    REX::INFO("ini: [Sound] bSnd3D={} bCues={} fMasterDb={:.1f}", g_ini.snd3d ? 1 : 0, g_ini.sndCues ? 1 : 0, g_ini.sndMasterDb);
    REX::INFO("ini: overlay {} ({})", over, hasOver ? "found: its keys override Cyberware.ini" : "none: player defaults only");
    REX::INFO("ini: [Arms] bFinishers={} bLeapRepick={} bKnockback={} bDismember={} bStuckUndrive={} bSlamNoRepick={} bLeapLaunch={} bVisualKeep={} iSheathHideMs={} iSheathHide1PMaxMs={} [HUD] bEnabled={} sView='{}' bMessages={}", g_ini.finishers, g_ini.leapRepick, g_ini.knockback, g_ini.dismember, g_ini.stuckUndrive, g_ini.slamNoRepick, g_ini.leapLaunch, g_ini.visualKeep, g_ini.sheathHideMs, g_ini.sheathHide1PMaxMs, g_ini.hud, g_ini.hudView, g_ini.hudMessages);
    g_ini.openKeyDoc = Str("General", "sOpenKeyRipperdoc", "");
    g_ini.cmdFile = Str("Debug", "sCmdFile", "");
    g_ini.keyLogMax = std::max(0, Int("Debug", "iKeyLogMax", 40));
    if (g_ini.debug) REX::INFO("ini: [Debug] iKeyLogMax={}", g_ini.keyLogMax);
    if (!g_ini.cmdFile.empty()) REX::INFO("ini: [Debug] sCmdFile='{}' ({})", g_ini.cmdFile, g_ini.debug ? "on" : "ignored: bDebug=0");
    g_ini.stageBAnims = Int("Smart", "bStageBAnims", 0) != 0;   // default OFF: CP 1P clips for the Yukimura (ARMORY stage B, test build only)
    g_ini.smartLock = Int("Smart", "bSmartLock", 0) != 0;   // default OFF: per-projectile hook (risky default law)
    g_ini.smartSteer = Int("Smart", "iSteerMode", 2);
    g_ini.smartDiag = Int("Smart", "bDiag", 0) != 0;
    g_ini.armoryFormmapPath = Str("Paths", "sArmoryFormMap", g_ini.armoryFormmapPath.c_str());
    g_ini.armorySellUnready = Int("Armory", "bSellUnready", 0) != 0;
    REX::INFO("ini: [Smart] bSmartLock={} iSteerMode={} bDiag={} [Paths] sArmoryFormMap='{}'", g_ini.smartLock, g_ini.smartSteer, g_ini.smartDiag, g_ini.armoryFormmapPath);
    g_ini.settingsDir = Str("Settings", "sSettingsDir", "");
    if (!g_ini.settingsDir.empty()) REX::INFO("ini: [Settings] sSettingsDir='{}'", g_ini.settingsDir);
    g_ini.catalogPath = Str("Paths", "sCatalog", g_ini.catalogPath.c_str());
    g_ini.formmapPath = Str("Paths", "sFormMap", g_ini.formmapPath.c_str());
    g_ini.viewPath = Str("Paths", "sView", g_ini.viewPath.c_str());
    REX::INFO("ini: [General] bStateLock={}", g_ini.stateLock ? 1 : 0);
    REX::INFO("ini: iSetupKey=0x{:X} (read-only setup view)", g_ini.setupKey);
    REX::INFO("ini: bDebug={} iOpenKey=0x{:X} sOpenKeyRipperdoc='{}' bPauseGame={} keys OS=0x{:X} camo=0x{:X} scan=0x{:X} catalog='{}' formmap='{}' view='{}'",
              g_ini.debug, g_ini.openKey, g_ini.openKeyDoc, g_ini.pauseGame, g_ini.keyOS, g_ini.keyCamo, g_ini.keyScan, g_ini.catalogPath, g_ini.formmapPath, g_ini.viewPath);
}

#include "cw_sound.inl"   // SOUND: snd::Play2D / PlayAt / PlayFollow (reads g_ini.snd3d)

std::optional<json> ReadJsonFile(const std::string& path, std::string& err)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return std::nullopt; }
    try { return json::parse(f); }
    catch (const std::exception& e) { err = path + ": " + e.what(); return std::nullopt; }
}

// "Plugin.esm|00ABCD" -> form, or nullptr
RE::TESForm* ResolveFormKey(const std::string& fk)
{
    auto bar = fk.find('|');
    if (bar == std::string::npos) return nullptr;
    std::string plugin = fk.substr(0, bar);
    std::uint32_t id = 0;
    try { id = (std::uint32_t)std::stoul(fk.substr(bar + 1), nullptr, 16); } catch (...) { return nullptr; }
    auto* dh = RE::TESDataHandler::GetSingleton();
    return dh ? dh->LookupForm(id & 0x00FFFFFF, plugin) : nullptr;
}

// ---------------------------------------------------------------- formmap (from espgen)
struct FormMap {
    std::string plugin = cw::kPluginName;
    std::map<std::string, std::uint32_t> ids;                  // EditorID -> local id
    std::map<std::uint32_t, std::string> chairToDoc;           // local id of an ACTI -> ripperdoc id
    struct ChairRef { std::string plugin; std::uint32_t local; std::string doc; };
    std::vector<ChairRef> chairRefs;                           // placed chair REFRs (ours or a vanilla one) -> ripperdoc id
    std::map<std::uint32_t, std::string> npcToDoc;             // local id of a ripperdoc NPC_ base (CW_Ripperdoc_<doc>) -> ripperdoc id
    std::map<std::string, std::pair<std::string, std::string>> chips;  // MISC EditorID -> (kind, id)
    std::string generatorCatalogSha;
    json native = json::object();                              // CW_Nat_* perk -> {kind, magnitude, areaFeet}
    json arms = json::object();                                // ARMS: {weapons{edid:{handler,animKeyword}}, visuals{kw:armo}, leapKeyword, leapWeaponKeyword}

    RE::TESForm* Get(const std::string& edid) const
    {
        auto it = ids.find(edid);
        if (it == ids.end()) return nullptr;
        auto* dh = RE::TESDataHandler::GetSingleton();
        return dh ? dh->LookupForm(it->second, plugin) : nullptr;
    }
} g_map;

bool LoadFormMap()
{
    std::string err;
    auto j = ReadJsonFile(g_ini.formmapPath, err);
    if (!j) { REX::ERROR("formmap: {}", err); return false; }
    g_map.plugin = j->value("plugin", std::string(cw::kPluginName));
    g_map.generatorCatalogSha = j->value("catalogSha256", std::string());
    for (auto& [k, v] : j->value("forms", json::object()).items())
        if (v.is_string()) g_map.ids[k] = (std::uint32_t)std::stoul(v.get<std::string>(), nullptr, 16);
    for (auto& c : j->value("ripperdocChairs", json::array())) {
        const std::string doc = c.value("ripperdoc", "");
        if (g_map.ids.count(c.value("editorId", ""))) g_map.chairToDoc[g_map.ids[c["editorId"]]] = doc;
        // espgen: "refEditorId" = a chair ref placed in Cyberware.esp; "refForm" = an existing ref ("Fallout4.esm|019966")
        if (const std::string nb = "CW_Ripperdoc_" + doc; !doc.empty() && g_map.ids.count(nb)) g_map.npcToDoc[g_map.ids[nb]] = doc;   // talking to the doc
        const std::string re = c.value("refEditorId", ""), rf = c.value("refForm", "");
        if (!re.empty() && g_map.ids.count(re)) g_map.chairRefs.push_back({ g_map.plugin, g_map.ids[re], doc });
        else if (auto bar = rf.find('|'); bar != std::string::npos) {
            try { g_map.chairRefs.push_back({ rf.substr(0, bar), (std::uint32_t)std::stoul(rf.substr(bar + 1), nullptr, 16) & 0x00FFFFFF, doc }); }
            catch (...) { REX::WARN("formmap: bad refForm '{}' for ripperdoc '{}'", rf, doc); }
        }
    }
    g_map.native = j->value("native", json::object());
    g_map.arms = j->value("arms", json::object());
    for (auto& c : j->value("chips", json::array()))
        g_map.chips[c.value("editorId", "")] = { c.value("kind", ""), c.value("id", "") };
    REX::INFO("formmap: {} forms of {}, {} ripperdoc chair(s), {} placed chair ref(s), {} ripperdoc NPC(s), {} chip type(s), generated from catalog sha256 {}",
              g_map.ids.size(), g_map.plugin, g_map.chairToDoc.size(), g_map.chairRefs.size(), g_map.npcToDoc.size(), g_map.chips.size(), g_map.generatorCatalogSha);
    if (auto it = g_map.ids.find("CW_RipperdocRef_diamondcity"); it != g_map.ids.end())
        clinic::SetVargaRef(g_map.plugin, it->second);   // CLINIC: the schedule mover's actor
    pkgdiag::SetNameLookup([](std::uint32_t id) -> std::string {   // the packages seam names Cyberware.esp forms by EditorID
        for (const auto& [edid, local] : g_map.ids) if (auto* f = g_map.Get(edid); f && f->GetFormID() == id) return edid;
        return {};
    });
    return true;
}

// ---------------------------------------------------------------- actor values by EditorID (vanilla + our custom AVIFs)
std::map<std::string, RE::ActorValueInfo*> g_avByEdid;  // lower-case EditorID

void IndexActorValues()
{
    auto* dh = RE::TESDataHandler::GetSingleton();
    if (!dh) return;
    for (auto* avi : dh->GetFormArray<RE::ActorValueInfo>()) {
        if (!avi) continue;
        std::string e = avi->GetFormEditorID();
        if (e.empty()) continue;
        std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        g_avByEdid[e] = avi;
    }
    REX::INFO("forms: {} actor values indexed by EditorID", g_avByEdid.size());
}

RE::ActorValueInfo* FindAV(std::string e)
{
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    auto it = g_avByEdid.find(e);
    return it == g_avByEdid.end() ? nullptr : it->second;
}

// One actor value of the player as base / permanent / current (permanent - base = perks and abilities, current - permanent =
// temporary + damage). SpeedMult diagnosis (the lead, 2026-11-09): the test save walked 16 u per W hold until
// `forceav speedmult 100`; this line at every load and after every perk change shows who moves it.
std::string PlayerAVLine(const char* av)
{
    auto* p = RE::PlayerCharacter::GetSingleton();
    auto* info = FindAV(av);
    if (!p || !info) return std::string(av) + " n/a";
    auto* o = static_cast<RE::ActorValueOwner*>(p);
    return std::format("{} base {:.1f} perm {:.1f} now {:.1f}", av, o->GetBaseActorValue(*info), o->GetPermanentActorValue(*info), o->GetActorValue(*info));
}

// ---------------------------------------------------------------- native abilities (catalog spec.native.handler)
// v1: a registry with logging stubs. A handler gets Enable when its ability becomes active (or its tier/params
// change) and Disable when it goes away. Real handlers (time slow, scan, optical camo, on-kill/on-hit) plug in here.
struct NativeHandler {
    std::function<void(const cw::NativeAbility&)> enable, disable;
};
// AB-E1 (QD's row for defect E, 2026-11-10). The test seam {"cmd":"chromecrit","on":0|1} takes CW_ChromeCrit off the
// player and puts it back INSIDE ONE BOOT, so clips with and without the banded perk can alternate in one scene.
// Test seam only (bDebug=1 and [Debug] sCmdFile): no INI key, nothing a player can reach.
std::atomic<bool> g_abE1Off{ false };      // true: ApplyDesired leaves CW_ChromeCrit out of the wanted set, so the player loses it
std::atomic<bool> g_abE1HitLog{ false };   // true once the seam was used: every player hit the DoHitMe hook sees logs its time

namespace arms { void CueAutoEquipSoon(); }   // SOUND: an equip-activation enable means the draw that follows is the game's, not the player's
std::map<std::string, NativeHandler> g_handlers;
std::map<std::string, cw::NativeAbility> g_activeNative;  // ability id -> what is enabled now

// [Arms] sPlsRoundsShipped: thin wrappers over the PURE core functions, so the gate's logic is tested in cwtest
// (cw::PlsRoundListed / cw::PlsRoundShips, beside cw::PlsPoolStep) rather than only exercised in a game. The plugin's job
// here is to supply the ini value; the decisions - the aliases, the empty-list-ships-nothing rule and the thermal/incendiary
// pair rule - all live in the core and are covered by the table in cwtest.
bool PlsRoundListed(const std::string& suffix) { return cw::PlsRoundListed(g_ini.plsRoundsShipped, suffix); }
bool PlsRoundShips(const std::string& suffix) { return cw::PlsRoundShips(g_ini.plsRoundsShipped, suffix); }

// Drop every UNSHIPPED launcher round from the catalog JSON before the engine parses it. This is the point that makes the
// lead's ruling a refusal rather than a checklist: gating only the handler registration would leave the round SOLD AND
// INERT (SyncNative marks a handler-less ability active and logs "stub"), which is worse for a player than shipping it
// ungraded. Dropping the ability AND its mod means the round is never offered, bought or installed.
// An already-installed instance of a dropped round becomes an engine orphan ("not in the catalog - kept as orphan"), which
// is the correct and visible outcome for a test save, and no player build has ever shipped these.
int PlsRoundsFilterCatalog(json& j)
{
    // The thermal / incendiary pair rule lives in PlsRoundShips so every caller agrees; it is only REPORTED here, once, at
    // catalog load, because a list that names one of them is a mistake someone should see rather than a silent drop.
    if (PlsRoundListed("therm") != PlsRoundListed("incendiary"))
        REX::WARN("arms: [Arms] sPlsRoundsShipped='{}' names only ONE of thermal / incendiary, but they swap the SAME materials and are one gradable unit: NEITHER ships", g_ini.plsRoundsShipped);
    // The drop AND every reference to a dropped id are the core's (cw::PlsFilterCatalog, proven in cwtest on the real catalog):
    // on b7 the plugin dropped the six mods but left ripperdoc 'diamondcity' modStock naming them, so the core never loaded.
    std::vector<std::string> lines;
    const int n = cw::PlsFilterCatalog(j, g_ini.plsRoundsShipped, &lines);
    for (auto& l : lines) REX::INFO("catalog: [Arms] sPlsRoundsShipped='{}': {}", g_ini.plsRoundsShipped, l);
    return n;
}

void SyncNative(const std::vector<cw::NativeAbility>& want)
{
    std::map<std::string, const cw::NativeAbility*> w;
    for (auto& n : want) w[n.ability] = &n;
    for (auto it = g_activeNative.begin(); it != g_activeNative.end();) {
        auto f = w.find(it->first);
        if (f != w.end() && f->second->tier == it->second.tier && f->second->params == it->second.params) { ++it; continue; }
        auto h = g_handlers.find(it->second.handler);
        if (h != g_handlers.end() && h->second.disable) h->second.disable(it->second);
        REX::INFO("native: - {} ({}) t{}", it->first, it->second.handler, it->second.tier);
        it = g_activeNative.erase(it);
    }
    for (auto& [id, n] : w) {
        if (g_activeNative.count(id)) continue;
        auto h = g_handlers.find(n->handler);
        bool impl = h != g_handlers.end() && h->second.enable;
        if (impl) { h->second.enable(*n); if (n->activation == "equip") arms::CueAutoEquipSoon(); }
        REX::INFO("native: + {} ({}, {}) t{} params {}{}", id, n->handler, n->activation, n->tier, n->params.dump(), impl || cw::FamilyOf(n->handler, n->activation, n->params) != cw::NativeFamily::Stub ? "" : " - handler not implemented yet (stub)");
        g_activeNative[id] = *n;
    }
}

// ---------------------------------------------------------------- the game side of cw::Env
namespace optics { extern bool hudView; }   // GameEnv::Notify routes by it (defined with the optics code below)
namespace hud { void Event(const json& e); void BeginEdit(const char* via); void ToggleEdit(); }

namespace chrome { void HackVisual(const std::string& phase, const std::string& id, std::uint32_t target, double seconds); }
namespace chrome { void HackEffect(const std::string& kind, std::uint32_t target, double seconds, double radiusM); }
namespace fx { void AbilityCue(const std::string& handler, const std::string& phase); }

class GameEnv final : public cw::Env {
public:
    RE::TESBoundObject* currency = nullptr;
    std::set<std::uint32_t> knownPerks;       // every Cyberware.esp perk the generator made (formIDs)
    std::set<std::uint32_t> externalApplied;  // non-Cyberware.esp perks we added (cosaved, 'EXTP')

    RE::PlayerCharacter* P() { return RE::PlayerCharacter::GetSingleton(); }

    std::int64_t GetCurrency() override
    {
        auto* p = P();
        if (!p || !currency) return 0;
        return p->GetInventoryObjectCount(currency);
    }
    bool SpendCurrency(std::int64_t amount) override
    {
        auto* p = P();
        if (!p || !currency || amount <= 0) return amount == 0;
        if (GetCurrency() < amount) return false;
        // Silent (the lead, 2026-11-09): the vanilla "Bottle Cap -N" HUD feed stays quiet;
        // the ripperdoc menu and our HUD are what show the cost.
        RE::PlayerCharacter::ScopedInventoryChangeMessageContext silent(true, true);
        RE::TESObjectREFR::RemoveItemData rd(currency, (std::int32_t)amount);
        rd.reason = RE::ITEM_REMOVE_REASON::kSelling;
        p->RemoveItem(rd);
        return true;
    }
    void AddCurrency(std::int64_t amount) override
    {
        auto* p = P();
        if (!p || !currency || amount <= 0) return;
        RE::PlayerCharacter::ScopedInventoryChangeMessageContext silent(true, true);   // sell-backs too
        p->AddObjectToContainer(currency, RE::BSTSmartPointer<RE::ExtraDataList>{}, (std::int32_t)amount, nullptr, RE::ITEM_REMOVE_REASON::kNone);
    }
    int GetLevel() override
    {
        auto* p = P();
        return p ? (int)p->GetLevel() : 1;
    }
    int GetFo4PerkRank(const std::string& fk) override
    {
        auto* p = P();
        auto* form = ResolveFormKey(fk);
        auto* perk = form ? form->As<RE::BGSPerk>() : nullptr;
        if (!p || !perk) return 0;
        // FO4 ranks are separate records chained by nextPerk: count how many links the player has.
        int rank = 0;
        for (int guard = 0; perk && guard < 16; ++guard, perk = perk->nextPerk)
            if (p->GetPerkRank(perk) > 0) ++rank;
        return rank;
    }
    void ApplyDesired(const cw::Desired& d) override
    {
        auto* p = P();
        if (!p) return;
        std::set<std::uint32_t> want, wantExternal;
        int missing = 0;
        for (auto& e : d.perkEditorIds) {
            if (!e.empty() && e[0] == '@') {
                auto* f = ResolveFormKey(e.substr(1));
                if (f && f->As<RE::BGSPerk>()) wantExternal.insert(f->GetFormID());
                else ++missing;
                continue;
            }
            if (g_abE1Off.load() && e == cw::kChromeCritPerk) continue;   // AB-E1 "without" arm: the perk stays known, so it is REMOVED below
            auto* f = g_map.Get(e);
            if (f && f->As<RE::BGSPerk>()) want.insert(f->GetFormID());
            else { ++missing; if (missingLogged_.insert(e).second) REX::WARN("reconcile: perk {} is not in formmap/Cyberware.esp (regenerate the ESP?)", e); }
        }
        int added = 0, removed = 0;
        // every wanted perk joins the managed set: CW_ChromeCrit has none of the CW_Stat_/CW_Ab_/CW_Nat_ prefixes ResolveForms
        // collects, so it was wanted and never added (first in-game test: "verified on player 18/19", the crit system off)
        for (std::uint32_t id : want) knownPerks.insert(id);
        for (std::uint32_t id : knownPerks) {
            auto* perk = RE::TESForm::GetFormByID(id) ? RE::TESForm::GetFormByID(id)->As<RE::BGSPerk>() : nullptr;
            if (!perk) continue;
            bool has = p->GetPerkRank(perk) > 0, should = want.count(id) > 0;
            if (should && !has) { p->AddPerk(perk, 0); ++added; }
            else if (!should && has) { p->RemovePerk(perk); ++removed; }
        }
        for (std::uint32_t id : wantExternal) {
            auto* perk = RE::TESForm::GetFormByID(id)->As<RE::BGSPerk>();
            if (p->GetPerkRank(perk) == 0) { p->AddPerk(perk, 0); externalApplied.insert(id); ++added; }
        }
        for (auto it = externalApplied.begin(); it != externalApplied.end();) {
            if (wantExternal.count(*it)) { ++it; continue; }
            if (auto* f = RE::TESForm::GetFormByID(*it); f && f->As<RE::BGSPerk>()) { p->RemovePerk(f->As<RE::BGSPerk>()); ++removed; }
            it = externalApplied.erase(it);
        }
        if (added + removed > 0)   // a SpeedMult change applies only after a CarryWeight nudge (docs/DESIGN.md 3.5)
            if (auto* cw = FindAV("CarryWeight")) {
                auto* o = static_cast<RE::ActorValueOwner*>(p);
                o->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, *cw, 1.0f);
                o->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, *cw, -1.0f);
                REX::INFO("reconcile: perks changed (+{} -{}), {}", added, removed, PlayerAVLine("SpeedMult"));
            }
        int verified = 0;
        for (std::uint32_t id : want)
            if (auto* f = RE::TESForm::GetFormByID(id); f && p->GetPerkRank(f->As<RE::BGSPerk>()) > 0) ++verified;
        REX::INFO("reconcile: want {} CW perk(s) + {} external, +{} -{}, verified on player {}/{}, unresolved {}, visuals {} (not applied yet), native {}",
                  want.size(), wantExternal.size(), added, removed, verified, want.size(), missing, d.visuals.size(), d.native.size());
        SyncNative(d.native);
    }
    std::vector<cw::ChipFound> TakeChips() override
    {
        std::vector<cw::ChipFound> out;
        auto* p = P();
        if (!p) return out;
        for (auto& [edid, kv] : g_map.chips) {
            if (kv.first == "capacity") continue;  // capacity items are consumed by RedeemCapacityItems, up to their max
            auto* f = g_map.Get(edid);
            auto* obj = f ? f->As<RE::TESBoundObject>() : nullptr;
            if (!obj) continue;
            std::uint32_t n = p->GetInventoryObjectCount(obj);
            if (!n) continue;
            RE::TESObjectREFR::RemoveItemData rd(obj, (std::int32_t)n);
            p->RemoveItem(rd);
            out.push_back({ kv.first, kv.second, (int)n });
            REX::INFO("chips: took {} x {} ({} '{}')", n, edid, kv.first, kv.second);
        }
        return out;
    }
    std::uint64_t Entropy() override
    {
        std::random_device rd;
        return ((std::uint64_t)rd() << 32) ^ rd() ^ (std::uint64_t)GetTickCount64();
    }
    double GetActorValue(const std::string& av) override
    {
        auto* p = P();
        auto* info = FindAV(av);
        return (p && info) ? static_cast<RE::ActorValueOwner*>(p)->GetActorValue(*info) : 0.0;
    }
    double GetBaseActorValue(const std::string& av) override
    {
        auto* p = P();
        auto* info = FindAV(av);
        return (p && info) ? static_cast<RE::ActorValueOwner*>(p)->GetBaseActorValue(*info) : 0.0;
    }
    double GetPermanentActorValue(const std::string& av) override
    {
        auto* p = P();
        auto* info = FindAV(av);
        return (p && info) ? static_cast<RE::ActorValueOwner*>(p)->GetPermanentActorValue(*info) : 0.0;
    }
    RE::TESBoundObject* ItemForm(const std::string& key)
    {
        RE::TESForm* f = (!key.empty() && key[0] == '@') ? ResolveFormKey(key.substr(1)) : g_map.Get(key);
        return f ? f->As<RE::TESBoundObject>() : nullptr;
    }
    int CountItem(const std::string& key) override
    {
        auto* p = P();
        auto* obj = ItemForm(key);
        return (p && obj) ? (int)p->GetInventoryObjectCount(obj) : 0;
    }
    bool ConsumeItem(const std::string& key, int n) override
    {
        auto* p = P();
        auto* obj = ItemForm(key);
        if (!p || !obj || n <= 0 || CountItem(key) < n) return false;
        RE::TESObjectREFR::RemoveItemData rd(obj, n);
        p->RemoveItem(rd);
        REX::INFO("items: consumed {} x {}", n, key);
        return true;
    }
    // Upgrade components (BETA QA): FO4-crafting counting (raw + inside junk) and scrap-on-consume.
    int CountComponent(const std::string& key) override
    {
        RE::TESForm* f = ResolveFormKey(key[0] == '@' ? key.substr(1) : key);
        if (!f) f = g_map.Get(key);
        return cwcomp::Count(P(), f);
    }
    bool ConsumeComponents(const std::vector<std::pair<std::string, int>>& need) override
    {
        return cwcomp::Consume(P(), [](const std::string& k) -> RE::TESForm* {
            RE::TESForm* f = ResolveFormKey(!k.empty() && k[0] == '@' ? k.substr(1) : k);
            return f ? f : g_map.Get(k);
        }, need);
    }
    int64_t NowMs() override { return (int64_t)GetTickCount64(); }
    double ActorValuePct(const std::string& av) override
    {
        auto* p = P();
        auto* info = FindAV(av);
        if (!p || !info) return 100;
        auto* o = static_cast<RE::ActorValueOwner*>(p);
        float mx = o->GetPermanentActorValue(*info);
        return mx > 0 ? 100.0 * o->GetActorValue(*info) / mx : 100;
    }
    void RestoreActorValuePct(const std::string& av, double pct) override
    {
        auto* p = P();
        auto* info = FindAV(av);
        if (!p || !info || pct <= 0) return;
        auto* o = static_cast<RE::ActorValueOwner*>(p);
        float amount = (float)(o->GetPermanentActorValue(*info) * pct / 100.0);
        o->RestoreActorValue(*info, amount);
        REX::INFO("native: restored {} {:.1f} ({}%)", av, amount, pct);
    }
    void Notify(const std::string& text) override
    {
        REX::INFO("hud: {}", text);
        // With the CyberwareHUD view up, an ability's timer, RAM and cooldown are already on it: the old vanilla
        // top-left line ("Kiroshi Doomsayer Optics: 12 s") showed the same thing twice (VISUAL POLISH, the lead's shot
        // desk-135853). Only a refusal, which the view cannot show, becomes a one-shot note on it. bMessages=1, or no
        // view, keeps the vanilla line for everything.
        if (!optics::hudView || g_ini.hudMessages) { RE::SendHUDMessage::ShowHUDMessage(text.c_str(), "", false, false); return; }
        for (const char* k : { "not enough RAM", "no hostile target", "recharging" })
            if (text.find(k) != std::string::npos) { hud::Event({ { "type", "note" }, { "text", text }, { "tone", "bad" } }); return; }
    }
    void RestoreActorValue(const std::string& av, double amount) override
    {
        auto* p = P();
        auto* info = FindAV(av);
        if (!p || !info || amount <= 0) return;
        static_cast<RE::ActorValueOwner*>(p)->RestoreActorValue(*info, (float)amount);
        if (amount >= 1) REX::INFO("native: restored {} {:.1f}", av, amount);
    }
    bool GiveItem(const std::string& key, int n) override
    {
        auto* p = P();
        auto* obj = ItemForm(key);
        if (!p || !obj || n <= 0) { REX::ERROR("armory: cannot give {} x {} (form not found: is the plugin loaded?)", n, key); return false; }
        p->AddObjectToContainer(obj, RE::BSTSmartPointer<RE::ExtraDataList>{}, (std::int32_t)n, nullptr, RE::ITEM_REMOVE_REASON::kNone);
        REX::INFO("armory: gave {} x {}", n, key);
        return true;
    }
    bool UseItem(const std::string& key) override
    {
        // Equipping an ALCH is how the game uses it (Stimpak: consumed, its effects applied). Unverified in game.
        auto* p = P();
        auto* obj = ItemForm(key);
        auto* em = RE::ActorEquipManager::GetSingleton();
        if (!p || !obj || !em || CountItem(key) < 1) return false;
        RE::BGSObjectInstance inst(obj, nullptr);
        bool ok = em->EquipObject(p, inst, 0, 1, nullptr, false, false, true, true, false);
        REX::INFO("native: used {} ({})", key, ok ? "ok" : "EquipObject returned false");
        return ok;
    }
    bool PlayerIs(const std::string& what) override
    {
        auto* p = P();
        if (!p) return false;
        if (what == "sneaking") return p->IsSneaking();
        if (what == "combat") return p->IsInCombat();
        if (what == "sprinting") {
            // D10 (2026-11-10): the sprint bit 0x100 is assumed (cw-int-1 saw moveMode 1A6 mid-sprint). Prove it before Kerenzikov builds on
            // it: with bDebug, every moveMode change and every Sprint key edge is logged; a boot that sprints shows whether the bit follows the key.
            const auto mm = (std::uint32_t)p->moveMode;
            static std::uint32_t lastMM = 0xFFFFFFFF; static int logged = 0;
            if (g_ini.debug && mm != lastMM && logged < 400) { ++logged; REX::INFO("kerenzikov: moveMode {:X} -> {:X} (sprint bit 0x100 {})", lastMM == 0xFFFFFFFF ? 0u : lastMM, mm, (mm & 0x100) ? "SET" : "clear"); }
            lastMM = mm;
            return (mm & 0x100) != 0;
        }
        if (what == "aiming") return p->gunState == RE::GUN_STATE::kSighted || p->gunState == RE::GUN_STATE::kFireSighted;
        if (what == "airborne") return p->IsJumping();
        return false;
    }
    // D1 / QD N7: the player cannot read a HUD message while a gameplay menu, the ripperdoc view or the install
    // scene is up, so the benched-quickhacks notice waits for this to go false instead of playing behind them.
    // Defined out of line below, after GameplayMenuOpen and the view globals it needs.
    bool UiBusy() override;
    // D1: the key BOUND to the setup view, so the benched notice never prints a literal (F8 for players, F7 on the
    // test build, "" when it has no key). g_ini is defined above this class.
    std::string SetupKeyName() override { return cw::VkName(g_ini.setupKey); }
    // Quickhack spread: other living hostiles near `around` (machine flag not checked for these: Short Circuit's x2 is
    // the primary target's only).
    std::vector<HackTarget> NearbyHostiles(std::uint32_t around, double radiusM, int n) override
    {
        std::vector<HackTarget> out;
        auto* p = P();
        auto* pl = RE::ProcessLists::GetSingleton();
        auto* cf = RE::TESForm::GetFormByID(around);
        auto* c = cf ? cf->As<RE::Actor>() : nullptr;
        if (!p || !pl || !c || n <= 0) return out;
        using host_t = bool (*)(RE::Actor*, RE::Actor*);
        static REL::Relocation<host_t> hostile{ RE::ID::Actor::GetHostileToActor };
        const auto cp = c->GetPosition();
        const double r2 = (radiusM * 70.0) * (radiusM * 70.0);
        auto* hp = FindAV("Health");
        for (auto& hd : pl->highActorHandles) {
            if ((int)out.size() >= n) break;
            auto a = hd.get();
            if (!a || a.get() == p || a.get() == c || a->lifeState != 0 || !hostile(a.get(), p)) continue;
            auto ap = a->GetPosition();
            if ((ap.x - cp.x) * (ap.x - cp.x) + (ap.y - cp.y) * (ap.y - cp.y) > r2) continue;
            HackTarget t; t.ok = true; t.id = a->GetFormID();
            if (hp) { auto* o = static_cast<RE::ActorValueOwner*>(a.get()); float mx = o->GetPermanentActorValue(*hp); t.hpPct = mx > 0 ? 100.0 * o->GetActorValue(*hp) / mx : 100; }
            out.push_back(t);
        }
        return out;
    }
    // Quickhack target: the hostile, living high-process actor nearest the view heading (within 8 deg) and range, in LOS.
    HackTarget FindHackTarget(double rangeM) override
    {
        HackTarget out;
        auto* p = P();
        auto* pl = RE::ProcessLists::GetSingleton();
        if (!p || !pl) return out;
        using host_t = bool (*)(RE::Actor*, RE::Actor*);
        static REL::Relocation<host_t> hostile{ RE::ID::Actor::GetHostileToActor };
        const auto pp = p->GetPosition();
        const double h = p->GetHeading(), maxU = rangeM * 70.0, cone = 8.0 * 3.14159265358979 / 180.0;
        RE::Actor* best = nullptr;
        double bestAng = cone;
        for (auto& hd : pl->highActorHandles) {
            auto a = hd.get();
            if (!a || a.get() == p || a->lifeState != 0 || !hostile(a.get(), p)) continue;
            auto ap = a->GetPosition();
            double dx = ap.x - pp.x, dy = ap.y - pp.y, dist = std::sqrt(dx * dx + dy * dy);
            if (dist < 1 || dist > maxU) continue;
            double ang = std::fabs(std::remainder(std::atan2(dx, dy) - h, 2 * 3.14159265358979));
            if (ang >= bestAng) continue;
            bool pick = false;
            if (!p->HasLOSToTarget(a.get(), &pick)) continue;
            best = a.get(); bestAng = ang;
        }
        if (!best) return out;
        out.ok = true;
        out.id = best->GetFormID();
        static const char* kMachine[] = { "Fallout4.esm|02CB73", "Fallout4.esm|0B2BF3", "Fallout4.esm|10C3CE" };   // ActorTypeRobot/Turret/Synth
        for (auto* fk : kMachine) {
            auto* kw = ResolveFormKey(fk) ? ResolveFormKey(fk)->As<RE::BGSKeyword>() : nullptr;
            if (!kw) continue;
            if ((best->race && static_cast<const RE::BGSKeywordForm*>(best->race)->HasKeyword(kw)) ||
                (best->GetNPC() && static_cast<const RE::BGSKeywordForm*>(best->GetNPC())->HasKeyword(kw))) out.machine = true;
        }
        if (auto* info = FindAV("Health")) {
            auto* o = static_cast<RE::ActorValueOwner*>(best);
            float mx = o->GetPermanentActorValue(*info);
            out.hpPct = mx > 0 ? 100.0 * o->GetActorValue(*info) / mx : 100;
        }
        return out;
    }
    bool HackTargetValid(std::uint32_t id, double rangeM) override
    {
        auto* f = RE::TESForm::GetFormByID(id);
        auto* a = f ? f->As<RE::Actor>() : nullptr;
        auto* p = P();
        if (!a || !p || a->lifeState != 0 || !a->Get3D()) return false;
        const auto ap = a->GetPosition(), pp = p->GetPosition();
        const double dx = ap.x - pp.x, dy = ap.y - pp.y, r = rangeM * 70.0;
        return dx * dx + dy * dy <= r * r;
    }
    void HackVisual(const std::string& phase, const std::string& id, std::uint32_t target, double seconds) override { chrome::HackVisual(phase, id, target, seconds); }
    void HackEffect(const std::string& kind, std::uint32_t target, double seconds, double radiusM) override { chrome::HackEffect(kind, target, seconds, radiusM); }
    void AbilityCue(const std::string& handler, const std::string& phase) override { fx::AbilityCue(handler, phase); }
    void TargetAV(std::uint32_t id, const std::string& av, double delta) override
    {
        auto* f = RE::TESForm::GetFormByID(id);
        auto* a = f ? f->As<RE::Actor>() : nullptr;
        auto* info = FindAV(av);
        if (!a || !info || delta == 0) return;
        // < 0: damage (kDamage, as a hit would); > 0: give back exactly what a debuff took. Unverified in game.
        static_cast<RE::ActorValueOwner*>(a)->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *info, (float)delta);
    }
    bool DebugEnabled() override { return g_ini.debug; }
    void Log(const std::string& line) override { REX::INFO("core: {}", line); }

private:
    std::set<std::string> missingLogged_;
} g_env;

std::unique_ptr<cw::Engine> g_engine;
std::unique_ptr<cw::Api> g_api;
bool g_coreOk = false;

// ---------------------------------------------------------------- state lock (docs/STABILITY.md, "Thread map")
// F4SE tasks and BindUIEvent callbacks run one at a time, but the cosave callbacks and the load/save messages arrive from
// the game's own load and save work and rewrite the same engine state, perks, maps and scene pointers. With [General]
// bStateLock=1 both sides take this lock. Every wait is bounded, so it cannot deadlock: a side that times out carries on
// unlocked, which is the behaviour with the lock off, and logs it. Recursive: a console `save` runs inside a task.
std::recursive_timed_mutex g_stateMutex;
class StateLock {
public:
    explicit StateLock(int waitMs = 1000, const char* who = "task")
    {
        if (!g_ini.stateLock) return;
        held_ = waitMs <= 0 ? g_stateMutex.try_lock() : g_stateMutex.try_lock_for(std::chrono::milliseconds(waitMs));
        if (!held_ && waitMs > 0) REX::WARN("lock: {} waited {} ms, continuing unlocked", who, waitMs);
    }
    ~StateLock() { if (held_) g_stateMutex.unlock(); }
    StateLock(const StateLock&) = delete;
    StateLock& operator=(const StateLock&) = delete;
    bool held() const { return held_; }
private:
    bool held_ = false;
};
void PostTask(std::function<void()> f)
{
    F4SE::GetTaskInterface()->AddTask([f = std::move(f)]() { StateLock lock; f(); });
}

// ---------------------------------------------------------------- PrismaUI
PRISMA_UI_API::IVPrismaUI1* g_ui = nullptr;
PRISMA_UI_API::IVPrismaUI4* g_ui4 = nullptr;  // game-thread listener (BindUIEvent) when the installed PrismaUI has it
PrismaView g_view = 0;
bool g_domReady = false;
bool g_setupMode = false;   // the menu is the read-only "your setup" view (OpenSetup), not a ripperdoc's shop
bool g_menuPaused = false;   // RIPPERDOC VOICES: the open menu paused the game (Say and the face would freeze)

void Push(const char* fn, const json& payload)
{
    if (!g_ui || !g_view || !g_ui->IsValid(g_view) || !g_domReady) return;
    std::string js = std::string("if(window.") + fn + ")window." + fn + "(" +
                     payload.dump(-1, ' ', false, json::error_handler_t::replace) + ");";
    g_ui->Invoke(g_view, js.c_str());
}

void CloseMenu()
{
    if (!g_ui || !g_view) return;
    if (voice::Enabled()) voice::Hush();   // the doctor stops mid-line when the talk closes
    scene::OnMenuClosed();                 // INSTALL SCENE: the player stands up, the camera goes back, the visit ends
    // NULL-SAFE (b7 rehearsal crash, Cyberware.dll+0098DA0): with the core not loaded there is no engine and no visit to end;
    // the view is still hidden and the menu state released, so a seam "close" after a failed catalog never crashes.
    const bool hadEngine = cw::EndVisitDoc(g_engine.get());
    g_ui->Unfocus(g_view);
    g_ui->Hide(g_view);
    escguard::SetMenuOpen(false);
    if (hadEngine) REX::INFO("menu: closed");
    else REX::WARN("menu: closed with the core NOT loaded (no engine): the view was hidden, no ripperdoc visit to end");
}

namespace talkface { void Gesture(); }
namespace talkgate { void OnPromptChoice(const std::string& choice); }   // VARGA'S PROMPT, defined in talkgate below

void HandleRequestOnGameThread(std::string req)
{
    if (!g_coreOk) {
        Push("cwOnResponse", json{ { "id", nullptr }, { "ok", false }, { "error", { { "code", "E_INTERNAL" }, { "message", "Cyberware core did not load; see Cyberware.log" } } } });
        return;
    }
    if (cwset::MaybeUiSound(req)) return;   // fire-and-forget view sounds; never reaches the engine
    // {"op":"hudEdit"}: the HUD layout editor (views/CyberwareHUD/layout.js). A plugin op, not a core one: it closes
    // the ripperdoc menu and hands focus to the HUD view.
    if (req.find("\"hudEdit\"") != std::string::npos) {
        json r = json::parse(req, nullptr, false);
        if (r.is_object() && r.value("op", std::string()) == "hudEdit") {
            Push("cwOnResponse", json{ { "id", r.contains("id") ? r["id"] : json(nullptr) }, { "ok", true }, { "op", "hudEdit" }, { "data", json::object() } });
            if (g_ui && g_view && !g_ui->IsHidden(g_view)) { CloseMenu(); Push("cwOnEvent", json{ { "type", "close" } }); }
            hud::BeginEdit("menu");
            return;
        }
    }
    {
        json r = json::parse(req, nullptr, false);
        const std::string op = r.is_object() ? r.value("op", std::string()) : std::string();
        const json rid = r.is_object() && r.contains("id") ? r["id"] : json(nullptr);
        // {"op":"setKey","args":{"key":"setup","vk":119}}: the Settings tab rebinds the setup-view key; saved with the
        // player's other preferences (CyberwarePrefs.ini next to HudLayout.json), never in the mod's ini
        if (op == "setKey") {
            const json a = r.value("args", json::object());
            const int vk = a.value("vk", -1);
            const bool okKey = a.value("key", std::string()) == "setup" && vk >= 0 && vk <= 0xFE && vk != 0x1B && vk != 0x09 && vk != 0x0D;
            const std::string prefs = hudlayout::PrefsIni();
            const bool isEditKey = vk > 0 && g_ini.hudEditKey && (std::uint32_t)vk == g_ini.hudEditKey;   // F2 by default: the HUD layout editor
            if (okKey && !isEditKey && !(g_ini.openKey && (std::uint32_t)vk == g_ini.openKey)) {
                g_ini.setupKey = (std::uint32_t)vk;
                if (!prefs.empty()) WritePrivateProfileStringA("Keys", "iSetupKey", std::format("0x{:X}", vk).c_str(), prefs.c_str());
                REX::INFO("keys: setup view key -> 0x{:X} (saved to {})", vk, prefs.empty() ? "nowhere: no prefs folder" : prefs);
                Push("cwOnResponse", json{ { "id", rid }, { "ok", true }, { "op", op }, { "data", { { "setup", vk } } } });
            } else {
                // printed word for word by the Settings tab (BETA QA): say which rule the key broke
                const std::string why = (vk == 0x1B || vk == 0x09 || vk == 0x0D) ? "Esc, Tab and Enter can't be bound. Pick another key."
                                      : (okKey && g_ini.openKey && (std::uint32_t)vk == g_ini.openKey) ? "That key opens the test shop. Pick another key."
                                      : (okKey && isEditKey) ? "That key opens the HUD layout editor. Pick another key."
                                      : "That key can't open your setup. Pick another key.";
                Push("cwOnResponse", json{ { "id", rid }, { "ok", false }, { "op", op },
                                           { "error", { { "code", "E_BAD_ARGS" }, { "message", why } } } });
            }
            return;
        }
        // the read-only view: anything that changes a piece is refused here, whatever the page sends
        // "setLoadout" is editable in the field view by the lead's ruling (2026-11-10): a deck swap benches hacks
        // away from a doctor, so the player must be able to re-rank there. docs/QUICKHACK-LOADOUT.md 6.5 D - the view's
        // header and MENU/HUD's MN-9 row change with it, since it is no longer strictly read only.
        static const std::set<std::string> kSetupOps = { "hello", "getCatalog", "getState", "getShop", "close", "setCustomize",
                                                         "resetCustomize", "getSettings", "setSettings", "uiSound", "hudEdit",
                                                         "setLoadout" };
        if (g_setupMode && !kSetupOps.count(op)) {
            REX::INFO("api: '{}' refused: the setup view is read only", op);
            Push("cwOnResponse", json{ { "id", rid }, { "ok", false }, { "op", op },
                                       { "error", { { "code", "E_READONLY" }, { "message", "Your setup is read only. A ripperdoc changes your chrome." } } } });
            return;
        }
    }
    if (req.find("\"say\"") != std::string::npos || req.find("\"hush\"") != std::string::npos) {   // RIPPERDOC VOICES ops: plugin-side, not core
        json r = json::parse(req, nullptr, false);
        const std::string op = r.is_object() ? r.value("op", std::string()) : std::string();
        if (op == "say" || op == "hush") {
            const bool paused = g_menuPaused && g_ui && g_view && !g_ui->IsHidden(g_view);
            // CW.Bridge.request(op, args) nests the fields under "args" (app.js); top level kept for seam/manual calls (cw-int-4 build C: every say was no-clip)
            const json& a = r.contains("args") && r["args"].is_object() ? r["args"] : r;
            json out = op == "say" ? voice::Say(a.value("doc", cw::VisitDoc(g_engine.get())), a.value("key", std::string()), paused) : voice::Hush();
            out["id"] = r.contains("id") ? r["id"] : json();
            out["op"] = op;
            if (g_ini.debug) REX::INFO("api: {} -> {}", req.substr(0, 300), out.dump());
            if (g_ui && g_view) g_ui->Invoke(g_view, ("if(window.cwOnResponse)window.cwOnResponse(" + out.dump(-1, ' ', false, json::error_handler_t::replace) + ");").c_str());
            return;
        }
    }
    if (req.find("\"talkPrompt\"") != std::string::npos) {   // VARGA'S PROMPT: the player's answer (talkgate::OnPromptChoice)
        json r = json::parse(req, nullptr, false);
        if (r.is_object() && r.value("op", std::string()) == "talkPrompt") {
            const json& a = r.contains("args") && r["args"].is_object() ? r["args"] : r;
            json out{ { "id", r.contains("id") ? r["id"] : json() }, { "ok", true }, { "op", "talkPrompt" } };
            if (g_ui && g_view) g_ui->Invoke(g_view, ("if(window.cwOnResponse)window.cwOnResponse(" + out.dump(-1, ' ', false, json::error_handler_t::replace) + ");").c_str());
            talkgate::OnPromptChoice(a.value("choice", std::string()));
            return;
        }
    }
    if (req.find("\"talkGesture\"") != std::string::npos) {   // a reply in the talk mode: plugin-side, fire and forget
        json r = json::parse(req, nullptr, false);
        if (r.is_object() && r.value("op", std::string()) == "talkGesture") {
            talkface::Gesture();
            json out{ { "id", r.contains("id") ? r["id"] : json() }, { "ok", true }, { "op", "talkGesture" } };      // the view's request times out without an answer
            if (g_ui && g_view) g_ui->Invoke(g_view, ("if(window.cwOnResponse)window.cwOnResponse(" + out.dump(-1, ' ', false, json::error_handler_t::replace) + ");").c_str());
            return;
        }
    }
    // INSTALL SCENE ops (docs/INSTALL-SCENE.md): plugin-side, never core. {"op":"scene"} is answered with the mode the
    // DLL chose ("chair" | "short" | "fade") and how long it will take, so the view knows what to draw; the beats then
    // arrive as cwOnEvent {type:"scene", phase:...}. {"op":"sceneSkip"} is SCN-10.
    if (req.find("\"scene\"") != std::string::npos || req.find("\"sceneSkip\"") != std::string::npos) {
        json r = json::parse(req, nullptr, false);
        const std::string op = r.is_object() ? r.value("op", std::string()) : std::string();
        if (op == "scene" || op == "sceneSkip") {
            json out;
            if (op == "sceneSkip") { scene::Skip(); out = json{ { "ok", true } }; }
            else {
                json a = r.contains("args") && r["args"].is_object() ? r["args"] : json::object();
                if (!a.contains("doc")) a["doc"] = cw::VisitDoc(g_engine.get());   // the view never has to repeat it
                out = scene::Start(a);
            }
            out["id"] = r.contains("id") ? r["id"] : json();
            out["op"] = op;
            if (g_ini.debug) REX::INFO("api: {} -> {}", req.substr(0, 300), out.dump());
            if (g_ui && g_view) g_ui->Invoke(g_view, ("if(window.cwOnResponse)window.cwOnResponse(" + out.dump(-1, ' ', false, json::error_handler_t::replace) + ");").c_str());
            return;
        }
    }
    std::string res = g_api->Handle(req);
    if (g_ini.debug) REX::INFO("api: {} -> {}", req.substr(0, 300), res.substr(0, 300));
    cwset::AfterOp(req, res);               // setSettings: persist to the settings ini + apply (HUD)
    if (!g_ui || !g_view) return;
    std::string js = "if(window.cwOnResponse)window.cwOnResponse(" + res + ");";
    g_ui->Invoke(g_view, js.c_str());
}

void OnDomReady(PrismaView v)
{
    g_domReady = true;
    REX::INFO("view: DOM ready ({})", g_ui4 ? "BindUIEvent, game thread" : "RegisterJSListener + AddTask");
    if (g_ui4) {
        g_ui4->BindUIEvent(v, "cwRequest", [](const char* arg) { StateLock lock; HandleRequestOnGameThread(arg ? arg : ""); });
        g_ui->Invoke(v, "if(window.cwNativeReady)window.cwNativeReady();");   // the page waits for cwRequest (bridge.js whenReady)
        g_ui4->RegisterConsoleCallback(v, [](PrismaView, PRISMA_UI_API::ConsoleMessageLevel lvl, const char* msg) {
            if (lvl == PRISMA_UI_API::ConsoleMessageLevel::Error) REX::WARN("[js] {}", msg ? msg : "");
            else if (g_ini.debug) REX::INFO("[js] {}", msg ? msg : "");
        });
    } else {
        g_ui->RegisterJSListener(v, "cwRequest", [](const char* arg) {
            std::string req = arg ? arg : "";
            PostTask([req]() { HandleRequestOnGameThread(req); });
        });
    }
}

void CreateView()
{
    if (!g_ui || (g_view && g_ui->IsValid(g_view))) return;
    static bool warned = false;
    std::string file = "Data\\PrismaUI_F4\\views\\" + g_ini.viewPath;
    std::replace(file.begin(), file.end(), '/', '\\');
    if (!fs::exists(file)) {  // the UI stream ships the page; until then the menu stays off instead of loading nothing
        if (!warned) REX::WARN("view: {} does not exist - ripperdoc menu disabled until the UI page is installed", file);
        warned = true;
        return;
    }
    g_domReady = false;
    g_view = g_ui->CreateView(g_ini.viewPath.c_str(), OnDomReady);
    if (g_view) g_ui->Hide(g_view);
    REX::INFO("view: created {} for '{}'", g_view, g_ini.viewPath);
}

// What the ripperdoc's lines can react to beyond the StateView (ripperdoc_talk.py `when.faction`). Read-only.
// Form ids read from Fallout4.esm on the rig, 2026-11-09 (f34rec list FACT).
static json TalkPlayer()
{
    static constexpr struct { const char* id; std::uint32_t form; } kFactions[] = {
        { "bos", 0x0005DE41 }, { "railroad", 0x000994F6 }, { "minutemen", 0x00068043 }, { "institute", 0x0005E558 },
    };
    json factions = json::array();
    if (auto* pc = RE::PlayerCharacter::GetSingleton())
        for (const auto& f : kFactions)
            if (auto* form = RE::TESForm::GetFormByID(f.form); form)
                if (auto* fac = form->As<RE::TESFaction>(); fac && pc->IsInFaction(fac))
                    factions.push_back(f.id);
    json out;
    out["factions"] = factions;
    return out;
}

// The doctor turns to face the player over 0.45 s instead of snapping. Only in the unpaused talk mode: a paused game never ticks.
namespace talkface {
    constexpr float kPi = 3.14159265f;
    constexpr std::uint64_t kTurnMs = 450;
    std::uint32_t ref = 0, talker = 0;      // ref: still turning; talker: the doctor in this conversation
    float from = 0, delta = 0;
    std::chrono::steady_clock::time_point start;
    std::uint64_t lastGesture = 0;
    std::size_t nextGesture = 0;

    // INSTALL SCENE <-> CRASH's talk guard (cw-ab-talk-guard 80bbba9f; the lead's ruling 2026-11-10). The guard ends an
    // unpaused ripperdoc talk when the player is hit or in combat, or when the doctor is more than 350 units away. It
    // is exempt while an install scene runs, from either of two signals: Varga's CW_ClinicSceneActive GLOB and this
    // flag. Only Varga has a global, so at Kessler and the Institute this flag is the whole exemption. cw_scene.cpp
    // drives it through the sceneBusy hook: true at the start of every mode, false in EndRun, at the chair scene's
    // end beat and in OnLoad.
    // This is the one declaration: the talk guard's own copy (80bbba9f) was deleted when the two branches met.
    bool sceneBusy = false;

    void SetSceneBusy(bool on)
    {
        if (sceneBusy == on) return;
        sceneBusy = on;
        REX::INFO("talk: scene busy {} (the talk guard is exempt while this is 1)", on ? 1 : 0);
    }

    // Her talk idles: Fallout4.esm's own standalone gestures, no parent and no conditions in their IDLE records, played like the finishers (PlayIdle).
    // Only dyn_ events: PointForward (dyn_Flavor) played in boot 7 and Shrug, the one standalone idle on a dlg_ event, was refused with her at rest.
    // One per reply at most, in turn so none repeats; a refused one falls through to the next. She stays still in combat.
    struct Gesture_ { std::uint32_t id; const char* name; };
    constexpr Gesture_ kGestures[] = { { 0x001793E3u, "PointForward" }, { 0x00038C7Bu, "HeadShakeYes" }, { 0x001E81ACu, "IdleThinking" } };
    constexpr std::size_t kGestureCount = sizeof(kGestures) / sizeof(kGestures[0]);

    void Gesture()
    {
        if (!g_ini.talkIdles || !talker) return;
        const std::uint64_t now = GetTickCount64();
        if (now - lastGesture < 4500) return;
        auto* form = RE::TESForm::GetFormByID(talker);
        auto* npc = form ? form->As<RE::Actor>() : nullptr;
        if (!npc || !npc->currentProcess || npc->lifeState != 0 || npc->IsInCombat()) return;
        for (std::size_t n = 0; n < kGestureCount; ++n) {
            const std::size_t i = (nextGesture + n) % kGestureCount;
            auto* f = RE::TESForm::GetFormByID(kGestures[i].id);
            auto* idle = f ? f->As<RE::TESIdleForm>() : nullptr;
            if (!idle) continue;
            const bool ok = npc->currentProcess->PlayIdle(*npc, idle, RE::PlayerCharacter::GetSingleton());
            REX::INFO("talk: {} on {:08X}: {}", kGestures[i].name, talker, ok ? "ok" : "refused");
            if (ok) { lastGesture = now; nextGesture = (i + 1) % kGestureCount; return; }
        }
    }

    // STABILITY (the two CRASH consumers of [Menu] bTalkUnpaused, READY-crash-talk-unpaused-1 TU-T2/T3/T4/T7; rows AB-TK1-12): the unpaused
    // talk leaves the world running under a focused view, and nothing but the view itself closed it. A vanilla conversation
    // ends by itself when the player is attacked; so does this one. Game thread only (the pump and PostTask).
    // UNVERIFIED IN GAME: kTalkRange, and whether the player's IsInCombat() lingers after a fight nearby.
    constexpr float kTalkRange = 350.0f;   // units (~5 m): she has walked off, or the player was moved away
    bool guard = false;                    // an unpaused talk view is up
    std::uint32_t guardRef = 0;            // its doctor (0: opened by the seam, no doctor to watch)
    float guardMax = 0;                    // the furthest she got from the player in this talk (row AB-TK10 reads it)
    RE::TESGlobal* sceneGlob = nullptr;    // CLINIC's CW_ClinicSceneActive, looked up once per talk
    // INSTALL SCENE (the lead, 2026-11-10): the guard never ends a talk while the install scene runs. Two signals, either
    // is enough: the scene's own GLOB (1 while it runs; "Cyberware - Clinic.esp|A50", looked up by id once per talk),
    // and sceneBusy above, which cw_scene.cpp drives through SetSceneBusy for every doctor and every scene mode.

    bool SceneRunning() { return sceneBusy || (sceneGlob && sceneGlob->value >= 0.5f); }

    void GuardOn(std::uint32_t refId)
    {
        guard = true; guardRef = refId; guardMax = 0;
        auto* dh = RE::TESDataHandler::GetSingleton();
        const auto id = dh ? dh->LookupFormID(0xA50, "Cyberware - Clinic.esp") : 0;
        auto* form = id ? RE::TESForm::GetFormByID(id) : nullptr;
        sceneGlob = form ? form->As<RE::TESGlobal>() : nullptr;
        REX::INFO("talk: guard on (doctor {:08X}, range {:.0f} units, scene global {})", refId, kTalkRange, sceneGlob ? "found" : "not in this build");
    }

    void GuardOff(const char* why)
    {
        guard = false;
        REX::INFO("talk: guard off ({}; she was at most {:.0f} of {:.0f} units away)", why, guardMax, kTalkRange);
    }

    void End(const char* why)
    {
        if (!guard) return;
        if (!g_ui || !g_view || g_ui->IsHidden(g_view)) { GuardOff("the view was already closed"); return; }
        if (SceneRunning()) return;   // the install scene owns the player and the doctor until it ends
        REX::INFO("talk: ended ({})", why);
        GuardOff(why);
        CloseMenu();
        Push("cwOnEvent", json{ { "type", "close" } });
    }

    void Guard()   // ~30 Hz
    {
        if (!guard) return;
        if (!g_ui || !g_view || g_ui->IsHidden(g_view)) { GuardOff("closed by the view, the seam or a load"); return; }
        if (SceneRunning()) return;
        auto* pc = RE::PlayerCharacter::GetSingleton();
        if (!pc) return;
        if (pc->lifeState != 0) { End("the player is down"); return; }
        if (pc->IsInCombat()) { End("the player is in combat"); return; }
        if (!guardRef) return;
        auto* form = RE::TESForm::GetFormByID(guardRef);
        auto* npc = form ? form->As<RE::Actor>() : nullptr;
        if (!npc || !npc->Get3D()) { End("the doctor is gone"); return; }
        if (npc->lifeState != 0) { End("the doctor is down"); return; }
        if (npc->IsInCombat()) { End("the doctor is in combat"); return; }
        const auto a = npc->GetPosition(), b = pc->GetPosition();
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d > guardMax) guardMax = d;
        if (d > kTalkRange) End("the doctor is out of range");
    }

    void Begin(std::uint32_t refId)
    {
        ref = 0;
        talker = refId;
        auto* pc = RE::PlayerCharacter::GetSingleton();
        auto* form = refId ? RE::TESForm::GetFormByID(refId) : nullptr;
        auto* npc = form ? form->As<RE::Actor>() : nullptr;
        if (!pc || !npc) return;
        const auto a = npc->GetPosition(), b = pc->GetPosition();
        float d = std::atan2(b.x - a.x, b.y - a.y) - npc->GetHeading();
        while (d > kPi) d -= 2 * kPi;
        while (d < -kPi) d += 2 * kPi;
        from = npc->GetHeading();
        delta = d;
        start = std::chrono::steady_clock::now();
        ref = refId;
    }

    void Tick()
    {
        if (!ref) return;
        auto* form = RE::TESForm::GetFormByID(ref);
        auto* npc = form ? form->As<RE::Actor>() : nullptr;
        if (!npc) { ref = 0; return; }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        float t = std::min(1.0f, static_cast<float>(ms) / kTurnMs);
        t = t * t * (3 - 2 * t);
        npc->SetHeading(from + delta * t);
        if (t >= 1.0f) ref = 0;
    }
}

// via: "hotkey" | "chair" | "talk". "talk" makes the view greet first (the doc's line + "Let's see your chrome").
void OpenMenu(const std::string& doc, const char* via = "hotkey", std::uint32_t talkRef = 0)
{
    if (!g_coreOk) { REX::ERROR("menu: core not loaded, cannot open"); return; }
    if (!g_engine->catalog().ripperdocs.count(doc)) { REX::ERROR("menu: unknown ripperdoc '{}'", doc); return; }
    if (g_ini.talkUnpaused && std::strcmp(via, "talk") == 0)   // an unpaused talk would end at once (talkface::Guard): do not flash it
        if (auto* pc = RE::PlayerCharacter::GetSingleton(); pc && pc->IsInCombat()) { REX::INFO("talk: not opened (the player is in combat)"); return; }
    g_engine->RedeemChips();
    g_engine->RedeemCapacityItems();
    g_engine->currentRipperdoc = doc;
    CreateView();
    if (!g_ui || !g_view) { REX::ERROR("menu: no PrismaUI view (is PrismaUI F4 installed and the view file deployed?)"); return; }
    g_setupMode = false;
    g_ui->Show(g_view);
    const bool talkMode = g_ini.talkUnpaused && std::strcmp(via, "talk") == 0;
    g_ui->Focus(g_view, g_ini.pauseGame && !talkMode);
    g_menuPaused = g_ini.pauseGame && !talkMode;
    if (talkMode) { talkface::Begin(talkRef); talkface::GuardOn(talkRef); }
    else talkface::guard = false;   // a paused shop is never watched, even one opened in the frame a talk closed
    if (g_ini.escGuard) escguard::SetMenuOpen(true);   // Esc must not also open the game's Pause menu under ours
    json ev = g_api->Event("open");
    ev["via"] = via;
    ev["player"] = TalkPlayer();
    ev["mode"] = "shop";
    ev["keys"] = { { "setup", g_ini.setupKey } };
    ev["talkMode"] = talkMode;
    Push("cwOnEvent", ev);
    REX::INFO("menu: opened for ripperdoc '{}' (via {})", doc, via);
    REX::INFO("menu: talk mode {}, game {}", talkMode ? "on" : "off", g_menuPaused ? "paused" : "running");
}

// The read-only "your setup" view (the author, 2026-11-08): installed chrome, stats, capacity, Customize, Settings. No
// ripperdoc is set, the page shows no shop, and HandleRequestOnGameThread refuses every op that changes chrome.
void OpenSetup(const char* via)
{
    if (!g_coreOk) { REX::ERROR("menu: core not loaded, cannot open"); return; }
    g_engine->currentRipperdoc.clear();
    CreateView();
    if (!g_ui || !g_view) { REX::ERROR("menu: no PrismaUI view (is PrismaUI F4 installed and the view file deployed?)"); return; }
    g_setupMode = true;
    talkface::guard = false;
    g_ui->Show(g_view);
    g_ui->Focus(g_view, g_ini.pauseGame);
    if (g_ini.escGuard) escguard::SetMenuOpen(true);
    json ev = g_api->Event("open");
    ev["via"] = via;
    ev["mode"] = "setup";
    ev["keys"] = { { "setup", g_ini.setupKey } };
    Push("cwOnEvent", ev);
    REX::INFO("menu: opened read-only setup view (via {})", via);
}

// ---------------------------------------------------------------- talkgate: the dialogue decides, not the activation
// VARGA TALK 2026-11-12 (docs/VARGA-TALK.md), from the author's words after playing beta E: "the popup has dialogue, with the main
// menu behind it, just show dailoague ... and hides the menu till you see let me see cyberwear". Activating a ripperdoc used to
// open the view straight away while the engine started its own conversation, so both were on screen at once.
//
// Now: the activation opens NOTHING. The ripperdoc's own dialogue (quest CW_RipperdocDialogue, a top-level branch built by
// espgen with --ripperdoc-dialogue-guard) greets the player and offers "Let me see your cyberware." / "Not right now.". This
// watches for the SHOP choice having been played and opens the view only after the dialogue UI is gone.
//
// Two independent signals, because one rig turn is expensive and neither can be proved off the rig:
//   subtitle - the shop INFO is in the SubtitleManager. espgen sets Force Subtitle (ENAM 0x0200) on every one of our INFOs, so
//              the line is displayed even with the player's dialogue subtitles off, which is also why it is readable at all
//              while the lines are text only.
//   said     - TESTopicInfo::saidOnce on that INFO. Cleared on every activation and after acting, so only a choice made in
//              THIS conversation counts and talking again works the same way.
// The log says which one fired, so the first capture settles which to keep.
//
// With no talk in the build (the play build generates no dialogue records at all) this stays disarmed and the activation opens
// the view directly, exactly as before.
const char* OpenGameplayMenu(bool includeMessageBox);   // defined with the HUD below; talkgate's prompt reads it
namespace talkgate {
    struct State {
        std::uint32_t infoShop = 0, infoBye = 0, quest = 0;   // local ids; infoShop == 0 means no talk in this build
        std::string pendingDoc;
        std::uint32_t pendingRef = 0;
        std::uint64_t pendingSince = 0;
        bool fired = false;
        const char* firedBy = "";
        bool asked = false;      // we have asked the DialogueMenu to close
    } st;
    constexpr std::uint64_t kPendingMs = 120000;   // a conversation nobody finishes stops waiting

    void BranchFlags();   // defined below Init, which calls it

    bool Armed() { return st.infoShop != 0; }

    void Init()
    {
        auto id = [](const char* e) -> std::uint32_t {
            auto it = g_map.ids.find(e);
            return it == g_map.ids.end() ? 0u : it->second;
        };
        st = State{};
        st.infoShop = id("CW_RipperdocInfoShop");
        st.infoBye = id("CW_RipperdocInfoBye");
        st.quest = id("CW_RipperdocDialogue");
        if (Armed())
            REX::INFO("talk: ripperdoc dialogue IS in this build (quest {:03X}, shop info {:03X}, bye info {:03X}): activating a doc opens no menu; the view opens on the shop line",
                      st.quest, st.infoShop, st.infoBye);
        else
            REX::INFO("talk: no ripperdoc dialogue in this build (formmap has no CW_RipperdocInfoShop): activating a doc opens the view directly, as before");
        BranchFlags();
    }

    // [Menu] iTalkBranchFlags: the ONE in-game unknown of this work, switchable without a rebuild (the lead,
    // 2026-11-12). The record ships DNAM 0x01 (Top-Level), the shape 129 of Fallout4.esm's 132 branches use. If a capture
    // shows F34's own topics still in the player's option list, the variant to try is 3 (Top-Level|Blocking) - set the key,
    // relaunch, no rebuild. BGSDialogueBranch::flags is a plain uint32 on the loaded form, so this writes the same value the
    // record would have carried. -1 (the default) leaves the record alone and writes nothing.
    void BranchFlags()
    {
        if (!st.quest) return;
        auto* dh = RE::TESDataHandler::GetSingleton();
        auto it = g_map.ids.find("CW_RipperdocBlockBranch");
        auto* br = (dh && it != g_map.ids.end()) ? dh->LookupForm<RE::BGSDialogueBranch>(it->second, g_map.plugin) : nullptr;
        if (!br) { REX::WARN("talk: CW_RipperdocBlockBranch did not resolve; [Menu] iTalkBranchFlags={} not applied", g_ini.talkBranchFlags); return; }
        const std::uint32_t was = br->flags;
        // -1 leaves the record alone, and SAYS SO. It is logged rather than silent because -1 is the value the beta
        // ships for variant A (it is the play value of the gated key), so the run must be able to prove it ran that
        // exact path and not a write of 0x1 over 0x1.
        if (g_ini.talkBranchFlags < 0) {
            REX::INFO("talk: branch DLBR flags 0x{:X} left as the record's own by [Menu] iTalkBranchFlags={} = VARIANT A (0x1 top-level, 0x2 blocking)",
                      was, g_ini.talkBranchFlags);
            return;
        }
        br->flags = (std::uint32_t)g_ini.talkBranchFlags;
        const std::uint32_t now = br->flags;   // READ BACK: a write the engine ignores must never read as applied
        if (now != (std::uint32_t)g_ini.talkBranchFlags)
            REX::ERROR("talk: branch DLBR flags WRITE DID NOT STICK: wrote 0x{:X} by [Menu] iTalkBranchFlags={}, reads back 0x{:X}. "
                       "The variant on screen is NOT the one asked for - do not grade a variant row from this launch",
                       (std::uint32_t)g_ini.talkBranchFlags, g_ini.talkBranchFlags, now);
        else
            REX::INFO("talk: branch DLBR flags 0x{:X} -> 0x{:X} by [Menu] iTalkBranchFlags={} (read back 0x{:X}; 0x1 top-level, 0x2 blocking)",
                      was, now, g_ini.talkBranchFlags, now);
    }

    RE::TESTopicInfo* Info(std::uint32_t local)
    {
        auto* dh = RE::TESDataHandler::GetSingleton();
        return (dh && local) ? dh->LookupForm<RE::TESTopicInfo>(local, g_map.plugin) : nullptr;
    }

    bool SubtitleShowing(std::uint32_t local)
    {
        if (!local) return false;
        auto* sm = RE::SubtitleManager::GetSingleton();
        if (!sm) return false;
        RE::BSAutoReadLock lock{ RE::SubtitleManager::GetRWLock() };
        for (auto& s : sm->subtitlePriorityArray)
            if (s.topicInfo && (s.topicInfo->GetFormID() & 0x00FFFFFF) == local) return true;
        return false;
    }

    // saidOnce is a plain runtime bool on the form. Reading and clearing it is how "was this line just played" is asked
    // without a topic-info event binding (CommonLibF4 a08c731 declares no TESTopicInfoEvent).
    bool SaidAndClear(std::uint32_t local)
    {
        auto* info = Info(local);
        if (!info || !info->saidOnce) return false;
        info->saidOnce = false;
        return true;
    }

    bool DialogueUp()
    {
        auto* ui = RE::UI::GetSingleton();
        return ui && ui->GetMenuOpen(RE::BSFixedString("DialogueMenu"));
    }

    // The engine's own conversation decides (the original VARGA TALK path). Used when a real DialogueMenu is up, so a
    // build that does carry a greeting topic keeps working exactly as before.
    void WatchDialogue(const std::string& doc, std::uint32_t refId)
    {
        st.pendingDoc = doc;
        st.pendingRef = refId;
        st.pendingSince = GetTickCount64();
        st.fired = false;
        st.firedBy = "";
        st.asked = false;
        SaidAndClear(st.infoShop);   // a stale "said" from an earlier talk must not count as this one's choice
        SaidAndClear(st.infoBye);
        REX::INFO("talk: ripperdoc '{}' ref {:08X} activated; no menu opened, waiting for a dialogue choice", doc, refId);
    }

    // ---- VARGA'S PROMPT (Builder 3, 2026-11-13; RUN-SHEET 8f VP1-VP9). The esp ships no greeting topic, so on 3f8e37b6 the
    // activation starts no conversation at all (3 of 3). Until the greeting topic ships, the DLL shows its OWN prompt in the
    // PrismaUI view: her greeting (the catalog's ripperdocs[].talk, picked by the view per doc) and two choices. The decision
    // is core/cw_talkprompt.h, which cwtest drives; this only reads the game into it and does what it answers.
    // It is a PrismaUI layer, so it MISSES beat 1 of VARGA-TALK.md section 3 by design (stated there).
    cw::talkprompt::Prompt prompt;

    bool ClinicSceneUp()
    {
        if (talkface::sceneBusy) return true;
        auto* dh = RE::TESDataHandler::GetSingleton();
        const auto id = dh ? dh->LookupFormID(0xA50, "Cyberware - Clinic.esp") : 0;   // CW_ClinicSceneActive, as talkface::GuardOn
        auto* form = id ? RE::TESForm::GetFormByID(id) : nullptr;
        auto* g = form ? form->As<RE::TESGlobal>() : nullptr;
        return g && g->value >= 0.5f;
    }

    cw::talkprompt::World PromptWorld()
    {
        cw::talkprompt::World w;
        w.armed = Armed();
        auto* ui = RE::UI::GetSingleton();
        const char* m = OpenGameplayMenu(false);
        w.menuOpen = (g_ui && g_view && !g_ui->IsHidden(g_view)) || (m && std::strcmp(m, "DialogueMenu") != 0);
        w.sceneRunning = ClinicSceneUp() || scene::Active();
        auto* pc = RE::PlayerCharacter::GetSingleton();
        w.playerInCombat = pc && pc->IsInCombat();
        w.dialogueUp = DialogueUp();
        w.otherPromptUp = ui && ui->GetMenuOpen(RE::BSFixedString("MessageBoxMenu"));
        return w;
    }

    cw::talkprompt::Target PromptTarget(std::uint32_t refId)
    {
        cw::talkprompt::Target t;
        auto* form = refId ? RE::TESForm::GetFormByID(refId) : nullptr;
        auto* npc = form ? form->As<RE::Actor>() : nullptr;
        auto* pc = RE::PlayerCharacter::GetSingleton();
        t.present = npc && pc && npc->Get3D() && !npc->IsDisabled();
        if (!t.present) return t;
        t.alive = npc->lifeState == 0;
        t.inCombat = npc->IsInCombat();
        const auto a = npc->GetPosition(), b = pc->GetPosition();
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        t.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        return t;
    }

    // Hides the prompt only. NOT CloseMenu: no shop visit was started, so there is none to end, and no scene to stop.
    void HidePrompt()
    {
        if (!g_ui || !g_view) return;
        g_ui->Unfocus(g_view);
        g_ui->Hide(g_view);
        escguard::SetMenuOpen(false);
        Push("cwOnEvent", json{ { "type", "close" } });
    }

    void ShowPrompt(const std::string& doc, std::uint32_t refId)
    {
        CreateView();
        if (!g_ui || !g_view || !g_domReady) {
            prompt.Reset();
            REX::ERROR("talk: prompt for '{}' not shown: the PrismaUI view is not ready (is PrismaUI F4 installed and the view deployed?)", doc);
            return;
        }
        g_setupMode = false;
        talkface::guard = false;           // the talk guard watches the unpaused SHOP; this prompt watches itself (Tick)
        talkface::Begin(refId);            // she turns to the player, as in the talk mode
        g_ui->Show(g_view);
        g_ui->Focus(g_view, false);        // a conversation: the world keeps running, so combat or her leaving closes it
        g_menuPaused = false;
        if (g_ini.escGuard) escguard::SetMenuOpen(true);   // Esc answers "Not right now.", never the game's Pause menu
        json ev = g_api->Event("state");
        ev["type"] = "talkPrompt";
        ev["doc"] = doc;
        ev["player"] = TalkPlayer();
        Push("cwOnEvent", ev);
        prompt.Shown(GetTickCount64());   // the grace starts HERE: PrismaUI is not visible yet (softlock 2026-11-13)
        REX::INFO("talk: prompt shown for '{}' ref {:08X} (greeting + 'Let me see your cyberware.' / 'Not right now.')", doc, refId);
    }

    // THE ONLY WAY A PROMPT EVER CLOSES (softlock 2026-11-13). It hides, unfocuses, releases the escguard and
    // pushes close in ONE step, so the state and the view can never be left disagreeing. The old code had a path
    // that cleared the state alone (prompt.Reset()) and left the panel drawn, focused and guarding: that is the
    // softlock. Nothing may close this prompt except this function and Choose's own Act.
    void ClosePrompt(const char* why)
    {
        REX::INFO("talk: prompt for '{}' closed ({}); nothing opened", prompt.Doc(), why ? why : "?");
        HidePrompt();
    }

    // Activating a ripperdoc NPC with the gate armed (ActivateSink, via PostTask: game thread).
    void OnActivate(const std::string& doc, std::uint32_t refId)
    {
        using cw::talkprompt::Refuse;
        const auto r = prompt.Activate(doc, refId, PromptWorld());
        if (r == Refuse::None) { ShowPrompt(doc, refId); return; }
        if (r == Refuse::AlreadyOpen) { REX::INFO("talk: '{}' activated again while the prompt is open: ignored", doc); return; }
        if (r == Refuse::Dialogue) {   // the engine started a real conversation: the dialogue choice decides, as before
            REX::INFO("talk: '{}' activated with a DialogueMenu up: no prompt, watching the dialogue", doc);
            WatchDialogue(doc, refId);
            return;
        }
        REX::INFO("talk: '{}' activated; prompt NOT shown ({}), nothing opened", doc, cw::talkprompt::RefuseWord(r));
    }

    // {"op":"talkPrompt","args":{"choice":"shop"|"decline"}} from the view.
    void OnPromptChoice(const std::string& choice)
    {
        using cw::talkprompt::Act;
        const bool wasOpen = prompt.Open();
        const std::string doc = prompt.Doc();
        const std::uint32_t ref = prompt.Ref();
        const bool viewVisible = g_ui && g_view && !g_ui->IsHidden(g_view);
        const Act act = prompt.Choose(choice == "shop" ? cw::talkprompt::Choice::Shop : cw::talkprompt::Choice::Decline, viewVisible);
        if (act == Act::None) { REX::INFO("talk: prompt answer '{}' ignored (no prompt open and no panel up)", choice); return; }
        if (act == Act::OpenMenu) {
            REX::INFO("talk: prompt: the player chose 'Let me see your cyberware.'; opening the ripperdoc view for '{}' (via prompt, ref {:08X})", doc, ref);
            OpenMenu(doc, "prompt", ref);
            return;
        }
        // Act::Close. Two ways in, and BOTH must dismiss the panel: the player declined, or an answer arrived with no
        // state while the panel was still up. The second used to be logged "ignored" and returned WITHOUT hiding -
        // that is what made Esc and Enter dead for 18 s (softlock 2026-11-13). Esc always works now.
        if (wasOpen) REX::INFO("talk: prompt: the player chose 'Not right now.' for '{}'; nothing opened", doc);
        else REX::WARN("talk: prompt answer '{}' arrived with no prompt open but the panel VISIBLE: dismissing the panel anyway", choice);
        HidePrompt();
    }

    // Releases the escguard WITHOUT consulting the prompt's state (the lead's requirement, 06:50Z). The guard may
    // never outlive the thing it guards: if no view of ours is on screen and the guard has been held past a grace
    // longer than any show takes, it is released. This is the backstop that makes a future state/view desync
    // survivable rather than a softlock - it would have freed the player on 106f23cc even with the bug intact.
    void GuardReconcile()
    {
        // ONCE PER EPISODE, NOT ONCE PER TICK (rig operator, 2026-11-13: 4,005 copies of this warning in one
        // boot, ~35 inside 250 ms from six thread ids, 46.8 per second = one per frame). The noise was the
        // smaller half: a per-tick release BUMPED THE ESCGUARD'S RESTORE GENERATION every 33 ms inside its own
        // 350 ms delay, so it starved the restore it was asking for and the guard stayed held 28-57 s. The spam
        // count WAS the loop counter. Both halves are fixed: the close is idempotent now
        // (core/cw_guardrestore.h), and this fires once and releases immediately instead of scheduling.
        static bool released = false;
        if (!g_ini.escGuard || !escguard::MenuOpen()) { released = false; return; }
        if (escguard::ReleasePending()) return;   // a release is already on its way: asking again is what starved it
        // SAFE UNDER AN OPEN SHOP MENU ONLY BECAUSE THE PROMPT AND THE SHOP SHARE ONE VIEW (release review, 2026-11-13):
        // OpenMenu shows g_view (:1327, :1355) and so does ShowPrompt (:1569), so this single read covers every panel
        // of ours that can be on screen. IF THE PROMPT EVER GETS ITS OWN VIEW, this must check every view we own, or
        // the reconciler will release the guard out from under a menu that is still up.
        const bool viewUp = g_ui && g_view && !g_ui->IsHidden(g_view);
        if (viewUp) { released = false; return; }
        constexpr std::uint64_t kGuardGraceMs = 2500;   // > Prompt::kShowGraceMs, so a pending show is never cut off
        if (escguard::HeldForMs() < kGuardGraceMs) return;
        if (released) return;   // said and done once for this episode
        released = true;
        const std::uint64_t heldMs = escguard::HeldForMs();
        REX::WARN("escguard: held {} ms with no view of ours on screen: releasing it (nothing may hold the player's input with no menu up)", heldMs);
        // IMMEDIATE, not deferred: there is no key press to let past, so the 350 ms delay buys nothing and the
        // deferred route is what livelocked. One line, one release, no worker thread.
        escguard::ReleaseNow("the guard outlived every view of ours");
    }

    void PromptTick()
    {
        GuardReconcile();   // first, and unconditionally: it must run even when no prompt is open
        if (!prompt.Open()) return;
        const bool viewVisible = g_ui && g_view && !g_ui->IsHidden(g_view);
        // THE SOFTLOCK FIX. This used to be `if (IsHidden) { Reset(); return; }`, which fired on the first tick after
        // Show (PrismaUI is not visible yet) and cleared the state ONLY, leaving the panel drawn, focused and
        // guarding. Observe applies the confirmed-visible latch and the never-shown grace, and every close it
        // returns goes through ClosePrompt - never a bare Reset.
        if (const char* why = prompt.Observe(viewVisible, GetTickCount64())) { ClosePrompt(why); return; }
        const auto w = PromptWorld();
        if (const char* why = prompt.Watch(w, PromptTarget(prompt.Ref()))) {
            const std::string doc = prompt.Doc();
            const std::uint32_t ref = prompt.Ref();
            ClosePrompt(why);
            if (w.dialogueUp) WatchDialogue(doc, ref);   // a real conversation took over: it decides
        }
    }

    void Drop(const char* why)
    {
        REX::INFO("talk: '{}' ended with no menu ({})", st.pendingDoc, why);
        st.pendingDoc.clear();
        st.pendingRef = 0;
        st.fired = false;
        st.asked = false;
    }

    void Tick()
    {
        PromptTick();
        if (!Armed() || st.pendingDoc.empty()) return;
        if (!st.fired) {
            if (SubtitleShowing(st.infoShop)) { st.fired = true; st.firedBy = "subtitle"; }
            else if (SaidAndClear(st.infoShop)) { st.fired = true; st.firedBy = "saidOnce"; }
            else if (SubtitleShowing(st.infoBye) || SaidAndClear(st.infoBye)) { Drop("the player declined the shop line"); return; }
            else if (GetTickCount64() - st.pendingSince > kPendingMs) { Drop("no choice was made"); return; }
            if (!st.fired) return;
            SaidAndClear(st.infoShop);   // so the NEXT talk starts clean whichever signal fired
            REX::INFO("talk: the player picked the shop line (seen by {}); closing the dialogue before the view opens", st.firedBy);
            return;   // never in the same frame the choice is seen: the dialogue is still up
        }
        // The dialogue UI must be CLOSED before or as the view opens - the two on screen together is the defect being fixed.
        // Asking the menu to hide (as cw_escguard does) means this holds whether or not the records end the conversation.
        if (DialogueUp()) {
            if (!st.asked) {
                st.asked = true;
                if (auto* q = RE::UIMessageQueue::GetSingleton()) {
                    q->AddMessage(RE::BSFixedString("DialogueMenu"), RE::UI_MESSAGE_TYPE::kHide);
                    REX::INFO("talk: asked DialogueMenu to hide");
                }
            }
            return;
        }
        const std::string doc = st.pendingDoc;
        const std::uint32_t ref = st.pendingRef;
        st.pendingDoc.clear();
        st.pendingRef = 0;
        st.fired = false;
        st.asked = false;
        REX::INFO("talk: dialogue closed; opening the ripperdoc view for '{}' (via talk, ref {:08X})", doc, ref);
        OpenMenu(doc, "talk", ref);
    }
}

// ---------------------------------------------------------------- activation + hotkey
class ActivateSink final : public RE::BSTEventSink<RE::TESActivateEvent> {
public:
    RE::BSEventNotifyControl ProcessEvent(const RE::TESActivateEvent& e, RE::BSTEventSource<RE::TESActivateEvent>*) override
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!e.objectActivated || !e.actionRef || e.actionRef.get() != player) return RE::BSEventNotifyControl::kContinue;
        auto* base = e.objectActivated->GetObjectReference();
        if (!base) return RE::BSEventNotifyControl::kContinue;
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh) return RE::BSEventNotifyControl::kContinue;
        std::string d;
        const auto refId = e.objectActivated->GetFormID();
        for (auto& c : g_map.chairRefs)                      // the placed chair itself (the vanilla 019966 in Diamond City)
            if (dh->LookupFormID(c.local, c.plugin) == refId) { d = c.doc; break; }
        if (d.empty())
            for (auto& [local, doc] : g_map.chairToDoc)      // any ref of a Cyberware chair base
                if (dh->LookupFormID(local, g_map.plugin) == base->GetFormID()) { d = doc; break; }
        if (!d.empty()) {
            REX::INFO("activate: ripperdoc chair {:08X} -> '{}'", refId, d);
            PostTask([d]() { OpenMenu(d, "chair"); });
            return RE::BSEventNotifyControl::kContinue;
        }
        // talking to a placed ripperdoc (the NPC has no vanilla dialogue): the view opens on the doc's greeting
        for (auto& [local, doc] : g_map.npcToDoc)
            if (dh->LookupFormID(local, g_map.plugin) == base->GetFormID()) { d = doc; break; }
        if (!d.empty()) {
            REX::INFO("activate: ripperdoc NPC {:08X} -> '{}' (talk)", refId, d);
            voice::SetSpeaker(e.objectActivated.get(), d);
            if (talkgate::Armed()) {
                // VARGA'S PROMPT (docs/VARGA-TALK.md): talkgate::OnActivate shows the DLL's own two-choice prompt; the menu
                // opens only on "Let me see your cyberware.". If the engine has a real DialogueMenu up instead, the old
                // dialogue watch decides, and the view opens only after the shop line AND the dialogue UI is gone.
                PostTask([d, refId]() { talkgate::OnActivate(d, refId); });
            } else {
                PostTask([d, refId]() {
                    if (auto* ui = RE::UI::GetSingleton(); ui && ui->GetMenuOpen(RE::BSFixedString("DialogueMenu")))
                        REX::WARN("activate: a vanilla DialogueMenu is open over the ripperdoc talk");
                    OpenMenu(d, "talk", refId);
                });
            }
        }
        return RE::BSEventNotifyControl::kContinue;
    }
} g_activateSink;

// ---------------------------------------------------------------- legs (native family "jump")
// le_reinforced_tendons: a second jump in mid-air (params.jumps). le_leeroy: one air dash per jump along the view
// heading (dash_m over 0.2 s). le_fortified_ankles: holding Jump for 0.3 s during the rise boosts it to jump_mult_max x
// the height (velocity x sqrt(mult)); its fall-damage cut is a perk entry. All through the player's character
// controller velocity (havok units, ~1 m). UNVERIFIED IN GAME: whether the controller keeps a velocity set mid-air.
namespace legs {
    constexpr float kJumpV = 4.6f;   // assumed vanilla take-off speed when none was measured (~1.1 m jump)
    struct State {
        bool air = false, dashed = false, charged = false, held = false;
        int extra = 0;
        float v0 = 0, heldSecs = 0, dx = 0, dy = 0, dashSpeed = 0;
        std::uint64_t airSince = 0, dashUntil = 0;
    } st;
    std::atomic<bool> pendingPress{ false };

    const cw::NativeAbility* Find(const char* h)
    {
        for (auto& [id, n] : g_activeNative) if (n.handler == h) return &n;
        return nullptr;
    }
    double Num(const cw::NativeAbility* n, const char* k, double d)
    {
        return n && n->params.is_object() && n->params.contains(k) && n->params[k].is_number() ? n->params[k].get<double>() : d;
    }
    void OnJumpButton(bool pressed, bool justPressed, float heldSecs)
    {
        st.held = pressed;
        st.heldSecs = heldSecs;
        if (justPressed) pendingPress = true;
    }
    void Frame()
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        const cw::NativeAbility* tend = Find("le_reinforced_tendons");
        const cw::NativeAbility* leer = Find("le_leeroy");
        const cw::NativeAbility* ank = Find("le_fortified_ankles");
        if (!p || (!tend && !leer && !ank)) { pendingPress = false; return; }
        auto* mh = p->currentProcess ? p->currentProcess->middleHigh : nullptr;
        RE::bhkCharacterController* cc = mh ? mh->charController.get() : nullptr;
        if (!cc) return;
        const std::uint64_t now = GetTickCount64();
        const bool air = p->IsJumping() || cc->fallTime > 0.1f;
        RE::hkVector4f v;
        cc->GetLinearVelocityImpl(v);
        if (air && !st.air) { st.airSince = now; st.extra = 0; st.dashed = false; st.charged = false; st.v0 = std::max(v.z, 0.0f); }
        if (!air) st.dashUntil = 0;
        st.air = air;
        if (air && now - st.airSince < 150) st.v0 = std::max(st.v0, v.z);
        const bool press = pendingPress.exchange(false);
        bool set = false;
        if (press && air && now - st.airSince > 120) {   // a press after take-off, not the take-off press itself
            int extraMax = std::max(1, (int)std::lround(Num(tend, "jumps", 2)) - 1);
            if (tend && st.extra < extraMax) {
                v.z = std::max(st.v0, kJumpV);
                ++st.extra; set = true;
                REX::INFO("legs: double jump ({}/{}), vz {:.2f}", st.extra, extraMax, v.z);
            } else if (leer && !st.dashed) {
                float h = p->GetHeading();
                st.dx = std::sin(h); st.dy = std::cos(h);
                st.dashSpeed = (float)(Num(leer, "dash_m", 5) / 0.2);
                st.dashed = true; st.dashUntil = now + 200;
                REX::INFO("legs: air dash {:.1f} m", Num(leer, "dash_m", 5));
            }
        }
        if (st.dashUntil > now) { v.x = st.dx * st.dashSpeed; v.y = st.dy * st.dashSpeed; set = true; }
        if (ank && air && st.held && !st.charged && st.heldSecs >= 0.3f && now - st.airSince < 500 && v.z > 0) {
            double m = std::max(1.0, Num(ank, "jump_mult_max", 3));
            v.z = (float)(std::max(st.v0, kJumpV) * std::sqrt(m));
            st.charged = true; set = true;
            REX::INFO("legs: charged jump x{:.2f}, vz {:.2f}", m, v.z);
        }
        if (set) cc->SetLinearVelocityImpl(v);
    }
}  // namespace legs

namespace optics { void SetZoom(bool on); void TrapScan(); extern bool hudView; extern bool zoomed; }
namespace fx { void OnScanKey(); void SweepFrame(); }
namespace chrome { bool OnHackKey(); void OnNoTarget(); void OnQuickhack(std::uint32_t target); void RevertAll(); void HackVisual(const std::string& phase, const std::string& id, std::uint32_t target, double seconds);
                   void Suspend(); void Resume(); json SaveJson(); int Restore(const json& j); }
namespace hud { void Event(const json& e); void BeginEdit(const char* via); void ToggleEdit(); bool Visible(); }
namespace arms { void Pacify(); void OnAttackPressed(); void TierComputeReport(); }   // OnAttackPressed: the slam re-pick's attack buffer
void ApplySettingsToGame(const cw::Settings& s);   // defined after namespace hud (settings_plugin's callback)
// HUD text lines (ShowHUDMessage): the fallback while the CyberwareHUD view is not up, or always with [HUD] bMessages=1
inline bool HudText() { return !optics::hudView || g_ini.hudMessages; }

// Any full-screen or modal menu the vanilla HUD also gives way to. While one is open the HUD hides and the ability keys
// do nothing: Home / End / Delete edit the console line and ] \ are typed characters, so without this typing in the
// console fired Sandevistan, camo, a quickhack or BISF (VISUAL POLISH 2026-11-08). A message box blocks the keys (it
// takes the input) but not the HUD (it sits over gameplay). Game thread only.
const char* OpenGameplayMenu(bool includeMessageBox = true)   // the first listed menu that is open, or nullptr
{
    auto* ui = RE::UI::GetSingleton();
    if (!ui) return nullptr;
    for (const char* m : { "PipboyMenu", "DialogueMenu", "PauseMenu", "LoadingMenu", "BarterMenu", "ContainerMenu",
                           "WorkshopMenu", "MainMenu", "Console", "TerminalMenu", "TerminalHolotapeMenu", "ExamineMenu",
                           "CookingMenu", "PowerArmorModMenu", "LockpickingMenu", "LevelUpMenu", "SPECIALMenu", "BookMenu",
                           "SleepWaitMenu", "LooksMenu", "VATSMenu", "CreditsMenu" })
        if (ui->GetMenuOpen(RE::BSFixedString(m))) return m;
    return includeMessageBox && ui->GetMenuOpen(RE::BSFixedString("MessageBoxMenu")) ? "MessageBoxMenu" : nullptr;
}

bool GameplayMenuOpen(bool includeMessageBox = true) { return OpenGameplayMenu(includeMessageBox) != nullptr; }

// D1 / QD N7: held notices wait for the screen to be the player's again. Declared in GameEnv above; defined here
// because it needs GameplayMenuOpen and the PrismaUI view globals, both of which come after that class.
bool GameEnv::UiBusy()
{
    if (GameplayMenuOpen()) return true;
    if (g_ui && g_view && !g_ui->IsHidden(g_view)) return true;   // the ripperdoc view, the install scene included
    return false;
}

// a native ability key (game thread): the hotkey sink and the test seam both come here
void FireNativeKey(const std::string& a)
{
    if (!g_coreOk) return;
    if (a == "skippy") { smartgame::CycleSkippyMode(); return; }   // not an engine ability: no cooldown, no cost
    if (a == "hack" && chrome::OnHackKey()) return;   // aimed at a machine with an uplink installed: not a quickhack
    cw::json r = g_engine->NativeKey(a);
    REX::INFO("native key {}: {}", a, r.dump());
    if (r.value("ok", false)) {
        if (a == "pacify") arms::Pacify();          // BISF: the engine ran the cooldown, the plugin stops the fight
        else if (a == "scan") optics::TrapScan();   // Doomsayer / Oracle: mines, hazards and trap triggers too
        else if (a == "os" || a == "heal") {        // Sandevistan / Berserk / Blood Pump: a one-shot glow pulse (colours: tint.cpp, HUD accent.js)
            if (tint::Pulse(a == "os" ? 1.0f : 0.7f)) hud::Event(json{ { "type", "pulse" }, { "kind", a } });
        }
    }
    // Companion Uplink: fired from chrome::HackVisual("applied") when the hack lands, not here at the key press (D3)
    if (a == "hack" && r.value("message", std::string()) == "no cyberdeck installed") chrome::OnNoTarget();
    if (a == "scan") fx::OnScanKey();   // any Kiroshi: highlights + scan readout, even when no Scan-family optic fired

}

class HotkeySink final : public RE::BSInputEventUser {
public:
    bool ShouldHandleEvent(const RE::InputEvent* e) override
    {
        return e && e->eventType == RE::INPUT_EVENT_TYPE::kButton;   // keyboard keys + the "Jump" user event on any device
    }
    void OnButtonEvent(const RE::ButtonEvent* e) override
    {
        if (!e) return;
        if (e->QUserEvent() == "Jump") { legs::OnJumpButton(e->QPressed(), e->QJustPressed(), e->QHeldDownSecs()); }
        if (g_ini.debug && e->QUserEvent() == "Sprint" && (e->QJustPressed() || !e->QPressed())) {   // D10 proof, see PlayerIs("sprinting")
            auto* sp = RE::PlayerCharacter::GetSingleton();
            REX::INFO("kerenzikov: Sprint key {} (moveMode {:X})", e->QJustPressed() ? "down" : "up", sp ? (std::uint32_t)sp->moveMode : 0u);
        }
        // the attack button on any device (mouse, pad trigger): only observed, never consumed. P15 M8 +250: a harness
        // left click 300 ms into the slam re-pick never reached the buffer, so the left mouse button is also taken by device and code
        // (mouse button 0), whatever user event the control map gives it, and debug logs what non-keyboard presses actually carry.
        if (e->QJustPressed() && e->device != RE::INPUT_DEVICE::kKeyboard) {
            const bool byName = e->QUserEvent() == "PrimaryAttack" || e->QUserEvent() == "RightAttack";
            const bool lmb = e->device == RE::INPUT_DEVICE::kMouse && (std::uint32_t)e->GetBSButtonCode() == 0;
            if (g_ini.debug) {
                static int seen = 0;
                if (seen < 20) { ++seen; REX::INFO("input: {} button 0x{:X} user event '{}'{} (debug log {}/20)", e->device == RE::INPUT_DEVICE::kMouse ? "mouse" : "pad/other",
                                                   (std::uint32_t)e->GetBSButtonCode(), e->QUserEvent().c_str() ? e->QUserEvent().c_str() : "", byName || lmb ? " = attack" : "", seen); }
            }
            if (byName || lmb) PostTask([]() { arms::OnAttackPressed(); });
        }
        // MAN-2: is a test's "held" attack really held? (P15: lmb:900 started a light swing 163 ms after the press, and FO4 powers a
        // melee attack on a hold.) Debug logs how long the attack button was down when it is released.
        if (g_ini.debug && !e->QPressed() && e->QHeldDownSecs() > 0.0f && e->device != RE::INPUT_DEVICE::kKeyboard &&
            ((e->device == RE::INPUT_DEVICE::kMouse && (std::uint32_t)e->GetBSButtonCode() == 0) || e->QUserEvent() == "PrimaryAttack"))
            REX::INFO("input: attack button released after {:.2f} s ('{}')", e->QHeldDownSecs(), e->QUserEvent().c_str() ? e->QUserEvent().c_str() : "");
        // Kiroshi scan key: a TAP scans on release, a HOLD zooms. Timed on our own clock: the engine's held time on the release event
        // did not reach the old check (cw-int-3 T4b: a 100 ms tap logged the press and nothing else).
        if (e->device == RE::INPUT_DEVICE::kKeyboard && g_ini.keyScan && (std::uint32_t)e->GetBSButtonCode() == g_ini.keyScan) {
            static std::uint64_t downAt = 0;
            static bool zoomed = false;
            const std::uint64_t now = GetTickCount64();
            if (e->QJustPressed()) downAt = now;
            const std::uint64_t heldMs = downAt ? now - downAt : 0;
            // D12 (2026-11-10): no hold-zoom while the ripperdoc view is up (it zoomed the camera behind the menu); the tap path below
            // already checked the view. Releasing, or the view opening mid-hold, always unzooms.
            const bool viewUp = g_ui && g_view && !g_ui->IsHidden(g_view);
            const bool want = e->QPressed() && heldMs >= 250 && !viewUp;
            if (want != zoomed) {
                zoomed = want;
                PostTask([want]() { const bool vu = g_ui && g_view && !g_ui->IsHidden(g_view); if (!want || (!GameplayMenuOpen() && !vu)) optics::SetZoom(want); });
            }
            if (!e->QPressed() && downAt) {
                downAt = 0;
                if (g_ini.debug) REX::INFO("input: scan key released after {} ms ({})", heldMs, heldMs < 250 ? "tap" : "hold");
                if (heldMs < 250 && !(g_ui && g_view && !g_ui->IsHidden(g_view)))
                    PostTask([]() { if (!GameplayMenuOpen()) FireNativeKey("scan"); });   // cw-int-4: PostTask (STABILITY state lock)
            }
        }
        if (e->device != RE::INPUT_DEVICE::kKeyboard || !e->QJustPressed()) return;
        const std::uint32_t code = (std::uint32_t)e->GetBSButtonCode();
        if (g_ini.debug) { static int logged = 0; if (logged < g_ini.keyLogMax) { ++logged; REX::INFO("input: key 0x{:X} pressed (debug key log {}/{})", code, logged, g_ini.keyLogMax); } }
        const char* action = nullptr;
        if (code && code == g_ini.keyOS) action = "os";
        else if (code && code == g_ini.keyCamo) action = "camo";
        else if (code && code == g_ini.keyHeal) action = "heal";
        else if (code && code == g_ini.keyHack) action = "hack";
        else if (code && code == g_ini.keyHackNext) action = "hack_next";
        else if (code && code == g_ini.keyPacify) action = "pacify";
        else if (code && code == g_ini.keySkippy) action = "skippy";
        if (action) {
            if (g_ui && g_view && !g_ui->IsHidden(g_view)) return;  // not while the ripperdoc menu is up
            std::string a = action;
            PostTask([a]() {
                if (GameplayMenuOpen()) { if (g_ini.debug) REX::INFO("native key {}: ignored, a menu is open", a); return; }
                FireNativeKey(a);
            });
            return;
        }
        if (g_ini.hudEditKey && code == g_ini.hudEditKey) {   // the HUD layout editor: opens it, or saves and closes it
            if (g_ui && g_view && !g_ui->IsHidden(g_view)) return;
            PostTask([]() { hud::ToggleEdit(); });
            return;
        }
        if (g_ini.setupKey && code == g_ini.setupKey && code != g_ini.openKey) {   // anywhere: your setup, read only
            if (g_ui && g_view && !g_ui->IsHidden(g_view)) return;
            PostTask([]() { if (!GameplayMenuOpen()) OpenSetup("key"); });
            return;
        }
        if (!g_ini.openKey || code != g_ini.openKey) return;   // the shop key: the test build only (Cyberware.Test.ini)
        if (g_ui && g_view && !g_ui->IsHidden(g_view)) return;  // the view has focus; it sends close itself
        std::string doc = !g_ini.openKeyDoc.empty() ? g_ini.openKeyDoc : (g_coreOk && !g_engine->catalog().ripperdocOrder.empty() ? g_engine->catalog().ripperdocOrder.front() : "");
        PostTask([doc]() { if (!GameplayMenuOpen()) OpenMenu(doc); });
    }
} g_hotkeySink;

// Kills and hits by the player feed the native on-kill / on-hit handlers. The last weapon the player hit each
// target with decides "melee kill" (melee or hand-to-hand: unarmed covers the cyber arms).
std::map<std::uint32_t, bool> g_lastHitMelee;  // target formID -> last player hit was melee

// Melee by the WEAPON_TYPE ordinal: hand-to-hand (0, the cyber arms and fists) through two-hand axe (6). Do NOT use
// commonlib's TESObjectWEAP::IsMeleeWeapon() or weaponData.type.any(kHandToHand): REX::TEnumSet::any ANDs the ordinals
// as bit masks, so any(kHandToHand) is (type & 0) = always false, and IsMeleeWeapon() is (type & 7) != 0, which is TRUE
// for a gun (9) or a grenade (10) and false for hand-to-hand (cw-int-2 T7: every Mantis kill logged melee=false).
inline bool IsMeleeType(const RE::TESObjectWEAP* w)
{
    if (!w) return false;
    const auto t = static_cast<std::int32_t>(w->weaponData.type.get());
    return t >= static_cast<std::int32_t>(RE::WEAPON_TYPE::kHandToHand) && t <= static_cast<std::int32_t>(RE::WEAPON_TYPE::kTwoHandAxe);
}

namespace arms {
    void TryFinisher(std::uint32_t target, const std::vector<std::string>& weaponKws, bool power, bool staggered, bool sneak);
    void StartDecidedFinisher(const std::string& id, std::uint32_t target, double hpPct);   // a finisher the DoHitMe hook chose before the hit
    void CueHit(const std::vector<std::string>& weaponKws, std::uint32_t target, bool power);   // SOUND: hit cue, below with the leap
    void ForceFinisher(std::string id, std::uint32_t targetId, const json& over);
    void MonowireCleave(std::uint32_t target, const std::vector<std::string>& weaponKws, float damage);
    void GorillaKnockback(std::uint32_t target, const std::vector<std::string>& weaponKws, bool power);   // Gorilla Arms: ragdoll a power punch's target
    void WeakPointHit(std::uint32_t target, int limb, float damage);
    void PoisonBonus(std::uint32_t target, float damage);   // Black Mamba: +dmg_pct vs a poisoned target
    void MicrogenHit(std::uint32_t target, bool melee);      // Microgenerator: the armed bonus on the next gun hit
    void PlayerHit(std::uint32_t attacker, float dealt, bool ballistic);
    void PreSave();
    void PostSave();
}

// ---- The player hit pipeline (P13c, the lead's melee-damage item). Two entry points feed the same two functions:
//   * the DoHitMe call-site hook (arms::DoHitMeDetour), one call per hit the engine applies, with the real HitData;
//   * TESHitEvent (HitSink), the old route.
// P13c proved TESHitEvent is NOT a per-hit signal in FO4: across the T3/T4 strikes the hook counted 13 player-aggressor
// hits and two player melee kills, while the sink saw ONE player melee event (its totalDamage read 0.0). Damage always
// applied; only everything hung off the event starved (hit cues, finishers, knockback, cleave, weak points, poison,
// Microgenerator, and the player-hurt family). With [Arms] bHitFromDoHitMe=1 the hook drives the pipeline and the sink
// stands down, so a hit is never handled twice.
std::atomic<bool> g_hitViaDoHitMe{ false };   // true once the DoHitMe hook is installed with bHitFromDoHitMe=1

void PlayerStrike(RE::TESObjectWEAP* weap, std::uint32_t srcId, std::uint32_t target, bool power, bool sneak, bool staggered, float dealt, int limb, const char* via,
                  std::string finId = {}, double finHp = 0.0)   // finId: a finisher the DoHitMe hook already chose for this hit
{
    std::vector<std::string> kws;
    bool melee = false;
    if (weap) {
        melee = IsMeleeType(weap);
        weap->ForEachKeyword([&](RE::BGSKeyword* k) {
            if (k && k->formEditorID.c_str()) kws.emplace_back(k->formEditorID.c_str());
            return RE::BSContainer::ForEachResult::kContinue;
        });
    }
    // SOUND WIRING 702a9d56's diagnostic, kept: the melee line names the source + keywords, and a player hit that is NOT
    // counted as melee logs under bDebug instead of vanishing.
    std::string srcName = weap && weap->GetFormEditorID() ? weap->GetFormEditorID() : (srcId ? "non-weap" : "none");
    std::string kwj;
    for (auto& k : kws) { kwj += k; kwj += ' '; }
    std::string how = via;
    PostTask([kws, melee, target, power, sneak, staggered, dealt, limb, srcId, srcName, kwj, how, finId, finHp]() {
        if (!g_coreOk) return;
        if (g_lastHitMelee.size() > 512) g_lastHitMelee.clear();
        g_lastHitMelee[target] = melee;
        // every player melee hit, always (cw-int-3 T2: a Gorilla x1 at 150 u left no line at all - a miss or no flag?)
        if (melee) REX::INFO("hit: player melee -> {:08X} power={} sneak={} staggered={} dealt {:.1f} limb {} src {:08X} '{}' kws [{}] via {}", target, power, sneak, staggered, dealt, limb, srcId, srcName, kwj, how);
        else if (g_ini.debug) REX::INFO("hit: player non-melee -> {:08X} dealt {:.1f} src {:08X} '{}' via {}", target, dealt, srcId, srcName, how);
        g_engine->NativeHit(kws, melee);
        if (melee) arms::CueHit(kws, target, power);
        if (!finId.empty()) arms::StartDecidedFinisher(finId, target, finHp);
        else if (g_ini.finishers && power) arms::TryFinisher(target, kws, power, staggered, sneak);
        arms::GorillaKnockback(target, kws, power);
        arms::MonowireCleave(target, kws, dealt);
        arms::WeakPointHit(target, limb, dealt);
        arms::PoisonBonus(target, dealt);
        arms::MicrogenHit(target, melee);
    });
}

void PlayerHurt(std::uint32_t attacker, float dealt, RE::TESObjectWEAP* w)   // the player was hit: Second Heart, Biomonitor, Countershell
{
    bool ballistic = false;
    if (w)
        w->ForEachKeyword([&](RE::BGSKeyword* k) {
            if (k && k->formEditorID.c_str() && !std::strcmp(k->formEditorID.c_str(), "WeaponTypeBallistic")) { ballistic = true; return RE::BSContainer::ForEachResult::kStop; }
            return RE::BSContainer::ForEachResult::kContinue;
        });
    PostTask([attacker, dealt, ballistic]() { talkface::End("the player was hit"); if (!g_coreOk) return; arms::PlayerHit(attacker, dealt, ballistic); g_engine->NativeHurt(); });
}

class HitSink final : public RE::BSTEventSink<RE::TESHitEvent> {
public:
    RE::BSEventNotifyControl ProcessEvent(const RE::TESHitEvent& e, RE::BSTEventSource<RE::TESHitEvent>*) override
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!g_coreOk || !player || !e.target) return RE::BSEventNotifyControl::kContinue;
        const bool toPlayer = e.target.get() == player, byPlayer = e.cause && e.cause.get() == player;
        if (!toPlayer && !byPlayer) return RE::BSEventNotifyControl::kContinue;
        auto* src = RE::TESForm::GetFormByID(e.sourceFormID);
        auto* weap = src ? src->As<RE::TESObjectWEAP>() : nullptr;
        if (g_hitViaDoHitMe.load()) {   // the DoHitMe hook already handled this hit (if the engine applied it)
            if (g_ini.debug) {
                static std::atomic<std::uint32_t> seen{ 0 };
                if (const auto n = ++seen; n <= 5 || n % 25 == 0)
                    REX::INFO("hit: TESHitEvent #{} ({} {:08X}) left to the DoHitMe hook", n, toPlayer ? "player hurt by" : "player hit", toPlayer ? (e.cause ? e.cause->GetFormID() : 0u) : e.target->GetFormID());
            }
            return RE::BSEventNotifyControl::kContinue;
        }
        if (toPlayer) {
            PlayerHurt(e.cause ? e.cause->GetFormID() : 0, e.hitData.totalDamage, weap);
            return RE::BSEventNotifyControl::kContinue;
        }
        PlayerStrike(weap, e.sourceFormID, e.target->GetFormID(), e.hitData.flags.any(RE::HitData::Flag::kPowerAttack),
                     e.hitData.flags.any(RE::HitData::Flag::kSneakAttack), e.hitData.stagger.underlying() != 0, e.hitData.totalDamage,
                     (int)e.hitData.damageLimb.underlying(), "event");
        return RE::BSEventNotifyControl::kContinue;
    }
} g_hitSink;

class DeathSink final : public RE::BSTEventSink<RE::TESDeathEvent> {
public:
    RE::BSEventNotifyControl ProcessEvent(const RE::TESDeathEvent& e, RE::BSTEventSource<RE::TESDeathEvent>*) override
    {
        auto* player = RE::PlayerCharacter::GetSingleton();
        if (!g_coreOk || !e.dead || !e.actorKiller || e.actorKiller.get() != player || !e.actorDying) return RE::BSEventNotifyControl::kContinue;
        std::uint32_t victim = e.actorDying->GetFormID();
        bool sneak = player->IsSneaking();
        // The death event fires inside the killing blow, BEFORE that blow's TESHitEvent, so on a first-hit kill the hit task
        // has not written g_lastHitMelee yet (cw-int-1: a Mantis kill logged melee=false). Also classify by the weapon the
        // player holds at the moment of death (same test as HitSink). Any equipped melee weapon counts: the grenade slot is
        // an equipped WEAP too and can come first in the list.
        // cw-int-3 T7: a 10mm kill logged melee=true - the victim's LAST hit (an earlier blade hit) was ORed in. The weapon in
        // hand decides (the first equipped WEAP that is not a grenade / mine, as arms::EquippedWeapon); the last hit only when
        // the player holds no weapon at all.
        int held = -1;   // -1 none, 0 ranged, 1 melee
        if (auto* mh = player->currentProcess ? player->currentProcess->middleHigh : nullptr)
            for (auto& it : mh->equippedItems)
                if (auto* w = it.item.object ? it.item.object->As<RE::TESObjectWEAP>() : nullptr) {
                    const auto t = static_cast<std::int32_t>(w->weaponData.type.get());
                    if (t == static_cast<std::int32_t>(RE::WEAPON_TYPE::kGrenade) || t == static_cast<std::int32_t>(RE::WEAPON_TYPE::kMine)) continue;
                    held = IsMeleeType(w) ? 1 : 0;
                    break;
                }
        PostTask([victim, sneak, held]() {
            auto it = g_lastHitMelee.find(victim);
            bool melee = held >= 0 ? held == 1 : (it != g_lastHitMelee.end() && it->second);
            REX::INFO("native: player kill {:08X} melee={} sneak={}", victim, melee, sneak);
            g_engine->NativeKill(melee, sneak);
        });
        return RE::BSEventNotifyControl::kContinue;
    }
} g_deathSink;

// ---------------------------------------------------------------- cosave
constexpr std::uint32_t kUid = 'CYBR', kRecState = 'STAT', kRecExt = 'EXTP', kRecEquipLocks = 'ELCK', kRecHacks = 'HAKS';
json g_pendingHacks;   // D2: the 'HAKS' record read at load (ids resolved); applied at kPostLoadGame after the clears
bool g_equipLocksDone = false;   // this save has had its one equip-lock repair (arms::OnLoad); kept in the cosave as 'ELCK'

void OnSave(const F4SE::SerializationInterface* s)
{
    if (!g_coreOk) return;
    StateLock lock(2000, "cosave save");
    std::string body = g_engine->SaveState().dump();
    if (s->OpenRecord(kRecState, 1)) {
        std::uint32_t n = (std::uint32_t)body.size();
        s->WriteRecordData(n);
        s->WriteRecordData(body.data(), n);
    }
    if (s->OpenRecord(kRecExt, 1)) {
        std::uint32_t n = (std::uint32_t)g_env.externalApplied.size();
        s->WriteRecordData(n);
        for (std::uint32_t id : g_env.externalApplied) s->WriteRecordData(id);
    }
    if (g_equipLocksDone && s->OpenRecord(kRecEquipLocks, 1)) {
        const std::uint8_t done = 1;
        s->WriteRecordData(done);
    }
    {   // D2: running quickhacks and machine hacks, by time left (the save itself holds the world without them: pre-save)
        const json hk = { { "engine", g_engine->HacksSave() }, { "chrome", chrome::SaveJson() } };
        const std::string hb = hk.dump();
        if (s->OpenRecord(kRecHacks, 1)) {
            std::uint32_t n = (std::uint32_t)hb.size();
            s->WriteRecordData(n);
            s->WriteRecordData(hb.data(), n);
        }
        REX::INFO("cosave: hacks record: {} quickhack effect(s){}, {} machine hack(s)", hk["engine"].value("fx", json::array()).size(),
                  hk["engine"].contains("upload") ? " + 1 upload" : "", hk["chrome"].value("hacked", json::array()).size());
    }
    REX::INFO("cosave: saved {} bytes of state, {} external perk(s)", body.size(), g_env.externalApplied.size());
}

void OnLoad(const F4SE::SerializationInterface* s)
{
    if (!g_coreOk) return;
    StateLock lock(2000, "cosave load");
    std::uint32_t type, version, length;
    bool gotState = false;
    g_env.externalApplied.clear();
    g_equipLocksDone = false;
    g_pendingHacks = nullptr;
    while (s->GetNextRecordInfo(type, version, length)) {
        if (type == kRecHacks && version == 1) {
            std::uint32_t n = 0;
            s->ReadRecordData(n);
            if (length < sizeof(n) || n > length - sizeof(n)) { REX::ERROR("cosave: hacks record length {} cannot hold {} bytes; ignored", length, n); continue; }
            std::string body(n, '\0');
            s->ReadRecordData(body.data(), n);
            try {
                json hk = json::parse(body);
                auto resolve = [&](json& v) { std::uint32_t id = v.get<std::uint32_t>(); auto r = s->ResolveFormID(id); v = r ? *r : 0u; };
                if (hk.contains("engine")) {
                    for (auto& f : hk["engine"]["fx"]) if (f.contains("target")) resolve(f["target"]);
                    if (hk["engine"].contains("upload") && hk["engine"]["upload"].contains("target")) resolve(hk["engine"]["upload"]["target"]);
                }
                if (hk.contains("chrome")) for (auto& h : hk["chrome"]["hacked"]) if (h.contains("id")) resolve(h["id"]);
                g_pendingHacks = std::move(hk);
            } catch (const std::exception& e) { REX::ERROR("cosave: hacks record unreadable ({}); no hacks restored", e.what()); }
            continue;
        }
        if (type == kRecEquipLocks && version == 1) { std::uint8_t done = 0; s->ReadRecordData(done); g_equipLocksDone = done != 0; continue; }
        if (type == kRecState && version == 1) {
            std::uint32_t n = 0;
            s->ReadRecordData(n);
            if (length < sizeof(n) || n > length - sizeof(n)) {   // a damaged record must not size the allocation
                REX::ERROR("cosave: state record length {} cannot hold {} bytes; starting empty", length, n);
                continue;
            }
            std::string body(n, '\0');
            s->ReadRecordData(body.data(), n);
            try {
                g_engine->LoadState(json::parse(body));
                gotState = true;
            } catch (const std::exception& e) {
                REX::ERROR("cosave: state record unreadable ({}); starting empty", e.what());
                g_engine->NewGame();
            }
        } else if (type == kRecExt && version == 1) {
            std::uint32_t n = 0;
            s->ReadRecordData(n);
            if (length < sizeof(n) || n > (length - sizeof(n)) / sizeof(std::uint32_t)) {
                REX::ERROR("cosave: external perk record length {} cannot hold {} ids; ignored", length, n);
                continue;
            }
            for (std::uint32_t i = 0; i < n; ++i) {
                std::uint32_t id = 0;
                s->ReadRecordData(id);
                if (auto r = s->ResolveFormID(id)) g_env.externalApplied.insert(*r);
            }
        }
    }
    if (!gotState) { g_engine->NewGame(); cwset::LoadDefaults(); }   // fresh save: the player's last settings
    REX::INFO("cosave: loaded ({}); {} owned piece(s), {} mod(s)", gotState ? "state record" : "no record, fresh", g_engine->state().instances.size(), g_engine->state().modInstances.size());
}

void OnRevert(const F4SE::SerializationInterface*)
{
    StateLock lock(2000, "cosave revert");
    g_pendingHacks = nullptr;
    if (g_coreOk) g_engine->NewGame();
    g_env.externalApplied.clear();
    g_equipLocksDone = false;
}

// ---------------------------------------------------------------- startup proof (headless evidence in Cyberware.log)
void LoadCore()
{
    std::string err;
    auto cj = ReadJsonFile(g_ini.catalogPath, err);
    if (!cj) { REX::ERROR("catalog: {}", err); return; }
    const int roundsOff = PlsRoundsFilterCatalog(*cj);   // [Arms] sPlsRoundsShipped: an ungraded round is not offered at all
    try {
        cw::Catalog cat = cw::Catalog::FromJson(*cj);
        int tiers = 0;
        for (auto& [k, v] : cat.items) tiers += (int)v.tiers.size();
        REX::INFO("catalog: OK - {} slots, {} stats, {} abilities, {} items ({} tiers), {} mods, {} cyber perks, {} ripperdocs",
                  cat.slots.size(), cat.stats.size(), cat.abilities.size(), cat.items.size(), tiers, cat.mods.size(), cat.perks.size(), cat.ripperdocs.size());
        if (roundsOff)
            REX::INFO("catalog: [Arms] sPlsRoundsShipped='{}' removed {} launcher-round entries and references (each logged above), so those rounds are not offered by this build",
                      g_ini.plsRoundsShipped, roundsOff);
        g_engine = std::make_unique<cw::Engine>(std::move(cat), &g_env);
        g_api = std::make_unique<cw::Api>(g_engine.get());
        g_api->pluginVersion = kVersion;
        g_api->buildId = kBuildId;
        g_api->onClose = CloseMenu;
        cwset::Init(g_engine.get(), ApplySettingsToGame, g_ini.settingsDir);
        g_coreOk = true;
    } catch (const cw::CwError& e) {
        REX::ERROR("catalog: {} {}", e.code, e.what());
    }
}

// ARMORY consumables whose effect is native (RAM): the item form, the native ("ram_restore" | "ram_regen"), its EditorID
struct ArmoryNative { RE::TESForm* form; std::string native; std::string edid; };
std::vector<ArmoryNative> g_armoryNative;

// ARMORY gear (docs/ARMORY.md §5): armory-formmap.json's stock -> the engine's gear list, so Varga's shop page can sell it.
// Only stock-ready entries (CP mesh staged) unless [Armory] bSellUnready=1 (test profile). Inert without the plugin.
void LoadArmoryGear()
{
    if (!g_coreOk || !g_engine) return;
    std::string err;
    auto j = ReadJsonFile(g_ini.armoryFormmapPath, err);
    if (!j) { REX::INFO("armory: no gear ({})", err); return; }
    try {
        const std::string plugin = j->value("plugin", "Cyberware - Armory.esp");
        auto* dh = RE::TESDataHandler::GetSingleton();
        const auto& forms = (*j)["forms"];
        std::vector<cw::GearDef> gear;
        int skipped = 0, missing = 0;
        for (const auto& e : (*j)["stock"]) {
            std::string id = e.value("edid", "");
            if (!e.value("stockReady", false) && !g_ini.armorySellUnready) { ++skipped; continue; }
            if (!forms.contains(id)) { ++missing; continue; }
            std::string local = forms[id].get<std::string>();
            std::uint32_t lid = (std::uint32_t)std::stoul(local, nullptr, 16);
            if (!dh || !dh->LookupForm(lid, plugin)) { ++missing; continue; }
            // the row's own text for the Armory page (armory.json): `description` + `lore`; `desc` is the older single line
            const std::string what = e.value("description", e.value("desc", std::string()));
            gear.push_back({ id, e.value("kind", ""), e.value("name", id), e.value("doc", ""), "@" + plugin + "|" + local, e.value("price", (int64_t)0),
                             what, e.value("lore", std::string()) });
        }
        g_armoryNative.clear();
        if (j->contains("consumables"))
            for (auto& [id, c] : (*j)["consumables"].items()) {
                if (!c.contains("native") || !c["native"].is_string() || !forms.contains(id)) continue;
                std::uint32_t lid = (std::uint32_t)std::stoul(forms[id].get<std::string>(), nullptr, 16);
                if (auto* f = dh ? dh->LookupForm(lid, plugin) : nullptr) g_armoryNative.push_back({ f, c["native"].get<std::string>(), id });
            }
        REX::INFO("armory: {} native consumable(s) watched", g_armoryNative.size());
        REX::INFO("armory: {} gear for sale ({} not stock-ready{}, {} not in the loaded plugin)", gear.size(), skipped,
                  g_ini.armorySellUnready ? "" : " and hidden", missing);
        g_engine->SetGear(std::move(gear));
    } catch (const std::exception& e) { REX::ERROR("armory: {} unreadable: {}", g_ini.armoryFormmapPath, e.what()); }
}

void ResolveForms()
{
    if (!g_coreOk) return;
    const auto& cat = g_engine->catalog();
    auto* cur = ResolveFormKey(cat.currencyForm);
    g_env.currency = cur ? cur->As<RE::TESBoundObject>() : nullptr;
    REX::INFO("forms: currency {} -> {}", cat.currencyForm, g_env.currency ? "OK" : "NOT FOUND");
    {   // upgrade components (BETA QA): resolve every component form NOW so a wrong id fails the load check
        std::set<std::string> comps;
        for (auto& [iid, itd] : cat.items)
            for (auto& td : itd.tiers)
                for (auto& cc : td.components) comps.insert(cc.form);
        int bad2 = 0;
        for (auto& fk : comps)
            if (!ResolveFormKey(fk)) { ++bad2; REX::ERROR("forms: upgrade component {} did not resolve", fk); }
        REX::INFO("forms: {} upgrade component form(s), {} failed", comps.size(), bad2);
    }
    auto* dh = RE::TESDataHandler::GetSingleton();
    auto* file = dh ? dh->LookupModByName(g_map.plugin) : nullptr;
    REX::INFO("forms: {} is {}", g_map.plugin, file ? (file->compileIndex == 0xFE ? "loaded as light (ESL)" : "loaded as FULL - it must be ESL-flagged") : "NOT LOADED");
    int perks = 0, bad = 0;
    for (auto& [edid, local] : g_map.ids) {
        bool isPerk = edid.rfind("CW_Stat_", 0) == 0 || edid.rfind("CW_Ab_", 0) == 0 || edid.rfind("CW_Nat_", 0) == 0;
        if (!isPerk) continue;
        auto* f = g_map.Get(edid);
        if (f && f->As<RE::BGSPerk>()) { g_env.knownPerks.insert(f->GetFormID()); ++perks; }
        else { ++bad; if (bad <= 10) REX::WARN("forms: {} (local {:03X}) did not resolve to a perk", edid, local); }
    }
    // Every perk the catalog can ask for must exist in the ESP.
    int needMissing = 0;
    for (auto& sd : cat.stats) {
        for (int b = 0; b < sd.bits; ++b) if (!g_map.ids.count("CW_Stat_" + sd.id + "_P" + std::to_string(b))) ++needMissing;
        for (int b = 0; b < sd.negBits; ++b) if (!g_map.ids.count("CW_Stat_" + sd.id + "_N" + std::to_string(b))) ++needMissing;
    }
    for (auto& [id, a] : cat.abilities) if (a.perk.empty() && !g_map.ids.count("CW_Ab_" + id)) ++needMissing;
    REX::INFO("forms: {} Cyberware perks resolved, {} failed; catalog needs {} perk(s) the formmap does not have{}",
              perks, bad, needMissing, needMissing ? " - REGENERATE Cyberware.esp from this catalog" : "");
    int chairsOk = 0;
    for (auto& c : g_map.chairRefs) {
        auto* f = dh ? dh->LookupForm(c.local, c.plugin) : nullptr;
        if (f && f->As<RE::TESObjectREFR>()) ++chairsOk;
        else REX::WARN("forms: ripperdoc '{}' chair ref {}|{:06X} did not resolve to a placed reference", c.doc, c.plugin, c.local);
    }
    REX::INFO("forms: ripperdoc chair refs resolved {} of {}", chairsOk, g_map.chairRefs.size());
    {   // the generator and the engine must agree on every native perk (cw_effects.cpp; espgen bakes cwtest's export)
        int checked = 0, bad = 0, notBaked = 0;
        for (auto& [ab, def] : cat.abilities) {
            const json& nat = def.raw.contains("spec") && def.raw["spec"].is_object() ? def.raw["spec"].value("native", json()) : json();
            if (!nat.is_object()) continue;
            for (int t = 1, cap = cw::BakedTierCap(ab); t <= cap; ++t) {   // 6 for the Kiroshi scans, 5 otherwise
                json prm = cw::NativeParamsFor(nat, t);
                cw::NativeFamily f = cw::FamilyOf(nat.value("handler", ""), nat.value("activation", ""), prm);
                cw::NativeEffects e = cw::EffectsFor(nat.value("handler", ""), f, prm);
                if (e.Empty()) continue;
                std::string perk = cw::TimedPerkId(ab, t);
                if (!g_map.native.contains(perk) && !g_map.ids.count(perk)) { ++notBaked; continue; }   // cw_effects.cpp bakes only the tiers its mods list
                const json& m = g_map.native.contains(perk) ? g_map.native[perk] : json();
                ++checked;
                if (!m.is_object() || m.value("kind", std::string()) != e.fx.kind || std::fabs(m.value("magnitude", -1.0) - e.fx.magnitude) > 1e-3 ||
                    m.value("areaFeet", -1) != e.fx.areaFeet || m.value("avs", -1) != (int)e.avs.size() || m.value("entries", -1) != (int)e.entries.size()) {
                    ++bad;
                    if (bad <= 5) REX::WARN("forms: native perk {} engine wants {} {} area {} ({} AV, {} entries) but formmap has {}", perk, e.fx.kind, e.fx.magnitude,
                                            e.fx.areaFeet, e.avs.size(), e.entries.size(), m.dump());
                }
            }
        }
        REX::INFO("forms: native perks checked {}, mismatched {}, not baked {} (mod-only tiers); Chrome Crit perk {}", checked, bad, notBaked, g_map.Get(cw::kChromeCritPerk) ? "OK" : "NOT FOUND");
    }
    for (auto& sd : cat.stats)
        if (!FindAV(sd.av)) REX::WARN("forms: stat '{}' actor value '{}' not found{}", sd.id, sd.av, sd.custom ? " (custom: is Cyberware.esp regenerated?)" : "");
    for (auto& pk : cat.perkOrder)
        if (auto& p = cat.perks.at(pk); !p.fo4Perk.empty())
            REX::INFO("forms: cyber perk '{}' follows {} -> {}", pk, p.fo4Perk, ResolveFormKey(p.fo4Perk) ? "OK" : "NOT FOUND");
}

void SelfTestLog(const char* when)
{
    if (!g_coreOk) return;
    std::string res = g_api->Handle(R"({"id":"selftest","op":"getState"})");
    json r = json::parse(res, nullptr, false);
    if (r.is_discarded() || !r.value("ok", false)) { REX::ERROR("selftest[{}]: getState failed: {}", when, res.substr(0, 400)); return; }
    auto& s = r["state"];
    REX::INFO("selftest[{}]: level {} {} {} capacity {}/{} owned {} installed-stats {} applied {}", when, s["level"].dump(),
              s["currency"]["amount"].dump(), s["currency"]["name"].get<std::string>(), s["capacity"]["used"].dump(), s["capacity"]["max"].dump(),
              s["instances"].size(), s["totals"].dump(), s["applied"].dump());
}

// ---------------------------------------------------------------- arm cyberware (ARMS)
// Arm weapons (CW_MantisBlades, CW_GorillaArms, CW_Monowire) are in the inventory while their cyberware is installed.
// A drawn weapon whose keyword has a visual (CW_AnimsCyberMantis -> CW_MantisBladesVisual) wears that non-playable
// ARMO from weaponDraw to weaponSheathe. The Mantis leap: while the blades are drawn, the player sprints and a hostile
// is 4-12 m ahead in a 10 deg cone with LOS, the player carries CW_MantisLeapReady (an actor keyword that selects the
// leap subgraph entry, the way ActorAttackInjuredLeft swaps anims); a sprint attack then plays the leap clip and the
// DLL moves the player from weaponSwing to HitFrame (cw::LungePos), ending 85 units short of the target.
namespace arms {
    cw::ArmsVisualState visual;
    std::string visualArmo;               // the ARMO equipped now (EditorID)
    bool leapReady = false, leapAttacking = false;
    std::uint64_t leapSwingAt = 0;   // when the leap attack's weaponSwing arrived: the sprint-stuck watchdog stands down for that clip (bounded, in case AttackStop never comes)
    RE::ActorHandle leapTarget;
    struct Lunge { bool active = false; std::uint64_t t0 = 0; cw::Vec3 start; RE::ActorHandle target;
                   bool launch = false, wasAir = false; cw::LeapShape shape; cw::Vec3 aim; int frames = 0; double peakZ = 0; } lunge;   // launch: bLeapLaunch's authored flight
    cw::LeapParams leapParams;
    std::atomic<bool> hooked{ false };

    RE::BGSKeyword* Kw(const std::string& edid) { auto* f = edid.empty() ? nullptr : g_map.Get(edid); return f ? f->As<RE::BGSKeyword>() : nullptr; }
    std::string S(const json& j, const char* k) { return j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(); }

    RE::TESObjectWEAP* EquippedWeapon(RE::Actor* a)
    {
        auto* mh = a && a->currentProcess ? a->currentProcess->middleHigh : nullptr;
        if (!mh) return nullptr;
        for (auto& e : mh->equippedItems)   // the grenade slot is an equipped WEAP too and can come first: skip grenades/mines
            if (auto* w = e.item.object ? e.item.object->As<RE::TESObjectWEAP>() : nullptr) {
                const auto t = static_cast<std::int32_t>(w->weaponData.type.get());
                if (t == static_cast<std::int32_t>(RE::WEAPON_TYPE::kGrenade) || t == static_cast<std::int32_t>(RE::WEAPON_TYPE::kMine)) continue;
                return w;
            }
        return nullptr;
    }
    bool WeapHas(RE::TESObjectWEAP* w, RE::BGSKeyword* k) { return w && k && static_cast<const RE::BGSKeywordForm*>(w)->HasKeyword(k); }

    // Element looks (queue item b): the installed edge / knuckles / round mod per arm picks an ARMO variant whose ARMAs
    // carry that element's MSWP (espgen formmap arms.elementVisuals). "" = the default (physical) look.
    std::map<std::string, std::string> element;   // "mantis" | "mono" | "gorilla" | "pls" -> thermal | chemical | electrical | explosive | tranq
    std::string Variant(const std::string& armo, const std::string& el)
    {
        if (armo.empty() || el.empty()) return armo;
        const json ev = g_map.arms.value("elementVisuals", json::object());
        std::string v = ev.contains(armo) ? S(ev[armo], el.c_str()) : std::string();
        return v.empty() ? armo : v;
    }

    // the visual ARMO EditorID for the equipped weapon, or ""
    std::string VisualFor(RE::TESObjectWEAP* w)
    {
        for (auto& [kw, armo] : g_map.arms.value("visuals", json::object()).items())
            if (armo.is_string() && WeapHas(w, Kw(kw))) return Variant(armo.get<std::string>(), element[kw.find("Monowire") != std::string::npos ? "mono" : "mantis"]);
        return {};
    }

    void SetVisual(const std::string& edid, bool on)
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* f = g_map.Get(edid);
        auto* armo = f ? f->As<RE::TESObjectARMO>() : nullptr;
        auto* em = RE::ActorEquipManager::GetSingleton();
        if (!p || !armo || !em) { REX::WARN("arms: visual {} not available (form {}, equip manager {})", edid, (void*)armo, (void*)em); return; }
        RE::BGSObjectInstance inst(armo, nullptr);
        if (on) {
            if (p->GetInventoryObjectCount(armo) == 0) p->AddObjectToContainer(armo, RE::BSTSmartPointer<RE::ExtraDataList>{}, 1, nullptr, RE::ITEM_REMOVE_REASON::kNone);
            bool ok = em->EquipObject(p, inst, 0, 1, nullptr, false, true, false, true, false);
            REX::INFO("arms: visual {} equipped ({})", edid, ok ? "ok" : "EquipObject returned false");
        } else {
            bool ok = em->UnequipObject(p, &inst, 1, nullptr, 0, false, true, false, true, nullptr);
            REX::INFO("arms: visual {} unequipped ({})", edid, ok ? "ok" : "UnequipObject returned false");
        }
    }

    void Apply(int change, const std::string& armo)
    {
        if (change > 0) { visualArmo = armo; SetVisual(armo, true); }
        else if (change < 0 && !visualArmo.empty()) { SetVisual(visualArmo, false); visualArmo.clear(); }
    }

    // A keyword added or removed at runtime does NOT re-pick the weapon subgraph: the game chooses it when the graph or
    // equip state is rebuilt (cost 4 sprint attempts, 2026-11-08). Changing the anim flavor re-picks it (the console
    // ChangeAnimFlavor with no argument resets it to none, which it already was): requestedWeaponSubGraphID moves to current in
    // a frame or two and the graph replays a quick weaponDraw + attackStop. Call only when no attack is running.
    std::string DrivenVars(RE::PlayerCharacter* p, bool* anyDriven = nullptr);   // below, next to the stuck watchdogs
    void RepickWeaponGraph(const char* why)
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        const std::string before = p ? DrivenVars(p) : std::string();
        RE::Console::ExecuteCommand("player.ChangeAnimFlavor");
        REX::INFO("arms: re-picked the weapon subgraph ({}) moveMode {:X} graph before {}", why, p ? (std::uint32_t)p->moveMode : 0u, before);
    }

    // The re-pick freeze (LESSONS 2026-11-08): a ChangeAnimFlavor fired straight from SetLeapReady /
    // SetSlamReady could land mid-sprint or mid-attack and leave the player at a fixed position with the
    // sprint bit still set. Queue it instead and fire from the pump a few frames later, only when no
    // attack, lunge or finisher is running (Frame() checks those flags, declared after this point).
    // cw-int-1 (2026-11-08): the re-pick also stops a running SPRINT (READY -> re-pick at moveMode 1A6 -> SprintStop ->
    // sprint STUCK -> a plain-clip lunge, 2 of 2 leaps). So the leap now goes READY before the sprint (a target in the cone
    // while walking) and a queued re-pick waits until the player is not sprinting; only the stuck recovery may fire mid-sprint.
    struct RepickReq { bool pending = false, evenSprinting = false; std::string why; std::uint64_t notBefore = 0, nextWaitLog = 0; } repick;
    std::uint64_t lastSprintMs = 0;     // last tick the sprint bit was set: only a SPRINT attack lunges (READY arms while walking)
    std::uint64_t leapRearmAfter = 0;   // after a stuck recovery: no READY for 2 s (cw-int-1: recovery dropped READY, the target
                                        // loop re-armed it the same tick, re-picked again and stopped the sprint again)
    void RequestRepick(const char* why, bool evenSprinting = false)
    {
        repick.pending = true;
        repick.evenSprinting = evenSprinting;
        repick.why = why;
        repick.notBefore = GetTickCount64() + 50;
    }

    void SetLeapReady(bool on)
    {
        if (on == leapReady) return;
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* k = Kw(S(g_map.arms, "leapKeyword"));
        if (!p || !k) return;
        if (on) p->AddKeyword(k); else p->RemoveKeyword(k);
        leapReady = on;
        if (g_ini.debug) REX::INFO("arms: leap {}", on ? "READY (CW_MantisLeapReady on the player)" : "off");
        hud::Event({ { "type", "cue" }, { "kind", "leap" }, { "on", on } });   // the ready cue (MAN, QUALITY-BAR): the HUD view draws it
        RequestRepick(on ? "leap ready" : "leap off");
    }

    // ---- installed visuals, PLS, rotor speed, Gorilla ground slam (2026-11-08; all UNVERIFIED IN GAME)
    RE::TESBoundObject* Obj(const std::string& edid) { auto* f = edid.empty() ? nullptr : g_map.Get(edid); return f ? f->As<RE::TESBoundObject>() : nullptr; }
    void RemoveAll(const std::string& edid)
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* o = Obj(edid);
        if (!p || !o) return;
        std::uint32_t c = p->GetInventoryObjectCount(o);
        if (c) { RE::TESObjectREFR::RemoveItemData rd(o, (std::int32_t)c); p->RemoveItem(rd); }
    }
    // Gorilla knuckles / PLS launcher: worn silently the whole time the piece is installed (ARMS, b1418275)
    std::map<std::string, std::string> worn;   // installed-visual handler -> the ARMO (variant) worn now
    void WearInstalled(const std::string& handler, bool on)
    {
        std::string base = S(g_map.arms.value("installedVisuals", json::object()), handler.c_str());
        if (base.empty()) return;
        std::string want = Variant(base, element[handler == "ar_gorilla" ? "gorilla" : "pls"]);
        auto it = worn.find(handler);
        if (on && it != worn.end() && it->second == want) return;
        if (it != worn.end()) { SetVisual(it->second, false); RemoveAll(it->second); worn.erase(it); }
        if (!on) { RemoveAll(base); return; }
        // a load or an element change: drop any other look of this piece the save still holds
        std::vector<std::string> all{ base };
        const json ev = g_map.arms.value("elementVisuals", json::object());
        if (ev.contains(base)) for (auto& [el, v] : ev[base].items()) if (v.is_string()) all.push_back(v.get<std::string>());
        for (auto& a : all) if (a != want) RemoveAll(a);
        SetVisual(want, true);
        worn[handler] = want;
    }
    double Pn(const cw::NativeAbility& a, const char* k, double d);   // defined below
    bool Active(const char* handler) { for (auto& [id, n] : g_activeNative) if (n.handler == handler) return true; return false; }
    void HurtTick();   // defined below (the hurt family)
    bool IsHostile(RE::Actor* a, RE::Actor* b);
    // Element arm WEAPs (CP: the edge / knuckles / cable mod converts the damage): the inventory holds the variant
    // CW_<Arm>_<Element> (base 1 + typed damage, espgen) that matches element[arm], else the base WEAP.
    std::map<std::string, std::string> armHeld;   // ar_* handler -> the WEAP EditorID it holds now
    std::string ElemKey(const std::string& h) { return h == "ar_mantis" ? "mantis" : h == "ar_gorilla" ? "gorilla" : h == "ar_monowire" ? "mono" : ""; }
    std::vector<std::string> ArmWeapons(const std::string& h)   // base first, then the variants
    {
        std::vector<std::string> out;
        for (auto& [edid, w] : g_map.arms.value("weapons", json::object()).items())
            if (S(w, "handler") == h) {
                out.push_back(edid);
                if (w.contains("elements") && w["elements"].is_object())
                    for (auto& [el, v] : w["elements"].items()) if (v.is_string()) out.push_back(v.get<std::string>());
            }
        return out;
    }
    std::string ArmWeapFor(const std::string& h)
    {
        for (auto& [edid, w] : g_map.arms.value("weapons", json::object()).items())
            if (S(w, "handler") == h) {
                const std::string el = element[ElemKey(h)];
                if (!el.empty() && w.contains("elements")) { std::string v = S(w["elements"], el.c_str()); if (!v.empty()) return v; }
                return edid;
            }
        return {};
    }
    std::uint64_t armSwapAt = 0;   // an equipped arm WEAP was just removed (uninstall / replace)
    bool loadingSave = false;      // set by OnLoad, cleared after the post-load Sync: handler enables then are a load, not an install
    std::uint32_t lastOtherWeap = 0;   // the last non-arm weapon the player held: given back when an equipped arm is uninstalled
    std::uint64_t restoreAt = 0;
    bool IsArmWeap(RE::TESObjectWEAP* w)
    {
        if (!w) return false;
        for (auto h : { "ar_mantis", "ar_gorilla", "ar_monowire" })
            for (auto& e : ArmWeapons(h)) if (auto* o = Obj(e); o && o->As<RE::TESObjectWEAP>() == w) return true;
        return false;
    }
    void HoldArmWeapon(const std::string& h, bool on)
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p) return;
        const std::string want = on ? ArmWeapFor(h) : std::string();
        auto* eq = EquippedWeapon(p);
        bool wasEquipped = false;
        for (auto& e : ArmWeapons(h)) {
            if (e == want) continue;
            auto* o = Obj(e);
            if (o && eq && eq == o->As<RE::TESObjectWEAP>()) wasEquipped = true;
            RemoveAll(e);   // also clears a look a save still holds
        }
        if (!on) {
            armHeld.erase(h);
            if (wasEquipped) {   // uninstalled (or replaced) while in the hands: drop the drawn visual now, not after the 1.5 s reconcile,
                armSwapAt = GetTickCount64();   // and let a replacing arm installed right after take the hands (USE-CASES rows 3, 26)
                if (visual.on) { Apply(-1, ""); visual.on = false; }
                restoreAt = GetTickCount64() + 400;   // no replacing arm by then: give the previous weapon back (USE-CASES row 26)
            }
            REX::INFO("arms: {} weapons removed{}", h, wasEquipped ? " (it was equipped)" : "");
            return;
        }
        auto* w = Obj(want);
        if (!w) { REX::WARN("arms: {} not found for {}", want, h); return; }
        if (p->GetInventoryObjectCount(w) == 0) p->AddObjectToContainer(w, RE::BSTSmartPointer<RE::ExtraDataList>{}, 1, nullptr, RE::ITEM_REMOVE_REASON::kNone);
        if (!wasEquipped && armSwapAt && GetTickCount64() - armSwapAt < 3000) wasEquipped = true;   // swap: the old arm was in the hands
        armSwapAt = 0;
        if (wasEquipped)
            if (auto* em = RE::ActorEquipManager::GetSingleton()) { RE::BGSObjectInstance inst(w, nullptr); const bool ok = em->EquipObject(p, inst, 0, 1, nullptr, false, false, false, true, false); if (!ok) REX::WARN("arms: {} swap: EquipObject returned false", h); }
        if (armHeld[h] != want) REX::INFO("arms: {} holds {}{}", h, want, wasEquipped ? " (re-equipped)" : "");
        armHeld[h] = want;
    }
    // [Arms] bVisualReattach (ARMS, 2026-11-10). QD block 26 s8 + P15b T2: the launcher is on the arm for about 0.2 s on the
    // FIRST throw after an install, then gone (60-90 ms after weaponAttach 2) and never back on later throws; the tint, which holds
    // only shapes it finds under the biped, then held 0 (T2). WearInstalled never re-wears it: worn[] still names the ARMO. This
    // checks, every slow tick, that each worn installed visual sits in every loaded biped with a 3D clone (a culled clone counts:
    // the Pip-Boy and keep hides cull, they do not detach); missing on two checks in a row -> unequip + equip, at most every 3 s.
    struct Reattach { int miss = 0; std::uint64_t lastAt = 0; int count = 0; };
    std::map<std::string, Reattach> reattach;   // handler -> state; reset on load
    const char* VisualMissing(RE::PlayerCharacter* p, std::uint32_t id)
    {
        const RE::BipedAnim* fpb = p->firstPersonBipedAnim.get();
        const RE::BipedAnim* tpb = p->biped.get();
        auto has = [id](const RE::BipedAnim* b) {
            for (int i = 0; i < std::to_underlying(RE::BIPED_OBJECT::kTotal); ++i) {
                const auto& o = b->object[i];
                if (o.parent.object && o.parent.object->GetFormID() == id && o.partClone) return true;
            }
            return false;
        };
        const bool fpMiss = fpb && p->Get3D(true) && !has(fpb), tpMiss = tpb && p->Get3D(false) && !has(tpb);
        return fpMiss && tpMiss ? "1P and 3P" : fpMiss ? "1P" : tpMiss ? "3P" : nullptr;
    }
    void ReattachTick(std::uint64_t now)
    {
        if (!g_ini.visualReattach) return;
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p || !p->Get3D(false)) return;
        for (auto& [h, armo] : worn) {
            auto* f = g_map.Get(armo);
            if (!f) continue;
            auto& r = reattach[h];
            const char* where = VisualMissing(p, f->GetFormID());
            if (!where) { if (r.miss >= 2) REX::INFO("arms: reattach: {} visual {} back on the biped", h, armo); r.miss = 0; continue; }
            if (++r.miss < 2 || (r.lastAt && now - r.lastAt < 3000)) continue;
            r.lastAt = now; ++r.count;
            REX::WARN("arms: reattach: {} visual {} has no 3D on the {} biped while installed -> re-equipped (#{})", h, armo, where, r.count);
            SetVisual(armo, false);
            SetVisual(armo, true);
        }
    }
    void RefreshInstalled()
    {
        for (auto h : { "ar_gorilla", "ar_pls" }) if (worn.count(h)) WearInstalled(h, true);
        for (auto h : { "ar_mantis", "ar_gorilla", "ar_monowire" }) if (armHeld.count(h)) HoldArmWeapon(h, true);
    }

    // Monowire: CP's lash hits several enemies. A hit with the Monowire also damages up to targets-1 other hostiles near
    // the struck one (within 150 units of it and the Monowire's reach of the player), with the same damage (kDamage).
    void MonowireCleaveImpl(std::uint32_t targetId, const std::vector<std::string>& kws, float dealt)
    {
        if (dealt <= 0 || std::find(kws.begin(), kws.end(), std::string("CW_AnimsCyberMonowire")) == kws.end()) return;
        // Militech Paraline Mk.3+ (TweakDB IncreasedDamageBasedOnMissingRamAbility): Monowire hits do +2% per missing RAM point,
        // capped at +30%. The game applied `dealt` already; the bonus is a second kDamage on the struck actor.
        if (g_engine)
            if (double per = g_engine->DeckParam("mono_dmg_per_missing_ram_pct"); per > 0) {
                const double mxPct = g_engine->DeckParam("mono_dmg_max_pct"), missing = std::max(0.0, g_engine->RamMaxNow() - std::max(0.0, g_engine->Ram()));
                const double pct = std::min(mxPct > 0 ? mxPct : 1e9, per * missing);
                auto* tf0 = RE::TESForm::GetFormByID(targetId);
                auto* t0 = tf0 ? tf0->As<RE::Actor>() : nullptr;
                auto* hp0 = FindAV("Health");
                if (pct > 0 && t0 && hp0) {
                    static_cast<RE::ActorValueOwner*>(t0)->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *hp0, -(float)(dealt * pct / 100.0));
                    if (g_ini.debug) REX::INFO("arms: monowire +{:.0f}% ({:.0f} RAM missing) = {:.0f} more", pct, missing, dealt * pct / 100.0);
                }
            }
        int targets = 0; double reachM = 4.0;
        for (auto& [id, n] : g_activeNative) if (n.handler == "ar_monowire") { targets = (int)std::lround(Pn(n, "targets", 1)); reachM = Pn(n, "cleave_reach_m", 4.0); }   // metres, flat by tier: what the wire visibly reaches on the 1P HitFrame (WIR-3)
        if (targets <= 1) return;
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* tf = RE::TESForm::GetFormByID(targetId);
        auto* t = tf ? tf->As<RE::Actor>() : nullptr;
        auto* hp = FindAV("Health");
        auto* pl = RE::ProcessLists::GetSingleton();
        if (!p || !t || !hp || !pl) return;
        auto tp = t->GetPosition(), pp = p->GetPosition();
        const double reach = reachM * 70.0;
        int extra = 0;
        std::vector<std::uint32_t> struck;
        for (auto& hnd : pl->highActorHandles) {
            if (extra >= targets - 1) break;
            auto a = hnd.get();
            if (!a || a.get() == p || a.get() == t || a->lifeState != 0 || !IsHostile(a.get(), p)) continue;
            auto ap = a->GetPosition();
            double dt2 = (ap.x - tp.x) * (ap.x - tp.x) + (ap.y - tp.y) * (ap.y - tp.y), dp2 = (ap.x - pp.x) * (ap.x - pp.x) + (ap.y - pp.y) * (ap.y - pp.y);
            if (dt2 > 150.0 * 150.0 || dp2 > reach * reach) continue;
            static_cast<RE::ActorValueOwner*>(a.get())->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *hp, -dealt);
            struck.push_back(a->GetFormID());
            ++extra;
        }
        if (extra && g_ini.debug) REX::INFO("arms: monowire lash hit {} more ({:.0f} each)", extra, dealt);
        // Data Tunneling (CP): a quickhack running on the struck enemy spreads to the others the same lash hit
        if (!struck.empty() && Active("perk_data_tunneling") && g_engine) {
            int n = g_engine->SpreadHacks(targetId, struck);
            if (n) REX::INFO("arms: Data Tunneling spread the quickhack to {} more", n);
        }
    }
    void SetKeyword(const std::string& edid, bool on)
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* k = Kw(edid);
        if (!p || !k) { REX::WARN("arms: keyword {} not found", edid); return; }
        if (on) p->AddKeyword(k); else p->RemoveKeyword(k);
    }
    // An actor keyword added at runtime does not re-pick the weapon subgraph (LESSONS), so with a weapon already out the PLS throw stayed
    // vanilla: arm off screen until 0.67 s, then a 0.25 s sweep (QD block 16). The queued re-pick waits for a neutral state with no menu open.
    void PlsRepick(const char* why, std::uint64_t delayMs)
    {
        if (!g_ini.plsRepick) return;
        RequestRepick(why);
        repick.notBefore = GetTickCount64() + delayMs;
    }
    double Pn(const cw::NativeAbility& a, const char* k, double d)
    {
        return a.params.is_object() && a.params.contains(k) && a.params[k].is_number() ? a.params[k].get<double>() : d;
    }

    // PLS: charges = how many CW_ProjectileLaunchSystem the player holds (the grenade key throws one); one comes back
    // every recharge_s while below the max (charges + Launch Capacity Override).
    // CP's stat pool (cw::PlsPoolStep): the whole pool refills over recharge_s / (1 + regen%); one charge = 1/max of it.
    struct Pls { bool on = false; int base = 0, extra = 0; double rechargeS = 25, regenPct = 0, pool = 1; std::uint64_t nextAt = 0, lastTick = 0; std::uint32_t last = 0; bool loading = false; } pls;   // loading: set by OnLoad, cleared after the post-load Sync
    std::string PlsS(const char* k) { return S(g_map.arms.value("pls", json::object()), k); }
    void PlsEquip()
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* w = Obj(PlsS("weapon"));
        auto* em = RE::ActorEquipManager::GetSingleton();
        if (!p || !w || !em) return;
        RE::BGSObjectInstance inst(w, nullptr);
        // the grenade slot EXPLICITLY: with a null slot the game put the PLS in the main hand (in game 2026-11-08: player's drawn
        // weapon = CW_ProjectileLaunchSystem, G threw nothing, attack swung fists)
        auto* gsf = RE::TESForm::GetFormByID(0x00046AAC);   // Fallout4.esm EQUP GrenadeSlot
        auto* gs = gsf ? gsf->As<RE::BGSEquipSlot>() : nullptr;
        bool ok = em->EquipObject(p, inst, 0, 1, gs, false, false, false, true, false);
        REX::INFO("arms: PLS equipped in the grenade slot {} ({})", gs ? "GrenadeSlot" : "NOT FOUND (null slot)", ok ? "ok" : "EquipObject returned false");
    }
    // EquipObject's force argument locks the equipped stack (BGSInventoryItem::Stack kEquipStateLocked) and the save keeps the bit:
    // the Pip-Boy then refuses to swap it, "You cannot unequip this item" (E2, build K2). Nothing here force-equips into the hands
    // or the grenade slot any more. This counts the bits on the player's weapons and, with clear, removes them: all weapons, because
    // our forced equips could lock a vanilla gun too. Armor and other items are not touched. arms::OnLoad clears once per save.
    int EquipLocks(bool clear)
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p || !p->inventoryList) return 0;
        int n = 0;
        RE::BSAutoWriteLock guard{ p->inventoryList->rwLock };
        for (auto& item : p->inventoryList->data) {
            if (!item.object || !item.object->As<RE::TESObjectWEAP>()) continue;
            for (auto* st = item.stackData.get(); st; st = st->nextStack.get()) {
                if (!st->flags.any(RE::BGSInventoryItem::Stack::Flag::kEquipStateLocked)) continue;
                ++n;
                REX::INFO("arms: equip lock on weapon {:08X} (equipped {}){}", item.object->GetFormID(), st->IsEquipped(), clear ? ": cleared" : "");
                if (clear) st->flags.reset(RE::BGSInventoryItem::Stack::Flag::kEquipStateLocked);
            }
        }
        return n;
    }
    // What holds the grenade slot right now, as the throw sees it: "pls", another "grenade" or "mine" (the player's choice wins), or "empty"
    const char* PlsSlotNow(RE::PlayerCharacter* p, const RE::TESBoundObject* w)
    {
        const char* holder = "empty";
        auto* mh = p && p->currentProcess ? p->currentProcess->middleHigh : nullptr;
        if (!mh) return holder;
        for (auto& e : mh->equippedItems) {
            auto* g = e.item.object ? e.item.object->As<RE::TESObjectWEAP>() : nullptr;
            if (!g) continue;
            if (g == w) return "pls";
            const auto t = static_cast<std::int32_t>(g->weaponData.type.get());
            if (t == static_cast<std::int32_t>(RE::WEAPON_TYPE::kGrenade)) holder = "grenade";
            else if (t == static_cast<std::int32_t>(RE::WEAPON_TYPE::kMine)) holder = "mine";
        }
        return holder;
    }
    void PlsTick(std::uint64_t now)
    {
        if (!pls.on) return;
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* w = Obj(PlsS("weapon"));
        if (!p || !w) return;
        const std::uint32_t mx = (std::uint32_t)std::max(1, pls.base + pls.extra);
        std::uint32_t c = p->GetInventoryObjectCount(w);
        if (c > mx) { RE::TESObjectREFR::RemoveItemData rd(w, (std::int32_t)(c - mx)); p->RemoveItem(rd); c = mx; }
        const double dt = pls.lastTick ? (double)(now - pls.lastTick) / 1000.0 : 0.0;
        pls.lastTick = now;
        auto st = cw::PlsPoolStep(pls.pool, (int)c, (int)pls.last, (int)mx, dt, pls.rechargeS, pls.regenPct);
        pls.pool = st.pool;
        if (g_ini.debug && c < pls.last) REX::INFO("arms: PLS charge spent ({} -> {})", pls.last, c);
        if (st.add > 0) {
            p->AddObjectToContainer(w, RE::BSTSmartPointer<RE::ExtraDataList>{}, st.add, nullptr, RE::ITEM_REMOVE_REASON::kNone);
            c += (std::uint32_t)st.add;
            if (g_ini.debug) REX::INFO("arms: PLS recharged ({}/{}, pool {:.0f}%)", c, mx, pls.pool * 100);
            hud::Event({ { "type", "plsCharge" }, { "charges", c }, { "max", mx } });
        }
        if (pls.last == 0 && c > 0) {   // the grenade slot empties at 0: put it back on the first charge, unless the player has
            const std::string_view holder = PlsSlotNow(p, w);   // a real grenade equipped there meanwhile wins; the PLS waits in the inventory
            const bool other = holder == "grenade" || holder == "mine";
            if (!other) PlsEquip();
            else if (g_ini.debug) REX::INFO("arms: PLS recharged but a grenade is equipped: left in the inventory");
        }
        pls.last = c;
    }
    // PLS rounds: the projectile's explosion (one CW_PLSProjectile, so the swap covers every charge)
    void PlsRound(const std::string& round)
    {
        auto* pf = g_map.Get(PlsS("projectile"));
        auto* proj = pf ? pf->As<RE::BGSProjectile>() : nullptr;
        std::string ex = S(g_map.arms.value("pls", json::object()).value("explosions", json::object()), round.c_str());
        auto* ef = g_map.Get(ex);
        auto* expl = ef ? ef->As<RE::BGSExplosion>() : nullptr;
        if (!proj || !expl) { REX::WARN("arms: PLS round {} not available (projectile {}, explosion {})", round, (void*)proj, (void*)expl); return; }
        proj->data.explosionType = expl;
        REX::INFO("arms: PLS round {} -> {}", round, ex);
    }

    // The arm WEAPs are full copies of DeathclawGauntlet / PowerFist, so they keep object templates: an unequipped stack
    // holds no computed copy (the engine builds it from the base form at equip), but an EQUIPPED one can hold its own
    // InstanceData (ARMS review of cf7553e0). A write to the base form is therefore repeated on the player's equipped
    // instance of that WEAP. Returns how many instances were written.
    int ForEquippedArmInstance(RE::TESObjectWEAP* w, const std::function<void(RE::TESObjectWEAP::InstanceData&)>& fn)
    {
        int n = 0;
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* mh = p && p->currentProcess ? p->currentProcess->middleHigh : nullptr;
        if (!mh || !w) return 0;
        for (auto& e : mh->equippedItems)
            if (e.item.object && e.item.object->As<RE::TESObjectWEAP>() == w && e.item.instanceData) {
                auto* d = static_cast<RE::TESObjectWEAP::InstanceData*>(e.item.instanceData.get());
                if (d != static_cast<RE::TESObjectWEAP::InstanceData*>(&w->weaponData)) { fn(*d); ++n; }
            }
        return n;
    }
    // Rotors: the arm WEAP's base speed x (1 + attack_speed_pct/100), written to the base form and to an equipped
    // instance that holds its own data (ForEquippedArmInstance).
    std::map<std::string, float> baseSpeed;
    void SetArmSpeed(const std::string& weap, double mult)
    {
        auto* f = g_map.Get(weap);
        auto* w = f ? f->As<RE::TESObjectWEAP>() : nullptr;
        if (!w) return;
        if (!baseSpeed.count(weap)) baseSpeed[weap] = w->weaponData.speed;
        w->weaponData.speed = (float)(baseSpeed[weap] * mult);
        const int inst = ForEquippedArmInstance(w, [&](RE::TESObjectWEAP::InstanceData& d) { d.speed = w->weaponData.speed; });   // ARMS review F2
        REX::INFO("arms: {} speed {:.2f} (x{:.2f}){}", weap, w->weaponData.speed, mult, inst ? ", equipped instance too" : "");
    }

    // Tier damage (BETA QA 2, 2026-11-10; the lead's order). The catalog's base_dmg for the installed tier
    // (cw::ArmWeaponDamage) is the arm WEAP's damage: before this the record carried its own number at every tier, so an
    // upgrade bought no hit damage and a tier-1 arm out-hit late vanilla gear. Written at install, upgrade and load (the
    // enable handler runs for all three: SyncNative turns a tier change into disable + enable); the record's value goes
    // back at uninstall and on a load's reset, because the forms outlive the session. A base WEAP carries it as
    // attackDamage; an element variant (espgen: base 1 + typed damage) in its typed damage, so the split stays
    // 1 + (dmg - 1). An equipped instance that holds its own copy of the data is written too (the base form alone is
    // enough only while the arm WEAPs carry no OMOD, which SetArmSpeed assumes). dmg <= 0 = back to the record.
    struct ArmDmgRec { std::uint16_t attack = 0; std::uint32_t typed = 0; bool variant = false; };
    std::map<std::string, ArmDmgRec> baseArmDamage;
    void SetArmDamage(const std::string& weap, int dmg)
    {
        auto* f = g_map.Get(weap);
        auto* w = f ? f->As<RE::TESObjectWEAP>() : nullptr;
        if (!w) return;
        auto typedOf = [](RE::TESObjectWEAP::InstanceData& d) -> std::uint32_t* { return d.damageTypes && !d.damageTypes->empty() ? &(*d.damageTypes)[0].second.i : nullptr; };
        if (!baseArmDamage.count(weap)) {
            ArmDmgRec rec;
            rec.attack = w->weaponData.attackDamage;
            if (auto* t = typedOf(w->weaponData)) { rec.typed = *t; rec.variant = rec.attack <= 1; }
            baseArmDamage[weap] = rec;
        }
        const ArmDmgRec r = baseArmDamage[weap];
        const std::uint16_t attack = dmg > 0 ? (r.variant ? (std::uint16_t)1 : (std::uint16_t)std::min(dmg, 65535)) : r.attack;
        const std::uint32_t typed = dmg > 0 ? (std::uint32_t)std::max(1, dmg - 1) : r.typed;
        auto write = [&](RE::TESObjectWEAP::InstanceData& d) {
            d.attackDamage = attack;
            if (r.variant) if (auto* t = typedOf(d)) *t = typed;
        };
        write(w->weaponData);
        const int inst = ForEquippedArmInstance(w, write);
        REX::INFO("arms: {} damage {}{} ({}{})", weap, r.variant ? 1 + (int)typed : (int)attack, r.variant ? " = 1 + typed" : "", dmg > 0 ? "tier" : "record",
                  inst ? ", equipped instance too" : "");
    }
    // The launcher follows the same rule: espgen bakes the T5 base_dmg into every lethal CW_PLSExplosion_*, and the
    // installed tier's value replaces it while the PLS is installed. The tranquilizer round stays at 0. The radius does
    // not change with the tier (the catalog keeps explosion_radius_m flat).
    std::map<std::string, float> basePlsDamage;
    void SetPlsDamage(int dmg)
    {
        const json rounds = g_map.arms.value("pls", json::object()).value("explosions", json::object());   // a named copy: items() on a temporary dangles
        int n = 0;
        float shown = 0;
        for (auto& [round, v] : rounds.items()) {
            if (!v.is_string()) continue;
            const std::string ex = v.get<std::string>();
            auto* ef = g_map.Get(ex);
            auto* expl = ef ? ef->As<RE::BGSExplosion>() : nullptr;
            if (!expl) continue;
            if (!basePlsDamage.count(ex)) basePlsDamage[ex] = expl->data.damage;
            if (basePlsDamage[ex] <= 0) continue;
            expl->data.damage = dmg > 0 ? (float)dmg : basePlsDamage[ex];
            shown = expl->data.damage;
            ++n;
        }
        REX::INFO("arms: PLS explosion damage {:.0f} on {} lethal round(s) ({})", shown, n, dmg > 0 ? "tier" : "record");
    }

    // Gorilla ground slam (ARMS): CW_GorillaSlamReady while the Gorilla Arms are drawn in the air (selects
    // cybergorillaslam_1p); a swing that starts then is a slam; its HitFrame (or the landing, if still airborne) damages
    // and knocks back hostiles within radiusUnits. Removed on landing (unless a slam is under way) or at AttackStop.
    struct Slam { bool ready = false, swinging = false, pending = false, driving = false, measured = false; float startZ = 0;
                  bool repicking = false, buffered = false; std::uint64_t repickAt = 0, replayAt = 0; int replays = 0; } slam;
    // The slam re-pick's attack buffer (board ea3f3716, P13c T5b). Jumping with the Gorilla drawn arms READY and re-picks the
    // weapon subgraph: that plays an equip clip (09.818 -> initiateEnd 10.299 in T5b) and the graph DROPS an attack pressed
    // inside it, while a jump lasts about 0.9 s. So a quick jump + punch did nothing. An attack pressed from READY until the
    // re-pick's initiateEnd is kept and replayed through the engine's own action (ActionRightAttack, as the test seam) the
    // moment the graph is back, if the player is still airborne or within the landing coyote time. Nothing is consumed.
    bool PerformPlayerAction(const char* edid)
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* f = RE::TESForm::GetFormByEditorID(RE::BSFixedString(edid));
        auto* act = f ? f->As<RE::BGSAction>() : nullptr;
        if (!p || !act) return false;
        return voice::PlayIdleAction(p, act);   // legacy-safe dispatch: the commonlibf4 template crashed P14 (cw_voice.cpp)
    }
    void OnAttackPressedImpl()
    {
        if (g_ini.debug && slam.ready)
            REX::INFO("arms: ground slam: attack seen (re-pick {}, swinging {}, {} ms after READY)", slam.repicking ? "pending" : "done", slam.swinging, GetTickCount64() - slam.repickAt);
        if (!slam.ready || !slam.repicking || slam.swinging) return;
        if (GetTickCount64() - slam.repickAt > 1200) { slam.repicking = false; return; }   // no initiateEnd came: stale
        if (!slam.buffered && g_ini.debug) REX::INFO("arms: ground slam: attack pressed during the re-pick, kept");
        slam.buffered = true;
    }
    void LandingShake(RE::PlayerCharacter* p, float strength, float secs);   // below, with the leap
    bool Airborne(RE::Actor* p)
    {
        auto* mh = p && p->currentProcess ? p->currentProcess->middleHigh : nullptr;
        RE::bhkCharacterController* cc = mh ? mh->charController.get() : nullptr;
        return p && (p->IsJumping() || (cc && cc->fallTime > 0.1f));
    }
    void SetSlamReady(bool on)
    {
        if (on == slam.ready) return;
        if (!g_ini.leapRepick) {   // bSlamNoRepick: the slam's effect (DoSlam on landing) on the vanilla jump attack; no keyword, no re-pick
            slam.ready = on;
            if (g_ini.debug) REX::INFO("arms: ground slam {} (effect only, no re-pick)", on ? "READY" : "off");
            return;
        }
        std::string k = S(g_map.arms.value("slam", json::object()), "keyword");
        if (k.empty()) return;
        SetKeyword(k, on);
        slam.ready = on;
        slam.repicking = on; slam.buffered = false; slam.repickAt = GetTickCount64(); slam.replayAt = 0; slam.replays = 0;
        RequestRepick(on ? "slam ready" : "slam off");
        if (g_ini.debug) REX::INFO("arms: ground slam {}", on ? "READY" : "off");
    }
    bool IsHostile(RE::Actor* a, RE::Actor* b);
    // Gorilla Arms knockback (the author, 2026-11-08, combat feel): a power punch (or the ground slam) ragdolls the target
    // away with AIProcess::KnockExplosion, magnitude from arms.combat.gorillaKnockback.ragdollByTier[tier-1]. Essential
    // actors (bosses) get the smaller staggerMagnitude so they stumble rather than fly. Normal punches keep the vanilla
    // PowerFist stagger. Data-driven (weapons_arms.json combat.gorillaKnockback -> espgen -> formmap arms.combat).
    const json& CombatCfg() { static const json empty = json::object(); auto it = g_map.arms.find("combat"); return it != g_map.arms.end() ? *it : empty; }
    int GorillaTier() { int tier = 5; for (auto& [id, n] : g_activeNative) if (n.handler == "ar_gorilla") tier = n.tier; return std::clamp(tier, 1, 5); }
    bool InPowerArmor(RE::Actor* t) { return t && t->extraList && t->extraList->HasType(RE::EXTRA_DATA_TYPE::kPowerArmor); }
    double GorillaKnockMag(RE::Actor* t, const json& kb)   // KnockExplosion magnitude; essential/boss/power-armor targets get the stagger value
    {
        const bool noRag = (kb.value("skipRagdollEssential", true) && t && t->GetNPC() && t->GetNPC()->IsEssential())
                        || (kb.value("skipRagdollPowerArmor", true) && InPowerArmor(t));   // a raider in PA should stagger, not fly
        if (noRag) return kb.value("staggerMagnitude", 1.0);
        const json by = kb.value("ragdollByTier", json::array());
        std::vector<double> table;
        if (by.is_array()) for (auto& v : by) if (v.is_number()) table.push_back(v.get<double>());
        // past the table = its last entry, never the stagger value: a T5+ Gorilla lost its knockback that way (ARMS review R2)
        return cw::ArmLadderAt(table, GorillaTier(), kb.value("staggerMagnitude", 1.0));
    }
    void DoSlam()
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        const json sj = g_map.arms.value("slam", json::object());
        if (!p) return;
        double radius = sj.value("radiusUnits", 350.0), dmg = sj.value("damage", 60.0), knock = sj.value("knock", 1.0);
        int tier = 5;
        for (auto& [id, n] : g_activeNative) if (n.handler == "ar_gorilla") tier = n.tier;
        dmg *= cw::ArmTierFactor(tier);   // the arm tier effect factor: T1 0.4 .. T5 1.0, T5+ 1.1, T5++ 1.2 (it stopped at T5: ARMS review R1)
        auto pp = p->GetPosition();
        const float drop = slam.measured ? std::max(0.0f, slam.startZ - pp.z) : 0.0f;   // world z can be negative: a flag, not 0
        slam.driving = false; slam.measured = false;
        LandingShake(p, std::clamp(0.55f + drop / 300.0f, 0.55f, 1.0f), 0.45f);   // GOR-4: the impact reads on the camera too
        REX::INFO("arms: slam impact after a {:.0f} u drive-down", drop);
        confirm::Mark("slamImpact");   // TEST HARNESS: the slam's confirmation
        auto* hp = FindAV("Health");
        const bool knockAll = Active("perk_limiter_removal");   // CP: Limiter Removal makes the shockwave knock down all in range
        int hit = 0;
        if (auto* pl = RE::ProcessLists::GetSingleton())
            for (auto& h : pl->highActorHandles) {
                auto a = h.get();
                if (!a || a.get() == p || a->lifeState != 0 || !IsHostile(a.get(), p)) continue;
                auto ap = a->GetPosition();
                double dx = ap.x - pp.x, dy = ap.y - pp.y, dz = ap.z - pp.z;
                if (dx * dx + dy * dy + dz * dz > radius * radius) continue;
                if (hp) static_cast<RE::ActorValueOwner*>(a.get())->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *hp, (float)-dmg);
                // The slam ragdolls everyone it hits (the author, 2026-11-08): tier knockback magnitude, except essential/boss
                // who get the stagger value; Limiter Removal still forces its own knockdown too. [Arms] bKnockback=0
                // falls back to the original behaviour (only Limiter Removal knocks down).
                if (a->currentProcess && (g_ini.knockback || knockAll)) {
                    const json kb = CombatCfg().value("gorillaKnockback", json::object());
                    const double mag = g_ini.knockback ? (knockAll ? std::max(knock, GorillaKnockMag(a.get(), kb)) : GorillaKnockMag(a.get(), kb)) : knock;
                    a->currentProcess->KnockExplosion(a.get(), pp, (float)mag);
                }
                ++hit;
            }
        snd::PlayAt("CW_SND_GorillaSlam", pp);   // SOUND: CP slam impact (GOR-4's sound half)
        REX::INFO("arms: ground slam t{} {:.0f} dmg, {} hostile(s) within {:.0f}{}", tier, dmg, hit, radius, knockAll ? ", knocked down (Limiter Removal)" : "");
        // the shockwave visual (the lead, 2026-11-08): CW_GorillaSlamShockwave (damage / force 0) placed at the player's feet.
        // UNVERIFIED IN GAME: TESDataHandler::CreateReferenceAtLocation with a BGSExplosion base.
        const std::string ex = S(sj, "explosion");
        auto* ef = ex.empty() ? nullptr : g_map.Get(ex);
        auto* expl = ef ? ef->As<RE::BGSExplosion>() : nullptr;
        if (expl) {
            RE::NEW_REFR_DATA d;
            d.location = pp;
            d.direction = RE::NiPoint3{ 0.0f, 0.0f, 0.0f };
            d.object = expl;
            d.interior = p->GetParentCell();
            d.world = d.interior ? d.interior->worldSpace : nullptr;
            auto h = RE::TESDataHandler::GetSingleton()->CreateReferenceAtLocation(d);
            auto ref = h.get();
            REX::INFO("arms: slam shockwave {} {}", ex, ref ? "placed" : "NOT placed (no reference back)");
        } else REX::WARN("arms: slam shockwave explosion '{}' not available", ex);
    }

    void GorillaKnockbackImpl(std::uint32_t targetId, const std::vector<std::string>& kws, bool power)
    {
        if (!g_ini.knockback) return;   // kill switch
        const json kb = CombatCfg().value("gorillaKnockback", json::object());
        const std::string wk = kb.value("weaponKeyword", std::string("CW_AnimsCyberGorilla"));
        if (std::find(kws.begin(), kws.end(), wk) == kws.end()) return;
        if (!power) {   // normal punches keep the vanilla PowerFist stagger; say so, so a test can tell "no power flag" from "no hit"
            // always logged (cw-int-3 T2: a real x1 power punch gave no knockback line and no way to tell why)
            REX::INFO("arms: gorilla hit {:08X} without the power-attack flag (no knockback)", targetId);
            return;
        }
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* tf = RE::TESForm::GetFormByID(targetId);
        auto* t = tf ? tf->As<RE::Actor>() : nullptr;
        if (!p || !t || !t->currentProcess || t->lifeState != 0) return;
        const double mag = GorillaKnockMag(t, kb);
        auto pp = p->GetPosition();
        t->currentProcess->KnockExplosion(t, pp, (float)mag);   // throws the target away from the player
        REX::INFO("arms: gorilla power knockback {:08X} mag {:.1f} (tier {})", targetId, mag, (int)GorillaTier());
    }

    // ---- CP finishers (ARMS finishers.json via formmap arms.finishers; the lead: behind [Arms] bFinishers). Two dynamic
    // idles started together: the lead on the player (the 1P or 3P record for the current camera), the victim on the
    // target, after placing it at the finisher's offset facing the player. The victim dies at killSeconds (our own timer:
    // its KillActor annotation is on ITS graph, which no sink watches). UNVERIFIED IN GAME: PlayIdle of a parentless
    // dyn_Activation IDLE on the player in 1P, the placement, and KillImpl attribution.
    struct Fin { bool active = false, killed = false, buffed = false; int rebuffs = 0; std::uint64_t killAt = 0, endAt = 0; std::uint32_t victim = 0; std::string id;
                 // bFinisherFloor / iFinisherSlideMs / bFinisherPin (FINISHERS 2026-11-10)
                 bool floor = false; float floorHp = 0;            // the health the victim is held on for the set piece
                 cw::Placed placed; bool placedOk = false;         // where it was put, for the pin and the slide
                 double fromX = 0, fromY = 0, fromH = 0;           // where it stood before the slide
                 int slideStep = 0, slideSteps = 0; std::uint64_t slideNextAt = 0, slideStepMs = 0;
                 double maxDrift = 0; int pins = 0, floors = 0; bool pinSaid = false;   // evidence for rows F2a / F1c
                 double rmX = 0, rmY = 0, victimS = 0, headingRad = 0;   // the victim clip's own root motion, so the pin follows the choreography
                 std::uint64_t startAt = 0;
    } fin;
    // the lead clip's HitFrames are real weapon hits: without a buffer the victim died ~0.5 s in and ragdolled out of the
    // choreography (in game 2026-11-08). The victim holds a temporary Health buffer until killSeconds.
    constexpr float kFinBuffer = 100000.0f;
    // The buffer is checked, not assumed (cw-int-1, 2026-11-08): a console `resurrect` issued in the same tick as the
    // finisher is applied LATER by the engine and resets the victim's actor values, wiping the buffer, so Mantis01's lead
    // HitFrame killed its victim at 0.6 s and FinUnbuff then took 100000 off the corpse (hp -44487%), which made the next
    // finisher on it refuse. So: FinBuff verifies the raise, FinisherFrame re-asserts a lost buffer, and a dead victim is
    // never un-buffed.
    float FinHp(RE::Actor* v) { auto* hp = FindAV("Health"); return (v && hp) ? static_cast<RE::ActorValueOwner*>(v)->GetActorValue(*hp) : 0.0f; }
    bool FinBuff(RE::Actor* v)
    {
        auto* hp = FindAV("Health");
        if (!v || !hp) return false;
        const float before = FinHp(v);
        static_cast<RE::ActorValueOwner*>(v)->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, *hp, kFinBuffer);
        fin.buffed = FinHp(v) - before > kFinBuffer * 0.5f;
        if (!fin.buffed) {   // the raise did not take: undo it so a later re-assert cannot stack, and retry next frame
            static_cast<RE::ActorValueOwner*>(v)->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, *hp, -kFinBuffer);
            REX::WARN("arms: finisher {} health buffer did not take on {:08X} (hp {:.0f}), retrying", fin.id, v->GetFormID(), before);
        }
        return fin.buffed;
    }
    void FinUnbuff(RE::Actor* v)
    {
        if (!fin.buffed || !v) return;
        fin.buffed = false;
        if (v->lifeState != 0) return;   // never push a corpse to -100000 (it refuses the next idle)
        if (auto* hp = FindAV("Health")) static_cast<RE::ActorValueOwner*>(v)->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, *hp, -kFinBuffer);
    }

    // ---- [Arms] bFinisherFloor: the hold that writes no damage field (FINISHERS 2026-11-10, the lead's condition 1).
    //
    // Why a scope guard and not the standing buffer. The old design left +100000 Health on a live actor for the whole set
    // piece, so every exit had to remember to take it off: PreSave stripped it and PostSave put it back, FinisherFrame
    // re-asserted it when a deferred `resurrect` wiped it, and FinUnbuff refused to touch a corpse in case it went to
    // -100000. An enemy left alive with 100000 Health is worse than no finisher at all, and that risk existed on every
    // frame of the clip.
    //
    // The lift exists only between two adjacent statements of ONE Actor::DoHitMe call. The game thread is inside that call
    // from the moment it is applied to the moment the destructor removes it, and FO4 saves, cell changes, unloads, menus,
    // reloads and PlayIdle all run on that same thread, so none of them can observe the lift or interrupt it. The
    // destructor runs on every path out, including an early return or a throw. So the whole class of exits the lead
    // listed -- the finisher never starts, it is interrupted, the victim unloads or the cell changes, a save is made and
    // loaded mid-finisher, the victim is essential, two finishers are decided in one frame, the game closes -- cannot
    // leave a lift behind, because none of them can happen while one is applied. g_finLifts proves it rather than
    // asserting it: it is incremented on apply and decremented in the destructor, so it reads 0 anywhere else, and the
    // audit also reads the victim's real kTemporary Health modifier back out of the engine.
    //
    // Between hits the victim simply sits on the health it had when the finisher was decided (fin.floorHp), restored with
    // the engine's own RestoreActorValue -- the stimpak path. That leaves ordinary game state with no modifier of ours in
    // it, so a save taken mid-finisher holds a normal enemy and nothing has to be stripped.
    constexpr float kFinLift = 100000.0f;   // 31x the worst figure the engine has ever been seen to apply (3222.1 of a written 39.8)
    std::atomic<int> g_finLifts{ 0 };                                    // applied right now: 0 everywhere outside one DoHitMe call
    std::atomic<std::uint32_t> g_finLiftsEver{ 0 }, g_finLiftsFailed{ 0 }, g_finLiftDeaths{ 0 };
    struct FinLift {
        RE::Actor* v = nullptr;
        RE::ActorValueInfo* hp = nullptr;
        bool on = false;
        FinLift(RE::Actor* a, RE::ActorValueInfo* h) : v(a), hp(h)
        {
            if (!v || !hp) return;
            auto* o = static_cast<RE::ActorValueOwner*>(v);
            const float before = o->GetActorValue(*hp);
            o->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, *hp, kFinLift);
            on = o->GetActorValue(*hp) - before > kFinLift * 0.5f;
            if (!on) {   // the raise did not take: undo it so nothing can stack, and let the caller fall back
                o->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, *hp, -kFinLift);
                ++g_finLiftsFailed;
                return;
            }
            ++g_finLifts;
            ++g_finLiftsEver;
        }
        ~FinLift() { Drop(); }
        FinLift(const FinLift&) = delete;
        FinLift& operator=(const FinLift&) = delete;
        // Always removed, corpse or not: the invariant "no lift outlives its call" outranks the old worry about a corpse
        // reading very negative, which only mattered when a later finisher could be refused on the same actor.
        void Drop()
        {
            if (!on) return;
            on = false;
            if (v->lifeState != 0) ++g_finLiftDeaths;   // the engine applied more than kFinLift: that is itself a finding
            static_cast<RE::ActorValueOwner*>(v)->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, *hp, -kFinLift);
            --g_finLifts;
        }
    };
    // ---- The QUALITY DIRECTOR's option 2 (2026-11-10): the blade-hand-to-chest distance, per GAME frame, across the two
    // actors. It rejected a 15 u bar resting on a straight-line reading of the victim's root motion for the two clips whose
    // victims travel by design ("a curved root path against a straight line can hide or invent several units, and those are
    // the clips where sliding would show most"), and offered this instead. It is the better measurement anyway: it reads
    // what FIN-2 actually asks -- is the blade on or in the victim -- and it depends on no model of the choreography at all.
    // The node names are tried in order and the pair actually used is logged once, so the line is self-documenting if a
    // skeleton names them differently.
    struct FinContact { RE::NiAVObject* blade = nullptr; RE::NiAVObject* chest = nullptr; bool said = false; float minD = 1e9f, maxD = 0; } fincontact;
    RE::NiAVObject* FirstNode(RE::NiAVObject* root, std::initializer_list<const char*> names, const char*& used)
    {
        if (!root) return nullptr;
        for (const char* n : names)
            if (auto* o = root->GetObjectByName(RE::BSFixedString(n))) { used = n; return o; }
        return nullptr;
    }
    // Put the victim back on its set-piece health with the engine's own heal, so no modifier of ours is left in the save.
    void FinFloorRestore(RE::Actor* v, const char* why)
    {
        auto* hp = FindAV("Health");
        if (!fin.floor || !v || !hp || v->lifeState != 0 || fin.floorHp <= 0) return;
        auto* o = static_cast<RE::ActorValueOwner*>(v);
        const float cur = o->GetActorValue(*hp);
        if (cur >= fin.floorHp - 0.5f) return;
        o->RestoreActorValue(*hp, fin.floorHp - cur);
        ++fin.floors;
        if (g_ini.debug)
            REX::INFO("arms: finisher floor: {:08X} {:.1f} -> {:.1f} ({}), lifts outstanding {}", v->GetFormID(), cur, o->GetActorValue(*hp), why, g_finLifts.load());
    }
    // The audit line the lead asked for. Reads the lift counter AND the victim's real kTemporary Health modifier back
    // out of the engine, so the invariant is measured and not merely claimed.
    void FinLiftAudit(const char* when, std::uint32_t victimId)
    {
        float leftOn = 0;
        if (auto* hp = FindAV("Health"))
            if (auto* f = victimId ? RE::TESForm::GetFormByID(victimId) : nullptr)
                if (auto* v = f->As<RE::Actor>()) leftOn = static_cast<RE::ActorValueOwner*>(v)->GetModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, *hp);
        const int out = g_finLifts.load();
        if (out != 0 || std::fabs(leftOn) > kFinLift * 0.5f)
            REX::WARN("arms: finisher buffers outstanding: {} ({}; victim {:08X} temporary Health modifier {:.0f}, lifts {} failed {} deaths-under-lift {})",
                      out, when, victimId, leftOn, g_finLiftsEver.load(), g_finLiftsFailed.load(), g_finLiftDeaths.load());
        else
            REX::INFO("arms: finisher buffers outstanding: 0 ({}; victim {:08X} temporary Health modifier {:.0f}, lifts {} failed {} deaths-under-lift {})",
                      when, victimId, leftOn, g_finLiftsEver.load(), g_finLiftsFailed.load(), g_finLiftDeaths.load());
    }
    std::uint64_t finNextOk = 0;
    std::uint32_t finHeldFor = 0;   // the victim whose buffer is stripped for the save in progress
    cw::WeakPoints weak;                                       // Vulnerability Analytics / Machine Learning (WeakPointHit below)
    cw::RuntimeMods rmods;   // every runtime modifier on the player (stripped before a save, restored after it)
    // Equipped weapon instance scaling (the lead, 2026-11-08): reload_pct -> reloadSpeed, Microrotors melee_speed_pct ->
    // speed (melee / unarmed only). Only a real instance (never the shared base form). Restored on unequip, load and save.
    struct WeapScale {
        RE::TESObjectWEAP::InstanceData* inst = nullptr;
        RE::BSTSmartPointer<RE::TBO_InstanceData> hold;   // keeps the instance alive while we track it (dropped / sold weapon)
        float origReload = 0, origSpeed = 0, mReload = 1, mSpeed = 1;
    } wscale;
    // ledger keys are "<AV EditorID>@temp" or "<AV>@perm" (kTemporary / kPermanent on the player)
    void ApplyLedger(const std::string& key, double delta)
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto at = key.find('@');
        auto* av = at == std::string::npos ? nullptr : FindAV(key.substr(0, at));
        if (!p || !av || delta == 0) return;
        auto kind = key.substr(at + 1) == "perm" ? RE::ACTOR_VALUE_MODIFIER::kPermanent : RE::ACTOR_VALUE_MODIFIER::kTemporary;
        static_cast<RE::ActorValueOwner*>(p)->ModActorValue(kind, *av, (float)delta);
    }
    const cw::NativeAbility* Find(const char* h) { for (auto& [id, n] : g_activeNative) if (n.handler == h) return &n; return nullptr; }
    // seconds a handler's timed effect still runs (Engine::NativeView), 0 when it is not running
    double TimerLeft(const char* handler)
    {
        if (!g_engine) return 0;
        std::string ab;
        for (auto& [id, n] : g_activeNative) if (n.handler == handler) ab = id;
        if (ab.empty()) return 0;
        for (auto& t : g_engine->NativeView()) if (t.value("ability", "") == ab && t.value("active", false)) return t.value("secondsActive", 0.0);
        return 0;
    }
    std::uint32_t weakShownFor = 0; int weakShownGroup = -2;
    bool IsHuman(RE::Actor* a)
    {
        bool h = false;
        if (a && a->race)
            a->race->ForEachKeyword([&](RE::BGSKeyword* k) {
                if (k && k->formEditorID.c_str() && !std::strcmp(k->formEditorID.c_str(), "ActorTypeHuman")) { h = true; return RE::BSContainer::ForEachResult::kStop; }
                return RE::BSContainer::ForEachResult::kContinue;
            });
        return h;
    }
    bool PlayIdleOn(RE::Actor* a, const std::string& edid, RE::TESObjectREFR* target)
    {
        auto* f = g_map.Get(edid);
        auto* idle = f ? f->As<RE::TESIdleForm>() : nullptr;
        if (!a || !idle || !a->currentProcess) { REX::WARN("arms: finisher idle {} not available", edid); return false; }
        bool ok = a->currentProcess->PlayIdle(*a, idle, target);
        REX::INFO("arms: PlayIdle {} on {:08X}: {}", edid, a->GetFormID(), ok ? "ok" : "refused");
        return ok;
    }
    void PlayFinisher(const std::string& id, RE::Actor* t, const json& over, double hpPct);
    void CueFinisherStart(const std::string& id);   // SOUND WIRING, defined with the cue layer below
    void TryFinisherImpl(std::uint32_t targetId, const std::vector<std::string>& kws, bool power, bool staggered, bool sneak)
    {
        const std::uint64_t now = GetTickCount64();
        if (fin.active || now < finNextOk) return;
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* tf = RE::TESForm::GetFormByID(targetId);
        auto* t = tf ? tf->As<RE::Actor>() : nullptr;
        auto* hp = FindAV("Health");
        if (!p || !t || !hp) return;
        // the finishers of the arm that made the hit (its CW_Anims* keyword is on the hitting WEAP)
        std::vector<std::string> ids;
        for (auto& [id, f] : g_map.arms.value("finishers", json::object()).items())
            if (std::find(kws.begin(), kws.end(), S(f, "weaponKeyword")) != kws.end()) ids.push_back(id);
        if (ids.empty()) return;
        auto* o = static_cast<RE::ActorValueOwner*>(t);
        float mx = o->GetPermanentActorValue(*hp);
        cw::FinisherHit h;
        h.human = IsHuman(t); h.power = power; h.staggered = staggered; h.sneak = sneak;
        h.unaware = !t->IsInCombat(); h.alive = t->lifeState == 0;
        h.hpPct = mx > 0 ? o->GetActorValue(*hp) / mx : 1;
        if (!cw::FinisherEligible(h)) {
            if (g_ini.debug) REX::INFO("arms: finisher not eligible on {:08X} (human {} hp {:.2f} unaware {} staggered {} sneak {})", targetId, h.human, h.hpPct, h.unaware, h.staggered, h.sneak);
            return;
        }
        PlayFinisher(ids[now % ids.size()], t, json::object(), h.hpPct);
    }
    // Finisher on the killing blow (the lead's order). TryFinisherImpl runs AFTER the hit and needs the victim
    // alive with hp <= 25%, so a power hit that would kill could never finish (P13c T5c: a clean sever + plain kill instead).
    // With bHitFromDoHitMe the DoHitMe hook decides BEFORE the engine applies the hit, on the hp the hit WOULD leave (a lethal
    // hit counts as 0+, i.e. eligible); the hook then holds the hit to leave the victim at 1 hp, and the finisher's own
    // killAt does the kill. Same rules otherwise (cw::FinisherEligible: human, power, unaware / staggered / sneak).
    bool FinisherBeforeHit(RE::Actor* t, const std::vector<std::string>& kws, bool staggered, bool sneak, float dealt, std::string& id, double& hpPct)
    {
        const std::uint64_t now = GetTickCount64();
        if (fin.active || now < finNextOk) return false;
        auto* hp = FindAV("Health");
        if (!t || !hp || t->lifeState != 0) return false;
        std::vector<std::string> ids;
        for (auto& [fid, f] : g_map.arms.value("finishers", json::object()).items())
            if (std::find(kws.begin(), kws.end(), S(f, "weaponKeyword")) != kws.end()) ids.push_back(fid);
        if (ids.empty()) return false;
        auto* o = static_cast<RE::ActorValueOwner*>(t);
        const float mx = o->GetPermanentActorValue(*hp), cur = o->GetActorValue(*hp);
        cw::FinisherHit h;
        h.human = IsHuman(t); h.power = true; h.staggered = staggered; h.sneak = sneak;
        h.unaware = !t->IsInCombat(); h.alive = true;
        h.hpPct = mx > 0 ? std::max((double)(cur - dealt) / mx, 0.001) : 1.0;
        if (!cw::FinisherEligible(h)) {
            if (g_ini.debug) REX::INFO("arms: finisher not eligible before the hit on {:08X} (human {} hp after {:.2f} unaware {} staggered {} sneak {})", t->GetFormID(), h.human, h.hpPct, h.unaware, h.staggered, h.sneak);
            return false;
        }
        id = ids[now % ids.size()];
        hpPct = h.hpPct;
        return true;
    }
    void StartDecidedFinisherImpl(const std::string& id, std::uint32_t targetId, double hpPct)
    {
        auto* tf = RE::TESForm::GetFormByID(targetId);
        auto* t = tf ? tf->As<RE::Actor>() : nullptr;
        if (!t || t->lifeState != 0) { REX::WARN("arms: finisher {} decided before the hit, but {:08X} is gone or dead by the task", id, targetId); return; }
        if (fin.active) return;
        PlayFinisher(id, t, json::object(), hpPct);
    }
    // place the victim and start both idles; `over` (test seam) may override x / y / yawDeg of finishers.json's placement
    void PlayFinisher(const std::string& id, RE::Actor* t, const json& over, double hpPct)
    {
        const std::uint64_t now = GetTickCount64();
        auto* p = RE::PlayerCharacter::GetSingleton();
        const std::uint32_t targetId = t->GetFormID();
        const json f = g_map.arms["finishers"][id];
        auto pp = p->GetPosition();
        bool first = false;
        if (auto* cam = RE::PlayerCamera::GetSingleton()) first = cam->QCameraEquals(RE::CameraStates::kFirstPerson);
        // finishers.json fits y to CP's contact frames (8-46 u), but in 1P that is under the camera and the victim cannot be seen
        constexpr double kFinisher1PMinY = 70.0;
        double fy = over.value("y", f.value("y", 30.0));
        if (first && !over.contains("y")) fy = std::max(fy, kFinisher1PMinY);
        const double fx = over.value("x", f.value("x", 0.0)), fyaw = over.value("yawDeg", f.value("yawDeg", 180.0));
        auto pl = cw::FinisherPlace(pp.x, pp.y, p->GetHeading(), fx, fy, fyaw);
        const auto vp = t->GetPosition();
        const float vh = t->GetHeading();
        // iFinisherSlideMs: the victim is PULLED into the grab over several frames instead of teleported there in one.
        // QD FN1 failed this on frames 42 to 43: 37 ms apart, the victim about 270 px to the left and about 90 degrees
        // round, with the blades gone -- "the logged placement applied in one step". CP yanks the victim in; it does not
        // cut. The idles start now either way, so the pull and the grab overlap.
        const int slideSteps = g_ini.finisherSlideMs > 0
            ? cw::FinisherSlideSteps(vp.x, vp.y, vh, pl, 35.0, 25.0, 24)
            : 0;
        if (slideSteps <= 0) {
            t->SetPosition(RE::NiPoint3{ (float)pl.x, (float)pl.y, pp.z }, true);
            t->SetHeading((float)pl.heading);
        }
        bool okLead = PlayIdleOn(p, S(f, first ? "lead1p" : "lead3p"), t);
        bool okVic = PlayIdleOn(t, S(f, "victim"), p);
        fin = {};
        fin.active = true; fin.id = id; fin.victim = targetId;
        fin.placed = pl; fin.placedOk = true;
        fincontact = {};
        fin.startAt = now;
        fin.headingRad = p->GetHeading();
        fin.victimS = f.value("victimSeconds", f.value("leadSeconds", 2.0));
        {   // three of the six victim clips end where they began; Mantis02's travels 112 u and Monowire01's 93 u by design
            const json rm = f.value("rootMotionEnd", json::object()).value("victim3p", json::array());
            if (rm.is_array() && rm.size() >= 2) { fin.rmX = rm[0].get<double>(); fin.rmY = rm[1].get<double>(); }
        }
        fin.fromX = vp.x; fin.fromY = vp.y; fin.fromH = vh;
        fin.slideSteps = slideSteps;
        fin.slideStep = 0;
        fin.slideStepMs = slideSteps > 0 ? (std::uint64_t)std::max(1, g_ini.finisherSlideMs / slideSteps) : 0;
        fin.slideNextAt = now;
        if (slideSteps > 0)
            REX::INFO("arms: finisher slide: {} steps of {} ms, {:.0f} u and {:.0f} deg to the placement (max step {:.0f} u)",
                      slideSteps, (int)fin.slideStepMs, std::sqrt((pl.x - vp.x) * (pl.x - vp.x) + (pl.y - vp.y) * (pl.y - vp.y)),
                      std::fabs(std::remainder(pl.heading - vh, 2 * 3.14159265358979323846)) * 180.0 / 3.14159265358979323846,
                      35.0);
        if (g_ini.finisherFloor) {   // the set-piece health: whatever it has now, which the hold just restored
            if (auto* hp = FindAV("Health")) {
                fin.floorHp = static_cast<RE::ActorValueOwner*>(t)->GetActorValue(*hp);
                fin.floor = fin.floorHp > 0;
            }
        } else FinBuff(t);
        fin.killAt = now + (std::uint64_t)(f.value("killSeconds", 2.0) * 1000.0);
        fin.endAt = now + (std::uint64_t)(std::max(f.value("leadSeconds", 2.0), f.value("killSeconds", 2.0)) * 1000.0) + 250;
        finNextOk = fin.endAt + 1000;
        REX::INFO("arms: finisher {} ({}) on {:08X} hp {:.0f}%: lead {}, victim {} (placement x {:.1f} y {:.1f} yaw {:.0f})", id, first ? "1P" : "3P", targetId, hpPct * 100,
                  okLead ? "ok" : "REFUSED", okVic ? "ok" : "REFUSED", fx, fy, fyaw);
        if (okLead) CueFinisherStart(id);   // SOUND WIRING: a cue that has to start with the clip, not on a HitFrame (the Monowire's)
    }
    // test seam: play finisher `id` (or the first one) on actor `targetId` (or the nearest live high-process actor), no eligibility check
    void ForceFinisher(std::string id, std::uint32_t targetId, const json& over)
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        const json all = g_map.arms.value("finishers", json::object());
        if (!p || all.empty()) { REX::WARN("cmd: finisher: no player or no finishers in the formmap"); return; }
        if (id.empty() || !all.contains(id)) { std::string want = id; id = all.begin().key(); if (!want.empty()) REX::WARN("cmd: finisher '{}' unknown, using {}", want, id); }
        RE::Actor* t = nullptr;
        if (targetId) { auto* tf = RE::TESForm::GetFormByID(targetId); t = tf ? tf->As<RE::Actor>() : nullptr; }
        else if (auto* pl = RE::ProcessLists::GetSingleton()) {
            auto pp = p->GetPosition(); double best = 1e18;
            for (auto& hnd : pl->highActorHandles) {
                auto a = hnd.get();
                if (!a || a.get() == p || a->lifeState != 0) continue;
                auto ap = a->GetPosition(); double d = (ap.x - pp.x) * (ap.x - pp.x) + (ap.y - pp.y) * (ap.y - pp.y);
                if (d < best) { best = d; t = a.get(); }
            }
        }
        if (!t) { REX::WARN("cmd: finisher: no target"); return; }
        fin = {}; finNextOk = 0;
        auto* hp = FindAV("Health"); auto* o = static_cast<RE::ActorValueOwner*>(t);
        const float mx = hp ? o->GetPermanentActorValue(*hp) : 0.0f;
        PlayFinisher(id, t, over, mx > 0 && hp ? o->GetActorValue(*hp) / mx : 1.0);
    }
    // The held killing blow's safety net (the lead, 2026-11-09): a lethal hit held at 1 hp for a finisher must never leave the target
    // at 1 hp or immortal. While a finisher runs on that target it owns the kill (its killAt); otherwise, kHeldKillMs after the hit,
    // or once that finisher has ended, a target still alive gets the original blow as a kill credited to the player. Covers a
    // finisher that never started (refused idle, another finisher, the task never ran) or was cut short (ragdoll, menu, VATS).
    struct HeldKill { std::uint32_t target = 0; float damage = 0; std::uint64_t deadline = 0; } heldKill;
    constexpr std::uint64_t kHeldKillMs = 1500;
    void HeldKillFrame(std::uint64_t now)
    {
        if (!heldKill.target || now < heldKill.deadline) return;
        if (fin.active && fin.victim == heldKill.target) { heldKill.deadline = std::max(heldKill.deadline, fin.endAt + 250); return; }   // the finisher owns it
        auto* tf = RE::TESForm::GetFormByID(heldKill.target);
        auto* t = tf ? tf->As<RE::Actor>() : nullptr;
        if (t && t->lifeState == 0) {
            FinUnbuff(t);
            t->KillImpl(RE::PlayerCharacter::GetSingleton(), heldKill.damage, true, false);
            REX::WARN("arms: held killing blow on {:08X} applied ({:.1f}): its finisher did not run or did not finish", heldKill.target, heldKill.damage);
        } else if (g_ini.debug) REX::INFO("arms: held killing blow on {:08X} closed (dead or gone)", heldKill.target);
        heldKill = {};
    }
    void FinisherFrame()
    {
        const std::uint64_t now = GetTickCount64();
        HeldKillFrame(now);
        if (!fin.active) return;
        auto* vf = RE::TESForm::GetFormByID(fin.victim);
        auto* v = vf ? vf->As<RE::Actor>() : nullptr;
        // iFinisherSlideMs: one eased step per tick until the victim is on its placement (FIN-1)
        if (fin.slideSteps > 0 && fin.slideStep < fin.slideSteps && v && v->lifeState == 0 && now >= fin.slideNextAt) {
            ++fin.slideStep;
            fin.slideNextAt = now + fin.slideStepMs;
            const cw::Slide s = cw::FinisherSlideStep(fin.fromX, fin.fromY, fin.fromH, fin.placed, fin.slideStep, fin.slideSteps);
            const auto cur = v->GetPosition();
            v->SetPosition(RE::NiPoint3{ (float)s.x, (float)s.y, cur.z }, true);
            v->SetHeading((float)s.heading);
            if (g_ini.debug && (fin.slideStep == 1 || fin.slideStep == fin.slideSteps))
                REX::INFO("arms: finisher slide: step {}/{} to ({:.0f}, {:.0f})", fin.slideStep, fin.slideSteps, s.x, s.y);
        }
        // bFinisherPin: hold the victim in the blade for the set piece (FIN-2). QD FN1: it was pushed from 75 u to 285 u
        // during one finisher, so the blade was on it for 5 frames of 5.5 s and at the kill it slumped metres away. Only a
        // drift past the bar is corrected, so the victim's own clip still reads.
        // The target is not a fixed point: it is the placement plus the clip's own root motion at this moment, so the two
        // victim clips that travel by design are not fought. QD's bar is 15 u from that choreographed offset (FIN-5).
        constexpr double kFinPinU = 15.0;
        if (fin.placedOk && v && v->lifeState == 0) {
            const double elapsed = (now - fin.startAt) / 1000.0;
            const cw::Placed want = cw::FinisherChoreographed(fin.placed, fin.headingRad, fin.rmX, fin.rmY, elapsed, fin.victimS);
            const auto vp = v->GetPosition();
            const double drift = cw::FinisherDrift(vp.x, vp.y, want);
            // bFinisherTrace: one line per GAME frame. A 28 fps capture can miss a one-frame jump, so the frames alone
            // cannot prove FIN-1 or FIN-5; this line can, and it is what the QUALITY DIRECTOR asked to grade them from.
            // The CONTACT distance (QD option 2): the player's blade hand to the victim's chest, in world space, every game
            // frame. This is the figure that grades FIN-2 and, for the two travelling victims, FIN-5; the straight-line
            // `drift` beside it is reported but the Director ruled it cannot PASS a row for those two.
            float contact = -1.0f;
            if (g_ini.finisherTrace) {
                auto* pc = RE::PlayerCharacter::GetSingleton();
                const char* bn = "?";
                const char* cn = "?";
                RE::NiAVObject* proot = pc ? pc->Get3D(pc && RE::PlayerCamera::GetSingleton() && RE::PlayerCamera::GetSingleton()->QCameraEquals(RE::CameraState::kFirstPerson)) : nullptr;
                auto* blade = FirstNode(proot, { "MantisBladeR:0", "AnimObjectR1", "RArm_Hand" }, bn);
                auto* chest = FirstNode(v->Get3D(), { "Chest", "SPINE2", "Spine2", "Chest_skin" }, cn);
                if (blade && chest) {
                    const auto& x = blade->world.translate;
                    const auto& y = chest->world.translate;
                    contact = std::sqrt((x.x - y.x) * (x.x - y.x) + (x.y - y.y) * (x.y - y.y) + (x.z - y.z) * (x.z - y.z));
                    fincontact.minD = std::min(fincontact.minD, contact);
                    fincontact.maxD = std::max(fincontact.maxD, contact);
                    if (!fincontact.said) { fincontact.said = true; REX::INFO("arms: finisher contact: measuring '{}' (player) to '{}' (victim)", bn, cn); }
                } else if (!fincontact.said) {
                    fincontact.said = true;
                    REX::WARN("arms: finisher contact: no node pair found (player '{}' {}, victim '{}' {}): contact is NOT measured this run",
                              bn, blade ? "ok" : "missing", cn, chest ? "ok" : "missing");
                }
                REX::INFO("arms: finisher trace {} t {:.3f} victim ({:.1f}, {:.1f}) yaw {:.1f} want ({:.1f}, {:.1f}) yaw {:.1f} drift {:.1f} u contact {:.1f} u slide {}/{}",
                          fin.id, elapsed, vp.x, vp.y, v->GetHeading() * 180.0 / 3.14159265358979323846, want.x, want.y,
                          want.heading * 180.0 / 3.14159265358979323846, drift, contact, fin.slideStep, fin.slideSteps);
            }
            // The pin only ever runs on a clip whose victim is MEANT to stay put (rootMotionEnd 0: Mantis01, Monowire02,
            // Gorilla01, Gorilla02). For Mantis02 and Monowire01, whose victims travel 112 u and 93 u by design, the only
            // target available is the straight-line approximation the Director rejected -- and pinning to a wrong path is
            // precisely what would produce the feet sliding with no step under them that it says fails whatever the number.
            // So those two are measured and never moved, and the row says so. A per-frame root track in finishers.json
            // would let them be pinned properly; that is a batch-3 data change, not a guess to ship now.
            const bool travels = fin.rmX != 0 || fin.rmY != 0;
            if (travels && g_ini.finisherPin && !fin.pinSaid) {
                fin.pinSaid = true;
                REX::INFO("arms: finisher {}: its victim travels {:.0f} u by design, so the pin is OFF for this clip and contact is measured only", fin.id, std::sqrt(fin.rmX * fin.rmX + fin.rmY * fin.rmY));
            }
            if (g_ini.finisherPin && !travels && fin.slideStep >= fin.slideSteps) {
                fin.maxDrift = std::max(fin.maxDrift, drift);
                double px = 0, py = 0;
                if (cw::FinisherPin(vp.x, vp.y, want, kFinPinU, px, py)) {
                    ++fin.pins;
                    v->SetPosition(RE::NiPoint3{ (float)px, (float)py, vp.z }, true);
                    if (g_ini.debug) REX::INFO("arms: finisher pin: drift {:.1f} u pulled back to the choreographed offset (pin {})", drift, fin.pins);
                }
            }
        }
        if (fin.floor) {
            FinFloorRestore(v, "frame");   // damage that did not come through our hook (poison, an explosion, a fall)
        } else if (!fin.killed && now < fin.killAt && v && v->lifeState == 0 && fin.rebuffs < 5 && FinHp(v) < kFinBuffer * 0.5f) {
            ++fin.rebuffs;
            const float h = FinHp(v);   // the buffer is gone (a deferred resurrect / AV reset) or never took: re-assert it
            fin.buffed = false;
            if (FinBuff(v)) REX::INFO("arms: finisher {} health buffer was lost (hp {:.0f}), re-applied", fin.id, h);
        }
        if (!fin.killed && now >= fin.killAt) {
            fin.killed = true;
            if (!fin.floor) FinUnbuff(v);
            if (v && v->lifeState == 0) { v->KillImpl(RE::PlayerCharacter::GetSingleton(), 1000.0f, true, false); REX::INFO("arms: finisher {} kill, {} ms after killAt", fin.id, now - fin.killAt); }
            else REX::WARN("arms: finisher {} victim {:08X} was already dead at killSeconds (lifeState {}): killed early, not by the finisher", fin.id, fin.victim, v ? (int)v->lifeState : -1);
        }
        if (now >= fin.endAt) {
            if (!fin.floor) FinUnbuff(v);
            if (fincontact.minD < 1e8f)
                REX::INFO("arms: finisher {} contact: closest {:.1f} u, furthest {:.1f} u", fin.id, fincontact.minD, fincontact.maxD);
            REX::INFO("arms: finisher {} end (max drift {:.1f} u, {} pin(s), {} floor restore(s), {} slide step(s))", fin.id, fin.maxDrift, fin.pins, fin.floors, fin.slideStep);
            FinLiftAudit("finisher end", fin.victim);
            fin.active = false;
        }
    }

    void EndLunge(const char* why)
    {
        if (!lunge.active) return;
        lunge.active = false;   // the keyword stays until the attack ends (Tick): a re-pick now would cut the clip
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (p) {
            auto pp = p->GetPosition();
            double dx = pp.x - lunge.start.x, dy = pp.y - lunge.start.y, dz = pp.z - lunge.start.z;
            REX::INFO("arms: lunge end ({}) at ({:.0f},{:.0f},{:.0f}) travelled {:.0f} u in {} ms", why,
                      pp.x, pp.y, pp.z, std::sqrt(dx * dx + dy * dy + dz * dz), GetTickCount64() - lunge.t0);
        } else
            REX::INFO("arms: lunge end ({})", why);
    }

    // ---- SOUND WIRING (sound/WIRING-SPEC.md). Every cue here is gated by [Sound] bCues (OFF in Cyberware.ini, ON in the test ini).
    enum class CueArm { None = 0, Mantis, Gorilla, Monowire };
    std::atomic<int> g_cueArm{ 0 };          // refreshed every Tick; the graph hook reads it, and that hook may not be on the game thread
    std::atomic<bool> g_cuePls{ false };     // the grenade slot holds the PLS (not a real grenade)
    CueArm CueArmOf(RE::TESObjectWEAP* w)
    {
        if (!w) return CueArm::None;
        if (WeapHas(w, Kw("CW_AnimsCyberMantis"))) return CueArm::Mantis;
        if (WeapHas(w, Kw("CW_AnimsCyberGorilla"))) return CueArm::Gorilla;
        if (WeapHas(w, Kw("CW_AnimsCyberMonowire"))) return CueArm::Monowire;
        return CueArm::None;
    }
    CueArm CueArmOfKws(const std::vector<std::string>& kws)
    {
        for (auto& k : kws) {
            if (k.find("CyberMantis") != std::string::npos) return CueArm::Mantis;
            if (k.find("CyberGorilla") != std::string::npos) return CueArm::Gorilla;
            if (k.find("CyberMonowire") != std::string::npos) return CueArm::Monowire;
        }
        return CueArm::None;
    }
    const char* CueName(CueArm a, const char* mantis, const char* gorilla, const char* mono) { return a == CueArm::Mantis ? mantis : a == CueArm::Gorilla ? gorilla : mono; }
    // The Monowire finishers' cue (found silent by FINISHERS, 2026-11-10). It is a STAND-IN (the lead): the wire-grapple
    // cue, already built and staged, because Cyberpunk's own Monowire finisher sounds (w_melee_finisher_nanowire01 / 02)
    // sit in an audio pack that has not been extracted. It starts WITH the finisher, not on the HitFrame: the wav is a
    // wire throw at 0.1 s and its loud part at 0.6-0.9 s, so from the HitFrame (0.467 s / 0.600 s) the loud part would
    // land 0.7 s after the contact, and from the start it lands 0.13 s / 0.00 s after it. Logged either way: a cue that
    // resolves to nothing is otherwise invisible, which is how the gap went unseen.
    void CueFinisherStart(const std::string& id)
    {
        if (id.rfind("Monowire", 0) != 0) return;
        REX::INFO("snd: finisher {} start cue 'CW_SND_MonowireGrapple' (bCues {})", id, snd::Cues() ? "on" : "off, nothing plays");
        snd::Cue("CW_SND_MonowireGrapple");
    }
    std::uint64_t cueDrawMs = 0, cueSheatheMs = 0, cueHitMs = 0;
    std::set<int> cueDrawnOnce;   // arms MANUALLY drawn since this load: the first manual draw plays FirstEquip (the lead
                                  // 2026-11-09: the load/install auto-draw is not the first draw)
    std::uint64_t cueAutoDrawUntil = 0;   // a draw inside this window is the game's own (load or install auto-equip):
                                          // it plays plain Equip and does not consume the first-draw flag
    void CueAutoEquipSoon() { cueAutoDrawUntil = GetTickCount64() + 8000; }
    bool cueBerserk = false;      // Berserk's on/off edge
    void CueOnLoad() { cueDrawMs = cueSheatheMs = cueHitMs = 0; cueDrawnOnce.clear(); cueBerserk = false; cueAutoDrawUntil = GetTickCount64() + 8000; snd::ChargeForget(); }

    // A hit by a Cyberware arm: one cue at the target, at most one per 70 ms so a multi-target cleave stays one sound.
    // A finisher's own HitFrames have their cue in OnAnimEvent, so this stays out of them.
    void CueHit(const std::vector<std::string>& kws, std::uint32_t target, bool power)
    {
        if (!snd::Cues() || fin.active) return;
        const CueArm arm = CueArmOfKws(kws);
        if (arm == CueArm::None) return;
        const std::uint64_t now = GetTickCount64();
        if (now - cueHitMs < 70) return;
        cueHitMs = now;
        auto* tf = RE::TESForm::GetFormByID(target);
        auto* a = tf ? tf->As<RE::Actor>() : nullptr;
        auto* sp = RE::PlayerCharacter::GetSingleton();
        if (!a && !sp) return;
        const RE::NiPoint3 pos = a ? a->GetPosition() : sp->GetPosition();
        bool machine = false;
        if (a) for (auto* fk : { "Fallout4.esm|02CB73", "Fallout4.esm|0B2BF3" }) {   // ActorTypeRobot / Turret
            auto* fm = ResolveFormKey(fk);
            auto* kw = fm ? fm->As<RE::BGSKeyword>() : nullptr;
            if (kw && ((a->race && static_cast<const RE::BGSKeywordForm*>(a->race)->HasKeyword(kw)) ||
                       (a->GetNPC() && static_cast<const RE::BGSKeywordForm*>(a->GetNPC())->HasKeyword(kw)))) machine = true;
        }
        if (arm == CueArm::Gorilla) { snd::CueAt(power ? "CW_SND_GorillaPowerHit" : "CW_SND_GorillaHit", pos); return; }
        snd::CueAt(machine ? "CW_SND_MantisHitMetal" : "CW_SND_MantisHitFlesh", pos);   // Monowire has no own hit slot
        if (arm == CueArm::Mantis && power) snd::CueAt("CW_SND_MantisSwingHeavy", pos);
    }

    // The anim-driven cues. Runs at the end of OnAnimEvent, after the lunge and slam state changed for this event.
    void CueAnim(RE::PlayerCharacter* p, const std::string& tag)
    {
        if (!snd::Cues()) return;
        if (tag == "plsThrow") { snd::ChargeStart("CW_SND_PLSCharge"); return; }
        if (tag == "plsFire") { snd::ChargeStop(); snd::Cue("CW_SND_PLSFire"); return; }
        const CueArm arm = CueArmOf(EquippedWeapon(p));
        if (arm == CueArm::None) return;
        const std::uint64_t now = GetTickCount64();
        if (tag == "drawStart") {   // only a clip's own draw SOUND cues (WPNUnarmedDraw or the generic melee equip, see
            // the hook): the graph also emits a bare weaponDraw at the END of every attack string (attackStop +
            // weaponDraw, P13c 19:07:30.154), and cueing on that played an equip after every combo (QD block 22).
            if (now - cueDrawMs < 700) return;
            cueDrawMs = now;
            const bool first = now >= cueAutoDrawUntil && cueDrawnOnce.insert((int)arm).second && arm != CueArm::Monowire;   // Monowire has no FirstEquip
            snd::Cue(first ? CueName(arm, "CW_SND_MantisFirstEquip", "CW_SND_GorillaFirstEquip", "")
                           : CueName(arm, "CW_SND_MantisEquip", "CW_SND_GorillaEquip", "CW_SND_MonowireEquip"));
        } else if (tag == "weaponSheathe") {
            if (now - cueSheatheMs < 700) return;
            cueSheatheMs = now;
            snd::Cue(CueName(arm, "CW_SND_MantisUnequip", "CW_SND_GorillaUnequip", "CW_SND_MonowireUnequip"));
        } else if (tag == "weaponSwing") {
            if (lunge.active) return;                                  // the leap has its own cue, at lunge start
            if (slam.driving) { snd::Cue("CW_SND_GorillaSlamJump"); return; }
            snd::Cue(CueName(arm, "CW_SND_MantisSwing", "CW_SND_GorillaSwing", "CW_SND_MonowireSwing"));
        }
    }

    // game thread: an animation event of the player's graph
    void KeepEvent(RE::PlayerCharacter* p, const std::string& tag);   // bVisualKeep, below with the Pip-Boy hide
    void FoldEvent(RE::PlayerCharacter* p, const std::string& tag);   // bMantisFold3P, below with Frame
    // bPlsChargeGlow, game thread: a grenade-throw wind-up or its shot. Only with the PLS installed; a gun's weaponFire with no wind-up
    // before it is refused inside tint::Charge.
    void PlsChargeEvent(bool fire)
    {
        if (!pls.on) return;
        const bool ok = tint::Charge("launcher", fire);
        if (g_ini.debug && ok) REX::INFO("arms: PLS charge glow {}", fire ? "flash (shot)" : "wind-up");
    }
    void OnAnimEvent(const std::string& tag)
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p) return;
        if (tag == "initiateEnd") {   // forwarded only for the slam re-pick's attack buffer: the graph is back after the equip clip
            if (slam.ready && slam.repicking) {
                slam.repicking = false;
                if (slam.buffered) {
                    slam.buffered = false;
                    const bool ok = PerformPlayerAction("ActionRightAttack");
                    slam.replayAt = GetTickCount64(); ++slam.replays;
                    REX::INFO("arms: ground slam: attack pressed during the re-pick replayed ({}, airborne {})", ok ? "queued" : "REFUSED", Airborne(p));
                }
            }
            return;
        }
        if (fin.active) {   // a finisher's own HitFrames are not slams or lunges
            if (tag == "HitFrame") {
                // The cue follows the finisher that is PLAYING (its id), not the equipped arm: the test seam can force any
                // finisher under any weapon. The Monowire has none here: its cue starts with the clip (CueFinisherStart).
                const CueArm fa = fin.id.rfind("Mantis", 0) == 0 ? CueArm::Mantis : fin.id.rfind("Gorilla", 0) == 0 ? CueArm::Gorilla
                                : fin.id.rfind("Monowire", 0) == 0 ? CueArm::Monowire : CueArmOf(EquippedWeapon(p));
                const char* cue = fa == CueArm::None ? "" : CueName(fa, "CW_SND_MantisFinisher", "CW_SND_GorillaFinisher", "");
                REX::INFO("snd: finisher {} HitFrame cue '{}' (bCues {})", fin.id, cue, snd::Cues() ? "on" : "off, nothing plays");
                snd::Cue(cue);
            }
            return;
        }
        if (tag == "drawStart" || tag == "foldRetract" || tag == "foldSheatheSnd") FoldEvent(p, tag);   // bMantisFold3P (ARMS LOOK, INTEGRATION P14 merge)
        if (tag == "foldRetract" || tag == "foldSheatheSnd") return;
        if (tag == "weaponSwing" && leapReady) { leapAttacking = true; leapSwingAt = GetTickCount64(); }
        else if (tag == "AttackStop") leapAttacking = false;
        if (tag == "weaponSwing" && slam.ready) {
            slam.swinging = true;
            if (Airborne(p)) { slam.driving = true; slam.measured = true; slam.startZ = p->GetPosition().z; }   // GOR-3: drive down into the ground (Frame)
        }
        else if (tag == "HitFrame" && slam.swinging && !slam.pending) {
            if (Airborne(p)) slam.pending = true; else DoSlam();
        } else if (tag == "AttackStop" && slam.swinging) {   // only after a real swing: a re-pick replays attackStop itself
            slam.swinging = false;
            if (!slam.pending) SetSlamReady(false);
        }
        if (g_ini.visualKeep && (tag == "weaponDraw" || tag == "weaponSheathe" || tag == "drawStart")) {
            KeepEvent(p, tag);
            if (tag == "weaponSheathe") { EndLunge("sheathe"); SetLeapReady(false); }
        } else if (tag == "drawStart") {
        } else if (tag == "weaponDraw" || tag == "weaponSheathe") {
            auto* w = EquippedWeapon(p);
            std::string armo = VisualFor(w);
            int c = visual.OnAnimEvent(tag, !armo.empty());
            if (c) REX::INFO("arms: {} -> visual {}", tag, c > 0 ? "on" : "off");
            Apply(c, armo);
            if (tag == "weaponSheathe") { EndLunge("sheathe"); SetLeapReady(false); }
        } else if (tag == "weaponSwing" && leapReady && !lunge.active && GetTickCount64() - lastSprintMs < 400) {   // a normal swing or a VATS strike with READY on must not slide the player
            auto t = leapTarget.get();
            if (!t) return;
            auto pos = p->GetPosition();
            lunge.active = true;
            snd::Play2D("CW_SND_MantisLeap");   // SOUND: CP special-attack cue at lunge start
            lunge.t0 = GetTickCount64();
            lunge.start = { pos.x, pos.y, pos.z };
            lunge.target = leapTarget;
            auto tp = t->GetPosition();
            double dx = tp.x - pos.x, dy = tp.y - pos.y;
            REX::INFO("arms: lunge start at {:08X} from ({:.0f},{:.0f},{:.0f}) target ({:.0f},{:.0f},{:.0f}) dist {:.0f} u",
                      t->GetFormID(), pos.x, pos.y, pos.z, tp.x, tp.y, tp.z, std::sqrt(dx * dx + dy * dy));
            lunge.launch = g_ini.leapLaunch; lunge.wasAir = false;
            if (lunge.launch) {   // a real leap: load-up, off the ground in an arc to the target, the strike on landing (Frame / LaunchLand)
                lunge.aim = { tp.x, tp.y, tp.z };
                if (g_ini.leapHangMs > 0) leapParams.hangMinS = leapParams.hangMaxS = g_ini.leapHangMs / 1000.0;   // one landing time for every distance
                lunge.shape = cw::LeapProfileShape(lunge.start, lunge.aim, leapParams);
                lunge.frames = 0; lunge.peakZ = 0;
                confirm::Mark("leapLaunch");   // TEST HARNESS: the leap's own confirmation (it has no graph event of its own)
                REX::INFO("arms: leap launch apex {:.0f} u, airborne {:.2f} s (load-up {:.2f} s), travel {:.0f} u, landing planned {:.2f} s after weaponSwing ({})",
                          lunge.shape.apex, lunge.shape.totalS - leapParams.loadS, leapParams.loadS, lunge.shape.travel, lunge.shape.totalS,
                          g_ini.leapHangMs > 0 ? "iLeapHangMs: the same at every distance" : "hang scaled by the travel");
            }
        } else if (tag == "HitFrame" && lunge.active && lunge.launch) {
            // mid-air: the clip's HitFrame strikes nothing; the landing does (LaunchLand)
        } else if (tag == "HitFrame" && lunge.active) {
            if (Active("perk_spatial_mapping"))   // CP: leap attacks cripple the target (both legs: FO4 mobility condition)
                if (auto t = lunge.target.get())
                    for (const char* av : { "LeftMobilityCondition", "RightMobilityCondition" })
                        if (auto* info = FindAV(av)) static_cast<RE::ActorValueOwner*>(t.get())->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *info, -100.0f);
            if (auto* sp = RE::PlayerCharacter::GetSingleton()) snd::PlayAt("CW_SND_MantisLand", sp->GetPosition());   // SOUND: arrival impact (MAN-7's sound half)
            EndLunge("HitFrame");
        }
        CueAnim(p, tag);
    }

    using ProcessEvent_t = RE::BSEventNotifyControl (*)(RE::BSTEventSink<RE::BSAnimationGraphEvent>*, const RE::BSAnimationGraphEvent&, RE::BSTEventSource<RE::BSAnimationGraphEvent>*);
    ProcessEvent_t g_orig = nullptr;
    std::atomic<bool> animLog{ false };   // test seam {"cmd":"animlog","on":true}: log every graph event tag of the player
    RE::BSEventNotifyControl Hook(RE::BSTEventSink<RE::BSAnimationGraphEvent>* a_this, const RE::BSAnimationGraphEvent& a_event, RE::BSTEventSource<RE::BSAnimationGraphEvent>* a_src)
    {
        const char* t = a_event.tag.c_str();
        confirm::OnAnimEvent(t);   // TEST HARNESS: a pending seam action is PLAYED only on its confirming event (no-op when none is pending)
        if (animLog.load(std::memory_order_relaxed) && t) REX::INFO("anim: {} '{}'", t, a_event.payload.c_str());
        // bVisualKeep: our 1P/3P arm Equip clips start with SoundPlay.WPNUnarmedDraw at t=0 (cp2fo4_1p role equip): the earliest
        // draw cue, before weaponDraw. FO4 may deliver it as the full tag or as "SoundPlay" + payload: accept both.
        const bool isSnd = t && !_strnicmp(t, "SoundPlay", 9);
        const char* pl = a_event.payload.c_str();
        const auto has = [&](const char* s) { return std::strstr(t, s) || (pl && std::strstr(pl, s)); };
        // SOUND WIRING: while bCues is on and an arm (or the PLS) is what made the event, our cue replaces the clip's vanilla
        // SoundPlay. Forwarding stops only for that one event of the player's own sink; the rest of the chain is untouched.
        const bool cues = g_ini.sndCues;
        const bool plsThrow = isSnd && cues && g_cuePls.load(std::memory_order_relaxed) && has("WPNGrenadeThrow");
        // An arm DRAW arrives as one of TWO sound events, never both (P14 log: WPNUnarmedDraw 26x, the generic 61x,
        // always exactly one per draw ~290 ms before weaponDraw). The generic form is what a swap from another weapon
        // emits, and it is the COMMONER of the two, so missing it left most draws with the vanilla cue and none of ours
        // (QD block 24, T11a). "WPNGenericMeleeEquip" is a prefix: it covers the Up/UpFast/Down variants.
        // "MeleeSwing" catches WPNMeleeSwingA/B, the swing sound some arm clips emit instead of WPNSwingUnarmed, which
        // the swing cue (driven by the weaponSwing TAG, not this sound) would otherwise have played on top of.
        const auto armEquipSnd = [&] { return has("WPNUnarmedDraw") || has("WPNGenericMeleeEquip"); };
        const bool armVanilla = isSnd && cues && g_cueArm.load(std::memory_order_relaxed) != 0 &&
                                (has("WPNSwing") || has("MeleeSwing") || armEquipSnd() || has("WPNUnarmedSheathe"));
        if (cues && t && !_strnicmp(t, "weaponFire", 10) && (std::strchr(t, '2') || (pl && std::strchr(pl, '2'))) && g_cuePls.load(std::memory_order_relaxed)) PostTask([]() { OnAnimEvent("plsFire"); });   // weaponFire.2: the grenade release (a gun's weaponFire has no .2)
        if (plsThrow) PostTask([]() { OnAnimEvent("plsThrow"); });
        // bPlsChargeGlow (ARMS LOOK 6d4942c0): every PLS throw clip keeps the vanilla contract, SoundPlay.WPNGrenadeThrow at its start and
        // weaponFire.2 at the shot (retarget/build_pls_clips.py), so the two bracket the wind-up. INTEGRATION merge (P13): placed beside
        // SOUND WIRING's cue block; it only observes the two events, the forwarding decision below is unchanged.
        if (g_ini.plsChargeGlow && isSnd && has("WPNGrenadeThrow")) PostTask([]() { PlsChargeEvent(false); });
        else if (t && g_ini.plsChargeGlow && !_strnicmp(t, "weaponFire", 10)) PostTask([]() { PlsChargeEvent(true); });
        if (isSnd && armEquipSnd())   // either draw sound keys the cue; CueAnim's 700 ms guard makes a double impossible (SOUND; INTEGRATION P15b merge)
            PostTask([]() { OnAnimEvent("drawStart"); });
        // bMantisFold3P (ARMS LOOK 71a81c58; INTEGRATION P14 merge, beside the glow observers): the holster starts. Our UnEquip clips carry a
        // Retract annotation at 0.033 s and SoundPlay.WPNUnarmedSheathe at 0.1 s. Observe only; the cue forwarding decision below is unchanged.
        // AAA row 5: our Block clips carry a Retract too (cp2fo4_1p.py), so the two cues get their own tags and FoldEvent tells them apart.
        else if (t && g_ini.mantisFold3P && !_stricmp(t, "Retract"))
            PostTask([]() { OnAnimEvent("foldRetract"); });
        else if (t && g_ini.mantisFold3P && isSnd && has("WPNUnarmedSheathe"))
            PostTask([]() { OnAnimEvent("foldSheatheSnd"); });
        else if (t && (!std::strcmp(t, "weaponDraw") || !std::strcmp(t, "weaponSheathe") || !std::strcmp(t, "weaponSwing") || !std::strcmp(t, "HitFrame") ||
                  !_stricmp(t, "AttackStop") || !std::strcmp(t, "initiateEnd"))) {
            std::string tag = !_stricmp(t, "AttackStop") ? std::string("AttackStop") : std::string(t);
            PostTask([tag]() { OnAnimEvent(tag); });
        }
        if (plsThrow || armVanilla) {
            if (animLog.load(std::memory_order_relaxed)) REX::INFO("anim: vanilla cue kept off '{}' '{}'", t, pl ? pl : "");
            return RE::BSEventNotifyControl::kContinue;
        }
        return g_orig(a_this, a_event, a_src);
    }

    // The player's own BSTEventSink<BSAnimationGraphEvent> (TESObjectREFR base at +0x38) receives its graph events.
    // Patch slot 1 (ProcessEvent) of the vtable that subobject points at: exact by construction, no vtable ID guess.
    void InstallHook()
    {
        if (hooked) return;
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p) { REX::WARN("arms: no player yet, anim hook not installed"); return; }
        auto* sink = static_cast<RE::BSTEventSink<RE::BSAnimationGraphEvent>*>(p);
        void** vtbl = *reinterpret_cast<void***>(sink);
        std::uint32_t old = 0;
        if (!REX::W32::VirtualProtect(&vtbl[1], sizeof(void*), 0x40u /* PAGE_EXECUTE_READWRITE */, &old)) { REX::ERROR("arms: VirtualProtect failed, anim hook not installed"); return; }
        g_orig = reinterpret_cast<ProcessEvent_t>(vtbl[1]);
        vtbl[1] = reinterpret_cast<void*>(&Hook);
        REX::W32::VirtualProtect(&vtbl[1], sizeof(void*), old, &old);
        hooked = true;
        REX::INFO("arms: anim hook installed on the player's BSAnimationGraphEvent sink (+0x{:X}, vtable at {:X})",
                  (std::uintptr_t)sink - (std::uintptr_t)p, (std::uintptr_t)vtbl);
    }

    // ---- Mantis / Monowire clean dismember (the author, 2026-11-08, approach A): detour Actor::DoHitMe so that a Mantis or
    // Monowire blow FROM THE PLAYER that would kill the target severs the struck limb cleanly -- HitData kDismemberLimb
    // SET and kExplodeLimb CLEARED, i.e. a clean cut, not a Bloody Mess gib. Chance per weapon from
    // arms.combat.dismember.<weap>.severChanceOnKill. The detour runs on EVERY hit to EVERY actor, so it exits fast
    // unless the aggressor is the player with a Mantis/Monowire weapon landing a fatal blow. The engine's own dismember
    // (inside DoHitMe, after we set the flag) does the sever + gore cap, so this is the native clean-cut look.
    //
    // HOOK SHAPE (fixed 2026-11-08): a CALL-SITE hook, NOT a function-entry detour. commonlib-shared
    // @ed737c0 has no write_branch, and its write_call5/write_jmp5 read the bytes at the address as an existing
    // call/jmp and return THAT target as the "original" -- at a function prologue that is garbage (every hit would jump
    // into noise). The decrypted 1.10.163 exe (SteamStub stripped, scanned for E8/E9 rel32 + absolute pointers) has
    // exactly ONE reference to DoHitMe (id 881215, rva 0xE01630): `call DoHitMe` at rva 0xD60D61 = id 1546751 + 0x921
    // (rcx = actor, rdx = HitData). No vtable slot, no tail jmp. So hooking that call covers every DoHitMe.
    // ARMS asked for the pinned commonlib line behind the 0x90 claim. From the pinned commonlibf4 (a08c731),
    // include/RE/H/HitData.h, in member order with their offsets as written in that file:
    //     float healthDamage;            // 90
    //     float totalDamage;             // 94
    //     float physicalDamage;          // 98
    //     float targetedLimbDamage;      // 9C
    // and `static_assert(sizeof(HitData) == 0xE0)` closes the struct. So `healthDamage` is its own field, immediately
    // before the `totalDamage` the old hold wrote. These asserts put the claim in the compiler: if the pinned commonlibf4
    // ever moves the layout, the build fails here instead of the hold silently writing the wrong field again.
    static_assert(sizeof(RE::HitData) == 0xE0, "pinned commonlibf4 HitData layout changed: re-read the finisher hold");
    static_assert(offsetof(RE::HitData, healthDamage) == 0x90, "HitData::healthDamage is no longer at 0x90");
    static_assert(offsetof(RE::HitData, totalDamage) == 0x94, "HitData::totalDamage is no longer at 0x94");
    using DoHitMe_t = void (*)(RE::Actor*, const RE::HitData*);
    DoHitMe_t g_origDoHitMe = nullptr;
    std::atomic<bool> dismemberHooked{ false };
    std::atomic<std::uint32_t> g_doHitMeCalls{ 0 }, g_doHitMePlayer{ 0 };   // stability-gate evidence (bDebug=1 logs them)
    // P15 H2b: the factor DoHitMe applied to totalDamage on the last survived player hit, per target (limb multiplier + armour)
    std::mutex g_hitRatioLock;
    std::unordered_map<std::uint32_t, float> g_hitRatio;
    // kUnknownHitRatio (5.0, "above the 4.3x worst case P15 measured") is GONE, ARMS (d) 2026-11-10. p15b-1 disproved the
    // design it served: no divisor can hold a hit whose applied figure, for one written value on one target, ranged from
    // 0.0 to 3222.1. That the guess happened to equal the limb-1 head multiplier is a coincidence that made it look sane.
    void DoHitMeDetour(RE::Actor* a_this, const RE::HitData* a_data)
    {
        if (const auto n = ++g_doHitMeCalls; g_ini.debug && (n == 1 || n % 25 == 0))
            REX::INFO("arms: DoHitMe hook calls {} (player aggressor {})", n, g_doHitMePlayer.load());
        // The player hit pipeline (bHitFromDoHitMe): read the hit BEFORE the engine applies it (the original may kill and
        // clear state), hand it on AFTER, so the give-backs see the damage applied as they assume. Hits on actors already
        // dead are skipped, as the sink never saw them either.
        if (a_data && a_data->aggressor.get().get() == RE::PlayerCharacter::GetSingleton()) ++g_doHitMePlayer;
        // AB-E1: one line per hit the player lands, with a millisecond time, so QD can match a capture gap to a hit. The
        // perk entries are evaluated by the engine while it builds the HitData, i.e. just BEFORE this call.
        if (g_abE1HitLog.load() && a_this && a_data && a_data->aggressor.get().get() == RE::PlayerCharacter::GetSingleton() && a_this != RE::PlayerCharacter::GetSingleton())
            REX::INFO("abe1: hit t={} arm={} target {:08X} damage {:.1f}", GetTickCount64(), g_abE1Off.load() ? "OFF" : "ON", a_this->GetFormID(), a_data->totalDamage);
        enum class Route { None, Strike, Hurt } route = Route::None;
        std::uint32_t tgt = 0, srcId = 0, attacker = 0;
        RE::TESObjectWEAP* hw = nullptr;
        bool hPower = false, hSneak = false, hStag = false;
        float hDealt = 0.0f;
        int hLimb = -1;
        if (g_hitViaDoHitMe.load() && g_coreOk && a_this && a_data && a_this->lifeState == 0) {
            auto* p = RE::PlayerCharacter::GetSingleton();
            auto agg = a_data->aggressor.get();
            if (p && (agg.get() == p || a_this == p) && !(agg.get() == p && a_this == p)) {
                hw = a_data->weapon.object ? a_data->weapon.object->As<RE::TESObjectWEAP>() : nullptr;
                srcId = a_data->weapon.object ? a_data->weapon.object->GetFormID() : 0;
                hDealt = a_data->totalDamage;
                if (a_this == p) {
                    route = Route::Hurt;
                    attacker = agg.get() ? agg->GetFormID() : 0;
                } else {
                    route = Route::Strike;
                    tgt = a_this->GetFormID();
                    hPower = a_data->flags.any(RE::HitData::Flag::kPowerAttack);
                    hSneak = a_data->flags.any(RE::HitData::Flag::kSneakAttack);
                    hStag = a_data->stagger.underlying() != 0;
                    hLimb = (int)a_data->damageLimb.underlying();
                }
            }
        }
        std::string finId;
        double finHp = 0.0;
        // bFinisherFloor: lifted for this call only, dropped below; the destructor is the backstop.
        // ARMS review 2026-11-10, blocking 1: the measurement and the restore both happen UNDER the lift, and the lift is
        // dropped last, so the victim's Health never passes through zero on a live actor.
        std::optional<FinLift> lift;
        float floorHp = 0;        // the health the victim is to end the call on
        float hpLifted0 = 0;      // its health immediately after the lift went on (floorHp + kFinLift)
        bool choreographyHit = false;   // blocking 2: a lead clip's own HitFrame on the victim of a running finisher
        if (route == Route::Strike && hPower && g_ini.finishers && hw && !heldKill.target) {
            std::vector<std::string> kws;
            hw->ForEachKeyword([&](RE::BGSKeyword* k) {
                if (k && k->formEditorID.c_str()) kws.emplace_back(k->formEditorID.c_str());
                return RE::BSContainer::ForEachResult::kContinue;
            });
            if (FinisherBeforeHit(a_this, kws, hStag, hSneak, hDealt, finId, finHp)) {
                // hold the hit so the victim lives into the finisher (its killAt kills); a non-fatal hit keeps its damage
                auto* hpav = FindAV("Health");
                const float cur = hpav ? static_cast<RE::ActorValueOwner*>(a_this)->GetActorValue(*hpav) : 0.0f;
                // P15 H2b read this as "DoHitMe applies a per-target factor to totalDamage (0.8x to 4.3x: limb + armour)",
                // and the hold divided by that factor. P15b retired that reading: the factor is the armour curve (<= 1x on
                // limbs 0, 6 and 8) times a flat 5x on limb 1, and in any case the hit cannot be held through a field the
                // engine does not read for Health. docs/FINISHER-ROWS.md part 1 has the twelve measured rows.
                if (g_ini.finisherFloor) {
                    // The hold that writes no damage field. p15b-1 killed this design's predecessor: the SAME written
                    // totalDamage of 39.8, on one 200 hp raider at limb 1 with one weapon, had the engine apply 0.0 once
                    // and 3222.1 once, so no divisor can turn a hit into a survivable figure and the field we can write is
                    // not the field the engine reads. Lift the victim clear of whatever the engine decides to apply, for
                    // the length of this call only, and let the finisher's own killAt do the killing.
                    floorHp = cur;
                    lift.emplace(a_this, hpav);
                    if (!lift->on) {   // the raise did not take: do not start a finisher we cannot hold
                        lift.reset();
                        floorHp = 0;
                        REX::WARN("arms: finisher {} refused on {:08X}: the health lift did not take (hp {:.0f}), the hit goes through unheld", finId, tgt, cur);
                        finId.clear();
                    } else {
                        hpLifted0 = static_cast<RE::ActorValueOwner*>(a_this)->GetActorValue(*hpav);
                        heldKill = { tgt, hDealt, GetTickCount64() + kHeldKillMs };   // the net applies the ORIGINAL blow, never a held figure
                        REX::INFO("arms: finisher {} decided before the hit on {:08X}: hp {:.0f}, hit {:.1f} unchanged, held by a {:.0f} health lift for this call only (leaves {:.0f}%)",
                                  finId, tgt, cur, hDealt, kFinLift, finHp * 100);
                    }
                } else {
                    // bFinisherFloor=0 (the shipped value): the hold is not attempted at all. The old branch wrote
                    // HitData::totalDamage and is deleted rather than kept behind a key, because p15b-1 showed it does not
                    // hold and leaves the engine's own damage arithmetic inconsistent. Without it the finisher falls back
                    // to TryFinisher AFTER the hit, which is where it was before the before-the-hit path existed. This
                    // costs players nothing today: the whole route needs bHitFromDoHitMe, which Cyberware.ini ships 0.
                    if (g_ini.debug)
                        REX::INFO("arms: finisher {} was eligible before the hit on {:08X} (hp {:.0f}, hit {:.1f}) but bFinisherFloor=0, so the hit is not held", finId, tgt, cur, hDealt);
                    finId.clear();
                }
            }
        }
        // ARMS review 2026-11-10, blocking 2: the lead clip's own HitFrames are REAL weapon hits. The standing buffer used
        // to cover them ("without a buffer the victim died ~0.5 s in and ragdolled out of the choreography", in game
        // 2026-11-08), and p15b-1 shows what they weigh: ten of them at 107.6 each on one victim. The deciding blow's lift
        // is gone by then and FinFloorRestore only heals a victim that is still ALIVE, so a lethal HitFrame mid-clip would
        // kill the victim early -- the same failure by another door. So every hit on the victim of a running finisher is
        // lifted the same way, with the same restore-then-drop order.
        if (!lift && route == Route::Strike && g_ini.finisherFloor && fin.active && fin.floor && !fin.killed &&
            tgt == fin.victim && fin.floorHp > 0 && a_this->lifeState == 0) {
            if (auto* hpav = FindAV("Health")) {
                floorHp = fin.floorHp;   // back to the set piece's own health, not to whatever it has now
                lift.emplace(a_this, hpav);
                if (!lift->on) {
                    lift.reset();
                    floorHp = 0;
                    REX::WARN("arms: finisher {}: the health lift did not take on a choreography hit on {:08X}; it may die mid-clip", fin.id, tgt);
                } else {
                    choreographyHit = true;
                    hpLifted0 = static_cast<RE::ActorValueOwner*>(a_this)->GetActorValue(*hpav);
                    if (g_ini.debug) REX::INFO("arms: finisher {}: choreography hit on {:08X} lifted (hit {:.1f})", fin.id, tgt, hDealt);
                }
            }
        }
        if (g_ini.dismember && g_coreOk && a_this && a_data && a_this->lifeState == 0) {
            auto* p = RE::PlayerCharacter::GetSingleton();
            if (p && a_data->aggressor.get().get() == p) {
                auto* w = a_data->weapon.object ? a_data->weapon.object->As<RE::TESObjectWEAP>() : nullptr;
                const json dj = CombatCfg().value("dismember", json::object());
                double chance = 0.0;
                for (const char* key : { "mantis", "monowire" })
                    if (dj.contains(key) && WeapHas(w, Kw(S(dj[key], "weaponKeyword")))) { chance = dj[key].value("severChanceOnKill", 0.0); break; }
                if (chance > 0.0) {
                    auto* hp = FindAV("Health");
                    const float cur = hp ? static_cast<RE::ActorValueOwner*>(a_this)->GetActorValue(*hp) : 1e9f;
                    if (a_data->totalDamage >= cur) {   // only a FATAL blow severs (dismember on kill, not every hit)
                        const std::uint64_t x = (GetTickCount64() ^ reinterpret_cast<std::uintptr_t>(a_this)) * 2654435761ull;
                        const double r = (double)((x >> 11) & 0xFFFFF) / (double)0x100000;
                        if (r < chance) {
                            auto* d = const_cast<RE::HitData*>(a_data);
                            d->flags.set(RE::HitData::Flag::kDismemberLimb);
                            d->flags.reset(RE::HitData::Flag::kExplodeLimb);
                            REX::INFO("arms: dismember r {:.2f}<{:.2f} -> clean sever limb {}", r, chance, (int)a_data->damageLimb.underlying());
                        }
                    }
                }
            }
        }
        // P14 H2 (INTEGRATION 3): three Mantis hits logged dealt 104.8 each (314.4) while the raider lost 257. totalDamage is
        // read BEFORE the original runs, and two things can change what lands after that: the hook we chain over
        // (EnergyWeaponCalcFix.dll on the test profile) and DoHitMe's own health application. So a strike's damage is
        // measured: the target's Health before and after the original call. The applied amount is what PlayerStrike gets
        // (it feeds give-backs and the hit log); the HitData figure stays in the log beside it. The player-hurt route keeps
        // the HitData figure (tgm test runs would read 0).
        auto* hpAV = route == Route::Strike ? FindAV("Health") : nullptr;
        auto* avo = static_cast<RE::ActorValueOwner*>(a_this);
        // ARMS 2026-11-10: the lift must never be counted as damage. For a lifted hit BOTH reads are taken under the lift
        // and their difference is the engine's own figure, with the lift cancelling exactly; `hpBefore` is reported as the
        // victim's real pre-lift health. For an ordinary hit this is just the health before the call.
        const float hpBefore = lift && floorHp > 0 ? floorHp : (hpAV ? avo->GetActorValue(*hpAV) : 0.0f);
        // Row E1 (the lead, 2026-11-10 condition 2): the engine's own damage fields, so the limb-1 5x can be attributed to one
        // of them instead of guessed at. `applied` is measured from Health, so it cannot say WHICH field the engine read;
        // these two lines can. Not ours to rule on: ARMS and BETA QA read them.
        const bool fieldLog = g_ini.hitFieldLog && g_ini.debug && route == Route::Strike && a_data;
        if (fieldLog)
            REX::INFO("hit: fields before {:08X} limb {} health {:.2f} total {:.2f} physical {:.2f} limbDmg {:.2f} blocked {:.2f} resistPhys {:.2f} resistTyped {:.2f} sneakBonus {:.2f} bonusMult {:.3f} critMult {:.3f} push {:.2f} reflect {:.2f} flags 0x{:X} material {}",
                      tgt, hLimb, a_data->healthDamage, a_data->totalDamage, a_data->physicalDamage, a_data->targetedLimbDamage, a_data->percentBlocked,
                      a_data->resistedPhysicalDamage, a_data->resistedTypedDamage, a_data->sneakAttackBonus, a_data->bonusHealthDamageMult,
                      a_data->criticalDamageMult, a_data->pushBack, a_data->reflectedDamage, a_data->flags.underlying(), a_data->material);
        g_origDoHitMe(a_this, a_data);
        if (route == Route::Strike) {
            float applied = hDealt;
            const bool liftWasOn = lift.has_value();
            // ARMS review 2026-11-10, blocking 1. The old order dropped the lift and only then healed, so between those
            // two statements a live victim sat on cur - D, at or below zero, on the very blow the hold exists for; whether
            // FO4 kills on that is unverified, and `survivedLift` had been sampled before the drop, so if it does the code
            // would log "hold held", heal a dying actor and start a finisher on a corpse -- "gone or dead by the task"
            // again. Order now: measure under the lift, restore under the lift, drop last. Health goes
            // cur + lift - D  ->  cur + lift  ->  cur, and never touches zero.
            float engineTook = 0;   // D: what the engine removed inside the call, measured under the lift
            bool aliveAfter = true;
            if (liftWasOn && hpAV) {
                engineTook = hpLifted0 - avo->GetActorValue(*hpAV);
                aliveAfter = a_this->lifeState == 0;
                if (aliveAfter && engineTook > 0.5f) avo->RestoreActorValue(*hpAV, engineTook);   // restore FIRST
                lift->Drop();                                                                     // then drop
                lift.reset();
            }
            if (hpAV) {
                // For a lifted hit the reading is computed, not sampled: the health the blow would really have removed,
                // capped by what the victim had. For an ordinary hit it is the measured drop, with hpAfter clamped at 0 so
                // a kill no longer credits its overkill -- the 3222.1 of QD row FN2 on a 200 hp target (ARMS (b)).
                const float hpAfter = liftWasOn ? hpBefore - std::min(engineTook, hpBefore) : avo->GetActorValue(*hpAV);
                applied = liftWasOn ? std::min(engineTook, hpBefore) : std::max(0.0f, hpBefore - std::max(hpAfter, 0.0f));
                // A lead clip's own HitFrame is choreography, not a blow the player aimed: p15b-1 logged ten of them at
                // 107.6 each on one victim, so crediting them would hand the give-backs about 1076 of free life steal per
                // finisher. They report 0. The deciding blow still reports its real figure.
                if (choreographyHit) applied = 0.0f;
                // A lifted hit teaches nothing: the lift decides whether the victim survived, so the "survived hit" test
                // cannot tell a real survival from one the lift bought, and ARMS's factor table would fill up with head
                // multipliers. kUnknownHitRatio is gone with the old hold, so nothing reads this map for a decision now.
                if (!liftWasOn && a_data->totalDamage > 1.0f && applied > 0.0f && hpAfter > 0.0f) {   // a kill caps the drop: only a survived hit teaches the factor; a lifted hit teaches nothing
                    std::lock_guard<std::mutex> lk(g_hitRatioLock);
                    if (g_hitRatio.size() > 256) g_hitRatio.clear();
                    g_hitRatio[tgt] = applied / a_data->totalDamage;
                }
                if (fieldLog)
                    REX::INFO("hit: fields after  {:08X} limb {} health {:.2f} total {:.2f} physical {:.2f} limbDmg {:.2f} blocked {:.2f} resistPhys {:.2f} resistTyped {:.2f} sneakBonus {:.2f} bonusMult {:.3f} critMult {:.3f} flags 0x{:X}; hp {:.1f} -> {:.1f} = {:.1f} applied, predicted {:.1f}",
                              tgt, hLimb, a_data->healthDamage, a_data->totalDamage, a_data->physicalDamage, a_data->targetedLimbDamage, a_data->percentBlocked,
                              a_data->resistedPhysicalDamage, a_data->resistedTypedDamage, a_data->sneakAttackBonus, a_data->bonusHealthDamageMult,
                              a_data->criticalDamageMult, a_data->flags.underlying(), hpBefore, hpAfter, applied, cw::PredictApplied(hDealt, hLimb));
                // The shape of this line is kept: other rows grep it. Anything new is appended at the end (ARMS (e)).
                // The shape of this line is kept byte for byte up to the closing bracket, so a grep on
                // `chain <number>)` still matches every line including the lifted ones (ARMS nit 3). Extra words follow it.
                if (g_ini.debug || std::fabs(applied - hDealt) > 0.5f)
                    REX::INFO("hit: applied {:.1f} of HitData {:.1f} on {:08X} (hp {:.1f} -> {:.1f}; totalDamage after the chain {:.1f}){}",
                              applied, hDealt, tgt, hpBefore, hpAfter, a_data->totalDamage,
                              liftWasOn ? (choreographyHit ? " held by a finisher health lift (choreography hit, reported as 0)" : " held by a finisher health lift (hp computed, not sampled)") : "");
                if (liftWasOn && aliveAfter && floorHp > 0) {
                    // `net` is what QD's FIN-4 row reads: the health the victim actually ends the held hit with against the
                    // health it entered with. 0.0 means the hold held. `applied` keeps the engine's figure, which ARMS wants
                    // for the give-backs and the factor work, and the two are deliberately different.
                    const float after = avo->GetActorValue(*hpAV);
                    REX::INFO("arms: finisher hold held {:08X}{}: engine took {:.1f}, hp {:.1f} back to {:.1f}, net {:.1f}, lifts outstanding {}",
                              tgt, choreographyHit ? " (choreography hit)" : "", engineTook, hpBefore - std::min(engineTook, hpBefore), after, floorHp - after, g_finLifts.load());
                } else if (liftWasOn && !aliveAfter) {
                    REX::WARN("arms: finisher hold FAILED on {:08X}: it died under a {:.0f} health lift, so the engine applied more than that", tgt, kFinLift);
                }
            }
            PlayerStrike(hw, srcId, tgt, hPower, hSneak, hStag, applied, hLimb, "DoHitMe", finId, finHp);
        }
        else if (route == Route::Hurt) PlayerHurt(attacker, hDealt, hw);
    }
    void InstallDismemberHook()
    {
        // kill switch: no detour at all unless one of its users is on (clean dismember, or the hit pipeline)
        if (!g_ini.dismember && !g_ini.hitFromDoHitMe) { REX::INFO("arms: [Arms] bDismember=0 bHitFromDoHitMe=0, DoHitMe detour NOT installed (arm hit effects hang off TESHitEvent)"); return; }
        if (!g_queryRuntime) { REX::WARN("arms: DoHitMe hook NOT installed (call-site offset is for OG 1.10.163 only)"); return; }
        if (dismemberHooked.exchange(true)) return;
        REL::Relocation<std::uintptr_t> site{ REL::ID(1546751), 0x921 };   // the one `call Actor::DoHitMe` in 1.10.163
        REL::Relocation<std::uintptr_t> doHitMe{ RE::ID::Actor::DoHitMe };
        const auto at = site.address();
        if (*reinterpret_cast<const std::uint8_t*>(at) != 0xE8) {
            REX::ERROR("arms: DoHitMe hook NOT installed: byte at call site {:X} is {:02X}, not E8 (exe differs?)", at, *reinterpret_cast<const std::uint8_t*>(at));
            return;
        }
        const auto cur = at + 5 + static_cast<std::intptr_t>(*reinterpret_cast<const std::int32_t*>(at + 1));
        g_origDoHitMe = reinterpret_cast<DoHitMe_t>(REL::GetTrampoline().write_call<5>(at, &DoHitMeDetour));
        if (cur == doHitMe.address())
            REX::INFO("arms: DoHitMe detour installed (call-site hook at {:X}; clean dismember for Mantis/Monowire kills)", at);
        else {
            const auto real = cwsys::FollowJumps(cur);
            const std::string who = cwsys::ModuleOf(real);
            REX::INFO("arms: DoHitMe detour installed, CHAINED over {}'s hook at {:X} (it called {:X} -> {:X}, vanilla is {:X})",
                      who.empty() ? std::string("an unknown module") : who, at, cur, real, doHitMe.address());
        }
        g_hitViaDoHitMe.store(g_ini.hitFromDoHitMe);
        REX::INFO("arms: DoHitMe users: clean dismember {}, player hit pipeline {} ({})", g_ini.dismember ? "on" : "off",
                  g_ini.hitFromDoHitMe ? "on" : "off", g_ini.hitFromDoHitMe ? "TESHitEvent stands down" : "still on TESHitEvent");
    }

    bool IsHostile(RE::Actor* a, RE::Actor* b)
    {
        using func_t = bool (*)(RE::Actor*, RE::Actor*);
        static REL::Relocation<func_t> f{ RE::ID::Actor::GetHostileToActor };
        return f(a, b);
    }

    // bLeapLaunch flight: the character controller carries the player on the planned arc (horizontal speed held every frame,
    // the vertical launch speed for the first 120 ms, then gravity); touchdown (or the flight time + 0.6 s) is the strike.
    // UNVERIFIED IN GAME: whether the controller keeps a velocity set from the ground (the legs implants set it mid-air);
    // "never left the ground" in the log says it did not.
    // A Papyrus native through the script VM (no address needed): Game.ShakeCamera / Game.ShakeController for the leap's
    // landing (QUALITY-BAR MAN-7: the landing must read as the player hitting the ground).
    void LandingShake(RE::PlayerCharacter* p, float strength, float secs)
    {
        (void)p;   // akSource None = the player (the commonlibf4 template that packed the player crashes the OG runtime: cw_voice.cpp)
        bool a = false, b = false;
        voice::LandingShake(strength, secs, a, b);
        REX::INFO("arms: leap landing shake {:.2f} for {:.2f} s (camera {}, controller {})", strength, secs, a ? "queued" : "REFUSED", b ? "queued" : "REFUSED");
    }
    void LaunchLand(RE::PlayerCharacter* p, const char* why)
    {
        auto pp = p->GetPosition();
        int hit = 0;
        REX::INFO("arms: leap peak rise {:.0f} u (planned {:.0f})", lunge.peakZ, lunge.shape.apex);
        // the area hit around the landing (MAN-7): every other hostile within 150 u is staggered and takes half damage
        if (auto* pl = RE::ProcessLists::GetSingleton()) {
            const cw::NativeAbility* ma0 = Find("ar_mantis");
            const double half = (ma0 ? Pn(*ma0, "base_dmg", 30) : 30) * 1.25;
            auto* hp0 = FindAV("Health");
            auto tgt = lunge.target.get();
            for (auto& hd : pl->highActorHandles) {
                auto a = hd.get();
                if (!a || a.get() == p || a.get() == tgt.get() || a->lifeState != 0 || !IsHostile(a.get(), p)) continue;
                auto ap = a->GetPosition();
                if ((ap.x - pp.x) * (ap.x - pp.x) + (ap.y - pp.y) * (ap.y - pp.y) > 150.0 * 150.0) continue;
                if (hp0) static_cast<RE::ActorValueOwner*>(a.get())->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *hp0, (float)-half);
                if (g_ini.knockback && a->currentProcess) a->currentProcess->KnockExplosion(a.get(), pp, 0.4f);
                ++hit;
            }
        }
        if (auto t = lunge.target.get(); t && t->lifeState == 0) {
            auto tp = t->GetPosition();
            const double dx = tp.x - pp.x, dy = tp.y - pp.y;
            if (dx * dx + dy * dy <= 170.0 * 170.0) {   // the target is within the blades' reach of the landing
                const cw::NativeAbility* ma = Find("ar_mantis");
                const double dmg = (ma ? Pn(*ma, "base_dmg", 30) : 30) * 2.5;   // CP's leap hits hard: 2.5x the arm's base damage (ESTIMATE)
                if (auto* hp = FindAV("Health")) static_cast<RE::ActorValueOwner*>(t.get())->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *hp, (float)-dmg);
                if (g_ini.knockback && t->currentProcess) t->currentProcess->KnockExplosion(t.get(), pp, 1.0f);   // knocked down by the impact
                if (Active("perk_spatial_mapping"))   // CP: leap attacks cripple the target (both legs: FO4 mobility condition)
                    for (const char* av : { "LeftMobilityCondition", "RightMobilityCondition" })
                        if (auto* info = FindAV(av)) static_cast<RE::ActorValueOwner*>(t.get())->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *info, -100.0f);
                ++hit;
                REX::INFO("arms: leap strike {:08X} {:.0f} dmg ({} hostile(s) hit in all)", t->GetFormID(), dmg, hit);
                confirm::Mark("leapStrike");
            }
        }
        // impact: the slam's shockwave explosion at the landing point (camera kick + sound + dust; damage/force 0 in its record)
        const std::string ex = S(g_map.arms.value("slam", json::object()), "explosion");
        if (auto* ef = ex.empty() ? nullptr : g_map.Get(ex); ef && ef->As<RE::BGSExplosion>()) {
            RE::NEW_REFR_DATA d;
            d.location = pp;
            d.direction = RE::NiPoint3{ 0.0f, 0.0f, 0.0f };
            d.object = ef->As<RE::BGSExplosion>();
            d.interior = p->GetParentCell();
            d.world = d.interior ? d.interior->worldSpace : nullptr;
            auto h = RE::TESDataHandler::GetSingleton()->CreateReferenceAtLocation(d);
            REX::INFO("arms: leap impact {} {}", ex, h.get() ? "placed" : "NOT placed");
        }
        LandingShake(p, std::clamp(0.35f + (float)lunge.peakZ / 400.0f, 0.35f, 0.8f), 0.35f);   // a higher drop shakes harder
        snd::PlayAt("CW_SND_MantisLand", pp);   // SOUND: one cue per touchdown (MAN-7's sound half)
        if (auto* mh = p->currentProcess ? p->currentProcess->middleHigh : nullptr)   // no skid after the landing
            if (auto* cc = mh->charController.get()) { RE::hkVector4f v; cc->GetLinearVelocityImpl(v); v.x = 0; v.y = 0; cc->SetLinearVelocityImpl(v); }
        EndLunge(hit ? why : (std::string(why) + ", target out of reach").c_str());
    }
    void LaunchFrame(RE::PlayerCharacter* p, double secs)
    {
        auto* mh = p->currentProcess ? p->currentProcess->middleHigh : nullptr;
        RE::bhkCharacterController* cc = mh ? mh->charController.get() : nullptr;
        if (!cc) { EndLunge("no character controller"); return; }
        if (auto t = lunge.target.get(); t && t->lifeState == 0 && secs < leapParams.loadS + leapParams.riseS) {   // home on a moving target until the top
            auto tp = t->GetPosition();
            lunge.aim = { tp.x, tp.y, tp.z };
            lunge.shape = cw::LeapProfileShape(lunge.start, lunge.aim, leapParams);
        }
        // Track the authored profile through the controller (it collides, unlike SetPosition): velocity = where the profile is
        // 50 ms from now minus where the player is. The contact speed is capped (-6 m/s) so the landing is heavy but not a fall.
        constexpr double kUnitsPerHavok = 70.0, kLook = 0.05;
        const cw::Vec3 want = cw::LeapProfilePos(lunge.start, lunge.aim, secs + kLook, leapParams);
        const auto pp = p->GetPosition();
        double vx = (want.x - pp.x) / kLook, vy = (want.y - pp.y) / kLook, vz = (want.z - pp.z) / kLook;
        const double sp = std::sqrt(vx * vx + vy * vy + vz * vz), cap = 2200.0;
        if (sp > cap) { vx *= cap / sp; vy *= cap / sp; vz *= cap / sp; }
        if (secs > lunge.shape.totalS - 0.06) vz = std::max(vz, -420.0);
        RE::hkVector4f v;
        cc->GetLinearVelocityImpl(v);
        v.x = (float)(vx / kUnitsPerHavok); v.y = (float)(vy / kUnitsPerHavok); v.z = (float)(vz / kUnitsPerHavok);
        cc->SetLinearVelocityImpl(v);
        const double rise = pp.z - lunge.start.z;
        lunge.peakZ = std::max(lunge.peakZ, rise);
        if (g_ini.debug && (lunge.frames++ % 2 == 0))   // the per-frame height the Quality Director asked for (every 2nd frame keeps the log readable)
            REX::INFO("arms: leap t {:.3f} rise {:.0f} u (want {:.0f}) travelled {:.0f} u", secs, rise, want.z - lunge.start.z,
                      std::sqrt((pp.x - lunge.start.x) * (pp.x - lunge.start.x) + (pp.y - lunge.start.y) * (pp.y - lunge.start.y)));
        const bool air = Airborne(p);
        if (air) lunge.wasAir = true;
        if (secs >= lunge.shape.totalS && (!air || secs > lunge.shape.totalS + 0.4))
            LaunchLand(p, lunge.peakZ < 20 ? "never left the ground" : "landed");
    }

    // every frame: the lunge
    // bMantisFold3P (QD block 20; P12 node read 0.31 s into a draw: 3P AnimObjectR1 = RArm_Hand, while CyberMantis\Equip.hkx holds it near
    // 70 deg there). The 3P graph never applies the AnimObject tracks of our clips, so the blades popped to full length. The plugin writes
    // the same keys itself (mantis_fold3p.inl, baked from those clips) into the 3P skeleton's AnimObjectR1/L1: the unfold from the first
    // draw cue, the fold from the holster's Retract annotation (or its sheathe sound), then the bones go back to identity (open, where
    // Idle holds them, and where vanilla AnimObject idles expect them). 3P only; 1P is left to its own clips.
    // Rotation: the standard matrix of the clip's quaternion (the 3P skeleton NIF's local rotations equal that matrix of skeleton.hkx's
    // reference quaternions, checked offline 2026-11-09). If a frame finds the bone changed from what was written, something else drives
    // it, and that is logged once per run.
    // bMantisStow3P (AAA row 5; P15 capture p15-qd-mantis-3p-fixed-tfc-front + p15-Cyberware.log, read 2026-11-10). A rigid 51.5 u blade has
    // nowhere to fold to: folded, it still sticks 20-31 u out behind the elbow. So the stowed state shrinks it into the forearm housing:
    // along its axis to kStowLen and, late, across it to kStowWid, about the housing centre (mantis_stow3p.inl, measured by
    // arms/tools/fold3p_stow.py, the numpy mirror of this code: keep the two in step). Our writes only hold while the graph is not
    // writing the bone (Equip/UnEquip clips yes, Idle/Block no; order per frame: our task, the graph, render), so the draw shows the
    // shapes only once two frames found our last write intact, and the holster keeps the shapes until the blade is stowed instead of
    // culling 220 ms into the fold. 3P only (decided at the cue); 1P is unchanged.
    bool StowApplies(RE::PlayerCharacter* p);                          // below with the keep
    bool KeepShown();
    void KeepShowFromFold(RE::PlayerCharacter* p, const char* why);
    void KeepHideFromFold(RE::PlayerCharacter* p, const char* why);
    struct Fold3P {
        int mode = 0; std::uint64_t t0 = 0; RE::NiTransform wroteR{}, wroteL{}; bool wrote = false; bool overwrittenLogged = false, yielded = false; int frames = 0;
        bool stow = false;             // bMantisStow3P applies to this fold (decided at its cue)
        float s = 0, sFrom = 0;        // stow amount now (0 out, 1 stowed), and where this fold started from (carried across a mode switch)
        bool pendingShow = false;      // draw: the shapes wait until our writes hold
        std::uint64_t shownAt = 0;     // draw: the stow-out clock starts when the shapes are shown
        int stuck = 0, lost = 0;       // consecutive frames that found our last write intact / overwritten
        bool held = false;             // our writes held at least one frame in this fold
        bool changed = false;          // FoldWrite, this frame: a bone changed since our last write
        int stowedFrames = -1;         // holster: frames since the stowed cull (-1 = not stowed yet)
        bool neverHeldLogged = false;
    } fold;
    struct M3 { float m[9]; };
    M3 QuatM(float x, float y, float z, float w)
    {
        return { { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w),
                   2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                   2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) } };
    }
    RE::NiMatrix3 NiM(const M3& a)   // row-major, 4th column 0 (as QuatMatrix always built it)
    {
        return RE::NiMatrix3(a.m[0], a.m[1], a.m[2], 0, a.m[3], a.m[4], a.m[5], 0, a.m[6], a.m[7], a.m[8], 0);
    }
    RE::NiMatrix3 QuatMatrix(float x, float y, float z, float w) { return NiM(QuatM(x, y, z, w)); }
    float Smooth01(float x)
    {
        x = std::clamp(x, 0.0f, 1.0f);
        return x * x * (3 - 2 * x);
    }
    // the key at `frame`; with a stow constant and s > 0 the blade is drawn in by s: S = w*I + (k-w)*a*a^T, rotate = R*S,
    // translate = t + R*(I-S)*c (fold3p_stow.py stow_local). The matrix is non-orthonormal on purpose: NiTransform's scale is uniform only.
    RE::NiTransform FoldKey(const fold3p::Key* k, int n, float frame, float s = 0.0f, const fold3p::Stow* st = nullptr)
    {
        if (frame <= 0) frame = 0;
        const int i = (std::min)((int)frame, n - 1), j = (std::min)(i + 1, n - 1);
        const float a = (std::min)(1.0f, frame - (float)i);
        const auto& A = k[i];
        const auto& B = k[j];
        const float sg = (A.qx * B.qx + A.qy * B.qy + A.qz * B.qz + A.qw * B.qw) < 0 ? -1.0f : 1.0f;   // shortest way round
        float x = A.qx + (sg * B.qx - A.qx) * a, y = A.qy + (sg * B.qy - A.qy) * a, z = A.qz + (sg * B.qz - A.qz) * a, w = A.qw + (sg * B.qw - A.qw) * a;
        const float len = std::sqrt(x * x + y * y + z * z + w * w);
        if (len > 1e-6f) { x /= len; y /= len; z /= len; w /= len; }
        M3 R = QuatM(x, y, z, w);
        float T[3] = { A.tx + (B.tx - A.tx) * a, A.ty + (B.ty - A.ty) * a, A.tz + (B.tz - A.tz) * a };
        if (st && s > 0.0f) {
            const float kl = 1 - s * (1 - fold3p::kStowLen);
            const float kw = 1 - Smooth01((s - 0.6f) / 0.4f) * (1 - fold3p::kStowWid);
            const float ax[3] = { st->ax, st->ay, st->az }, c[3] = { st->cx, st->cy, st->cz };
            float S[9], d[3];
            for (int r = 0; r < 3; ++r)
                for (int q = 0; q < 3; ++q) S[r * 3 + q] = (r == q ? kw : 0.0f) + (kl - kw) * ax[r] * ax[q];
            for (int r = 0; r < 3; ++r) d[r] = c[r] - (S[r * 3] * c[0] + S[r * 3 + 1] * c[1] + S[r * 3 + 2] * c[2]);   // (I - S) c
            M3 RS{};
            for (int r = 0; r < 3; ++r) {
                for (int q = 0; q < 3; ++q) RS.m[r * 3 + q] = R.m[r * 3] * S[q] + R.m[r * 3 + 1] * S[3 + q] + R.m[r * 3 + 2] * S[6 + q];
                T[r] += R.m[r * 3] * d[0] + R.m[r * 3 + 1] * d[1] + R.m[r * 3 + 2] * d[2];
            }
            R = RS;
        }
        RE::NiTransform t;
        t.rotate = NiM(R);
        t.translate = RE::NiPoint3(T[0], T[1], T[2]);
        t.scale = 1.0f;
        return t;
    }
    bool FoldWrite(RE::PlayerCharacter* p, const char* name, const RE::NiTransform& t, RE::NiTransform& wrote)
    {
        RE::NiAVObject* root = p ? p->Get3D(false) : nullptr;
        RE::NiAVObject* o = root ? root->GetObjectByName(RE::BSFixedString(name)) : nullptr;
        if (!o) return false;
        if (fold.wrote && (o->local.rotate != wrote.rotate || o->local.translate != wrote.translate)) {
            fold.changed = true;
            // P14 F4 (20:39:40, a gun mid-draw / the Pip-Boy): another driver took the bone during our fold. Vanilla uses the
            // AnimObject bones too (stimpak, Pip-Boy, paired idles), so once the Mantis is no longer the equipped weapon, or
            // the Pip-Boy is open, the fold yields instead of overriding them for the rest of its ~2.6 s.
            auto* ui = RE::UI::GetSingleton();
            const bool pip = ui && ui->GetMenuOpen(RE::BSFixedString("PipboyMenu"));
            if (pip || !WeapHas(EquippedWeapon(p), Kw("CW_AnimsCyberMantis"))) {
                REX::INFO("arms: fold3p: {} taken by another driver ({}) -> fold yields after {} frame(s)", name, pip ? "Pip-Boy open" : "Mantis no longer equipped", fold.frames);
                fold.mode = 0; fold.wrote = false; fold.yielded = true;
                return false;
            }
            if (!fold.overwrittenLogged) {
                fold.overwrittenLogged = true;
                REX::WARN("arms: fold3p: {} changed since our last write (something else drives it in 3P)", name);
            }
        }
        o->local = t;
        if (o->parent) o->world = o->parent->world * o->local;
        wrote = t;
        return true;
    }
    void FoldSet(RE::PlayerCharacter* p, const RE::NiTransform& r, const RE::NiTransform& l)
    {
        fold.yielded = false;
        fold.changed = false;
        const bool had = fold.wrote;
        const bool a = FoldWrite(p, "AnimObjectR1", r, fold.wroteR);
        const bool b = !fold.yielded && FoldWrite(p, "AnimObjectL1", l, fold.wroteL);
        if (fold.yielded) return;   // another driver owns the bones now; FoldFrame sees mode 0
        fold.wrote = a || b;
        if (had && fold.wrote) {
            if (fold.changed) { fold.stuck = 0; ++fold.lost; }
            else { ++fold.stuck; fold.lost = 0; fold.held = true; }
        }
        ++fold.frames;
    }
    void FoldStart(int mode, std::uint64_t backdateMs, const char* why)
    {
        const std::uint64_t now = GetTickCount64();
        if (fold.mode == 2 && mode == 2 && now - fold.t0 < 1000) return;   // Retract and the sheathe sound both mark the holster
        // SOUND's T11a fix (P15b) also raises drawStart on the generic melee equip sound (gun -> Mantis). A draw that carries both cues
        // must not restart an unfold already under way: that would snap the blades back to folded mid-motion
        if (fold.mode == 1 && mode == 1 && now - fold.t0 < 600) {
            REX::INFO("arms: fold3p: {} ignored: an unfold started {} ms ago", why, now - fold.t0);   // SOUND asked (8ea99c81)
            return;
        }
        auto* p = RE::PlayerCharacter::GetSingleton();
        const bool stow = StowApplies(p);
        const float sNow = (fold.mode != 0 && fold.stow) ? fold.s : -1.0f;   // a mode switch mid-fold carries the length (no jump)
        const bool shown = KeepShown();
        if (fold.pendingShow && !stow) KeepShowFromFold(p, "a fold without the stow replaced a draw still waiting");
        fold.mode = mode;
        fold.t0 = now - backdateMs;
        fold.frames = 0;
        fold.overwrittenLogged = false;
        fold.wrote = false; fold.stuck = 0; fold.lost = 0; fold.held = false; fold.stowedFrames = -1; fold.neverHeldLogged = false;
        fold.stow = stow;
        fold.pendingShow = false;
        fold.shownAt = now;
        if (!stow) fold.sFrom = 0.0f;
        else if (mode == 1) { fold.sFrom = sNow >= 0 ? sNow : (shown ? 0.0f : 1.0f); fold.pendingShow = !shown; }
        else fold.sFrom = sNow >= 0 ? sNow : 0.0f;
        fold.s = fold.sFrom;
        REX::INFO("arms: fold3p: {} ({})", mode == 1 ? "unfold" : "fold", why);
        if (stow) REX::INFO("arms: fold3p: stow from {:.2f}{}", fold.sFrom, fold.pendingShow ? ", shapes wait for our writes to hold" : "");
    }
    bool KeepMantisShown();
    void FoldEvent(RE::PlayerCharacter* p, const std::string& tag)
    {
        if (!g_ini.mantisFold3P) return;
        const bool mantis = WeapHas(EquippedWeapon(p), Kw("CW_AnimsCyberMantis"));
        // swapped off the Mantis (to a gun / fists): its UnEquip still plays and the visual is still shown, so the holster cue folds it
        const bool swapOut = !mantis && g_ini.mantisStow3P && KeepMantisShown();
        if (!mantis && !swapOut) return;
        if (tag == "drawStart") { if (mantis) FoldStart(1, 0, "draw cue"); }
        else if (tag == "foldRetract") {
            // AAA row 5: our Block clips carry a Retract too (P15 log 22:08:43.961 started a holster fold 1.1 s after the draw). A Block
            // happens with the weapon drawn; the holster's sheathe sound (always accepted, below) still starts the fold if this refuses
            if (mantis && p->GetWeaponMagicDrawn()) { REX::INFO("arms: fold3p: Retract with the weapon drawn (a Block) -> no holster fold"); return; }
            FoldStart(2, 33, swapOut ? "holster cue: Retract, swapped off the Mantis" : "holster cue: Retract");
        } else if (tag == "foldSheatheSnd") FoldStart(2, 100, swapOut ? "holster cue: sheathe sound, swapped off the Mantis" : "holster cue: sheathe sound");
    }
    bool FirstPerson()
    {
        auto* cam = RE::PlayerCamera::GetSingleton();
        return cam && cam->QCameraEquals(RE::CameraStates::kFirstPerson);
    }
    // the keep's sheathe hide waits while a stowing holster fold holds the bones (or is too young to have proved it yet)
    bool FoldHoldsHide()
    {
        return fold.mode == 2 && fold.stow && !fold.yielded && fold.stowedFrames < 0 && !FirstPerson() &&
               (fold.held || GetTickCount64() - fold.t0 < 400);
    }
    void FoldFrame(RE::PlayerCharacter* p)
    {
        if (!fold.mode || !p) return;
        const std::uint64_t now = GetTickCount64();
        const float t = (float)(now - fold.t0) / 1000.0f, f = t * fold3p::kFps;
        const fold3p::Stow* sr = fold.stow ? &fold3p::kStowR : nullptr;
        const fold3p::Stow* sl = fold.stow ? &fold3p::kStowL : nullptr;
        if (fold.mode == 1) {
            if (fold.pendingShow) {
                const char* why = nullptr;
                if (FirstPerson()) why = "camera in 1P: shown at once";
                else if (fold.stuck >= 2) why = "the stow fold's writes hold";
                else if (t > 1.5f) {   // the bones never held: give up waiting (drawn: show; not drawn: the keep keeps them hidden)
                    REX::WARN("arms: fold3p: our writes never held in {} frame(s) of the draw", fold.frames);
                    fold.pendingShow = false;
                    if (p->GetWeaponMagicDrawn()) KeepShowFromFold(p, "the draw's writes never held (fallback)");
                }
                if (why) {
                    REX::INFO("arms: fold3p: writes held {} frame(s) -> shapes shown {} ms after the draw cue", fold.stuck, now - fold.t0);
                    fold.pendingShow = false; fold.shownAt = now;
                    KeepShowFromFold(p, why);
                }
            }
            if (fold.stow) fold.s = fold.pendingShow ? fold.sFrom : fold.sFrom * (1.0f - Smooth01((float)(now - fold.shownAt) / (float)g_ini.foldStowOutMs));
            FoldSet(p, FoldKey(fold3p::kEquipR, fold3p::kEquipN, f, fold.s, sr), FoldKey(fold3p::kEquipL, fold3p::kEquipN, f, fold.s, sl));
            if (fold.yielded) {
                if (fold.pendingShow) { fold.pendingShow = false; KeepShowFromFold(p, "the fold yielded mid-draw"); }
                return;
            }
            if (f >= (float)(fold3p::kEquipN - 1) && !fold.pendingShow && fold.s <= 0.0f) { fold.mode = 0; REX::INFO("arms: fold3p: unfold done after {} frame(s)", fold.frames); }
        } else if (fold.mode == 2 && fold.stow) {
            // keys at clip time until iFoldStowAtMs; then over iFoldStowInMs the key eases to the last (folded) one while the blade
            // draws into the housing (fold3p_stow.py fold_state); stowed: cull at once, write the stowed pose 3 more frames, then identity
            const float at = (float)g_ini.foldStowAtMs / 1000.0f, in = (float)g_ini.foldStowInMs / 1000.0f, n1 = (float)(fold3p::kUnEquipN - 1);
            float fk = f, x = 0.0f;
            if (t >= at) { x = Smooth01((t - at) / in); fk = at * fold3p::kFps + (n1 - at * fold3p::kFps) * x; }
            fold.s = fold.sFrom + (1.0f - fold.sFrom) * x;
            if (fold.stowedFrames >= 0 && ++fold.stowedFrames > 3) {
                const RE::NiTransform id = RE::NiTransform::IDENTITY;
                FoldSet(p, id, id);
                fold.mode = 0; fold.wrote = false;
                REX::INFO("arms: fold3p: fold done (stowed) after {} frame(s), bones back to identity", fold.frames);
                return;
            }
            if (fold.stowedFrames < 0 && t >= at + in) {
                fold.stowedFrames = 0; fold.s = 1.0f;
                REX::INFO("arms: fold3p: stowed {} ms after the holster cue", now - fold.t0);
                KeepHideFromFold(p, "stowed");
            }
            FoldSet(p, FoldKey(fold3p::kUnEquipR, fold3p::kUnEquipN, fk, fold.s, sr), FoldKey(fold3p::kUnEquipL, fold3p::kUnEquipN, fk, fold.s, sl));
            if (fold.yielded) return;
            if (fold.stowedFrames < 0 && fold.held && fold.lost >= 2 && !p->GetWeaponMagicDrawn()) {   // the graph took the bones back
                REX::WARN("arms: fold3p: the graph took the blade bones back {} ms into the holster", now - fold.t0);
                KeepHideFromFold(p, "the graph took the blade bones back mid-holster");
                fold.mode = 0; fold.wrote = false;
                return;
            }
            if (!fold.held && !fold.neverHeldLogged && now - fold.t0 >= 400) {
                fold.neverHeldLogged = true;
                REX::WARN("arms: fold3p: holster writes have not held after 400 ms -> the keep hides as before");
            }
        } else if (fold.mode == 2) {
            if (t < (float)fold3p::kUnEquipN / fold3p::kFps + 0.6f) {   // hold the last (folded) key until the shapes are culled
                FoldSet(p, FoldKey(fold3p::kUnEquipR, fold3p::kUnEquipN, f), FoldKey(fold3p::kUnEquipL, fold3p::kUnEquipN, f));
            } else {
                const RE::NiTransform id = RE::NiTransform::IDENTITY;
                FoldSet(p, id, id);
                fold.mode = 0; fold.wrote = false;
                REX::INFO("arms: fold3p: fold done after {} frame(s), bones back to identity", fold.frames);
            }
        }
    }

    void Frame()
    {
        FinisherFrame();
        FoldFrame(RE::PlayerCharacter::GetSingleton());
        // queued graph re-pick: only from a neutral state, never mid-attack / lunge / finisher
        auto* pr = RE::PlayerCharacter::GetSingleton();
        const bool sprinting = pr && (pr->moveMode & 0x100);
        if (repick.pending && GetTickCount64() >= repick.notBefore && (repick.evenSprinting || !sprinting) && !GameplayMenuOpen() &&
            !lunge.active && !fin.active && !leapAttacking && !slam.swinging && !slam.pending) {
            repick.pending = false;
            RepickWeaponGraph(repick.why.c_str());
        }
        else if (repick.pending && g_ini.debug && GetTickCount64() >= repick.notBefore + 3000 && GetTickCount64() >= repick.nextWaitLog) {
            repick.nextWaitLog = GetTickCount64() + 5000;   // names what is holding the queue (P10: 109 s with no way to tell)
            const char* menu = OpenGameplayMenu();
            REX::INFO("arms: re-pick ({}) still waiting: menu {} sprinting {} lunge {} finisher {} leap {} slam {}/{}", repick.why, menu ? menu : "none",
                      sprinting, lunge.active, fin.active, leapAttacking, slam.swinging, slam.pending);
        }
        if (slam.driving && pr) {   // GOR-3 drive-down: from the air swing to the ground at 12 m/s (the Tick lands the slam)
            if (!Airborne(pr)) slam.driving = false;
            else if (auto* mh = pr->currentProcess ? pr->currentProcess->middleHigh : nullptr)
                if (auto* cc = mh->charController.get()) { RE::hkVector4f v; cc->GetLinearVelocityImpl(v); v.x *= 0.5f; v.y *= 0.5f; v.z = -12.0f; cc->SetLinearVelocityImpl(v); }
        }
        if (!lunge.active) return;
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto t = lunge.target.get();
        double secs = (double)(GetTickCount64() - lunge.t0) / 1000.0, dur = cw::LungeSeconds(leapParams);
        if (p && lunge.launch) { LaunchFrame(p, secs); return; }
        if (!p || !t || t->lifeState != 0) { EndLunge("target gone"); return; }
        auto tp = t->GetPosition();
        auto q = cw::LungePos(lunge.start, { tp.x, tp.y, tp.z }, secs / dur, leapParams);
        p->SetPosition(RE::NiPoint3{ (float)q.x, (float)q.y, (float)q.z }, true);
        if (secs >= dur + 0.15) EndLunge("timeout (no HitFrame)");
    }

    // The locomotion graph's "who moves the actor" flags. cw-int-2 T4: after a re-pick the walk and sprint clips played
    // (footsteps every 0.67 s) but the player moved 16-30 u in 1.5 s at a spot that sprinted 1200 u in cycle 1. A graph
    // left animation-driven does exactly that (clips without root motion move nobody). Logged on every STUCK line.
    constexpr const char* kDrivenVars[] = { "bAnimationDriven", "bGraphDriven", "bGraphDrivenTranslation", "bGraphDrivenRotation", "bFreezeSpeedUpdate" };
    std::string DrivenVars(RE::PlayerCharacter* p, bool* anyDriven)
    {
        auto* g = static_cast<RE::IAnimationGraphManagerHolder*>(p);
        std::string s;
        bool driven = false;
        for (const char* n : kDrivenVars) {
            bool v = false;
            const bool ok = g->GetGraphVariableImplBool(RE::BSFixedString(n), v);
            s += std::format("{}={} ", n, ok ? (v ? "1" : "0") : "?");
            if (ok && v && std::string_view(n) != "bGraphDrivenRotation") driven = true;
        }
        if (anyDriven) *anyDriven = driven;
        return s;
    }

    // bStuckUndrive (test ini): clear the translation flags once per stuck episode and say what the graph accepted
    void Undrive(RE::PlayerCharacter* p, const char* why)
    {
        auto* g = static_cast<RE::IAnimationGraphManagerHolder*>(p);
        std::string s;
        for (const char* n : { "bAnimationDriven", "bGraphDriven", "bGraphDrivenTranslation", "bFreezeSpeedUpdate" })
            s += std::format("{}:{} ", n, g->SetGraphVariableBool(RE::BSFixedString(n), false) ? "ok" : "refused");
        REX::INFO("arms: {} -> cleared the driven flags ({}), now {}", why, s, DrivenVars(p));
    }

    // The same variables as a cw::stuck::Driven snapshot, for cw::stuck::Decide (beta update 1). Rotation stays in
    // DrivenVars' log line only - it never roots a player and E's own `driven` excluded it.
    //
    // Each variable is read INDEPENDENTLY into a tri-state: a read that fails is Unknown, not False. Four of the
    // eleven episodes in the author's beta log print "?" for bGraphDrivenTranslation while the other reads succeed, and
    // one AND-ed `ok` over all four reads turned those four into "nothing is set" - which would have made this fix
    // do nothing on exactly the episodes inside his 87-second rooted stretch.
    cw::stuck::Driven ReadDriven(RE::PlayerCharacter* p)
    {
        auto* g = static_cast<RE::IAnimationGraphManagerHolder*>(p);
        cw::stuck::Driven d;
        auto rd = [&g](const char* n) {
            bool v = false;
            return g->GetGraphVariableImplBool(RE::BSFixedString(n), v) ? (v ? cw::stuck::Tri::True : cw::stuck::Tri::False)
                                                                        : cw::stuck::Tri::Unknown;
        };
        d.animationDriven = rd("bAnimationDriven");
        d.graphDriven = rd("bGraphDriven");
        d.graphDrivenTranslation = rd("bGraphDrivenTranslation");
        d.freezeSpeed = rd("bFreezeSpeedUpdate");
        return d;
    }

    // Pip-Boy open or Kiroshi zoom in 1P: hide every arm visual's shapes (blades, housings, knuckles, wire, launcher) (cw-int-2 vp-T8-pipboy.png: the folded left
    // blade and its housing stand across the Pip-Boy screen; abil-T4-zoom-held.png: the zoom narrows the 1P FOV too, which
    // swings a blade into the corner of the view). Restores exactly the shapes it hid. Game thread.
    void PipboyHideWalk(RE::NiAVObject* o, bool hide, std::vector<std::string>& hidden, bool spareHousing = false)
    {
        if (!o) return;
        if (auto* n = o->IsNode()) { for (const auto& c : n->children) PipboyHideWalk(c.get(), hide, hidden, spareHousing); return; }
        const char* nm = o->name.c_str();
        static constexpr const char* kArmShapes[] = { "MantisBlade", "MantisHousing", "GorillaKnuckles", "MonowireWire", "Launcher_" };
        if (!nm || std::none_of(std::begin(kArmShapes), std::end(kArmShapes), [nm](const char* pre) { return std::strncmp(nm, pre, std::strlen(pre)) == 0; })) return;
        if (hide && spareHousing && std::strncmp(nm, "MantisHousing", 13) == 0) return;   // bKeepHousing: the housing stays on the forearm
        if (hide) { if (!o->GetAppCulled()) { o->SetAppCulled(true); hidden.emplace_back(nm); } }
        else if (std::find(hidden.begin(), hidden.end(), nm) != hidden.end()) o->SetAppCulled(false);
    }
    struct ArmHide { bool on = false; std::vector<std::string> hidden; } armHide;   // reset on load (the 3D is rebuilt)
    void PipboyHide(RE::PlayerCharacter* p)
    {
        bool& on = armHide.on;
        std::vector<std::string>& hidden = armHide.hidden;
        auto* ui = RE::UI::GetSingleton();
        const bool pip = ui && ui->GetMenuOpen(RE::BSFixedString("PipboyMenu"));
        const bool want = pip || optics::zoomed;
        if (want == on) return;
        on = want;
        RE::NiAVObject* root = p->Get3D(true);
        if (want) { hidden.clear(); PipboyHideWalk(root, true, hidden); }
        else { PipboyHideWalk(root, false, hidden); }
        if (g_ini.debug) REX::INFO("arms: arm hide {} (Pip-Boy {}, zoom {}) -> {} {} arm shape(s)", want ? "on" : "off", pip, optics::zoomed, want ? "hid" : "restored", hidden.size());
        if (!want) hidden.clear();
    }

    // bPlsHidePipboy3P: in 3P the Pip-Boy covers the left forearm's outer face, so from the left at rest the
    // launcher shows only as an elbow rim and a band past the wrist (launcher_3p_visibility.py: 66 u^2 launcher vs 107 u^2
    // Pip-Boy; with the Pip-Boy gone, 125 u^2). While the PLS is installed, cull the 3P Pip-Boy's own shapes. Only the 3P root
    // (Get3D(false)): the 1P Pip-Boy and its menu are another tree and never touched. Only BSTriShape leaves named as the
    // vanilla / Pip-Boy Flashlight PipboyLowPlayer.nif names them; nodes and lights are never culled, so a light hung off the
    // Pip-Boy keeps working. Re-applied every tick (the 3D re-attaches on load, armor changes, race menu); restores exactly
    // the shapes it culled. Game thread.
    bool IsPip3PShape(RE::NiAVObject* o)
    {
        const char* nm = o->name.c_str();
        if (!nm) return false;   // a leaf (the walk recurses into nodes); the names below are BSTriShapes, never a light
        if (std::strncmp(nm, "PipboyBody", 10) == 0) return true;
        if (std::strcmp(nm, "Screen:0") != 0 && std::strcmp(nm, "ScreenGlowEffect01:0") != 0) return false;
        for (RE::NiAVObject* q = o->parent; q; q = q->parent) {   // a generic name: only under the Pip-Boy's own nodes
            const char* pn = q->name.c_str();
            if (pn && (std::strstr(pn, "Pipboy") || std::strstr(pn, "PipBoy"))) return true;
        }
        return false;
    }
    void PipCull3PWalk(RE::NiAVObject* o, bool hide, std::vector<std::string>& culled, int& n)
    {
        if (!o) return;
        if (auto* nd = o->IsNode()) { for (const auto& c : nd->children) PipCull3PWalk(c.get(), hide, culled, n); return; }
        if (!IsPip3PShape(o)) return;
        const std::string nm = o->name.c_str();
        if (hide) {
            if (!o->GetAppCulled()) { o->SetAppCulled(true); ++n; if (std::find(culled.begin(), culled.end(), nm) == culled.end()) culled.push_back(nm); }
        } else if (o->GetAppCulled() && std::find(culled.begin(), culled.end(), nm) != culled.end()) { o->SetAppCulled(false); ++n; }
    }
    struct PipCull3P { bool on = false; std::vector<std::string> culled; } pipCull3P;   // reset on load (the 3D is rebuilt)
    void PipCull3PTick(RE::PlayerCharacter* p, bool plsInstalled)
    {
        const bool want = g_ini.plsHidePipboy3P && plsInstalled;
        if (!want && !pipCull3P.on) return;
        RE::NiAVObject* r3 = p->Get3D(false);
        int n = 0;
        if (want) {
            if (r3) PipCull3PWalk(r3, true, pipCull3P.culled, n);
            if (!pipCull3P.on || n) REX::INFO("arms: 3P Pip-Boy cull on (PLS installed): culled {} shape(s) now, {} name(s) held", n, pipCull3P.culled.size());
            pipCull3P.on = true;
        } else {
            if (r3) PipCull3PWalk(r3, false, pipCull3P.culled, n);
            REX::INFO("arms: 3P Pip-Boy cull off ({}): restored {} shape(s)", g_ini.plsHidePipboy3P ? "PLS removed" : "ini off", n);
            pipCull3P = {};
        }
    }

    // bVisualKeep (anim-3: blades full length from their first frame with no unfold, and popping off mid-retract). The visual
    // ARMO is worn for as long as its arm weapon is EQUIPPED (no 3D attach latency at draw), and its shapes are culled while
    // the arm is sheathed. The first draw cue (drawStart = our Equip clip's t=0 sound, else weaponDraw) uncovers them, so
    // the clip's unfold plays on screen; weaponSheathe keeps them for iSheathHideMs so the retract finishes before they go.
    // Shares the walk with the Pip-Boy hide; while that hide is on, the keep never uncovers anything.
    struct Keep { bool shown = false; std::uint64_t hideAt = 0, uncoverAt = 0, sheatheAt = 0; std::vector<std::string> hidden; bool mantis = false, waitLogged = false, wait1PLogged = false; } keep;   // reset on load
    void KeepCull(RE::PlayerCharacter* p, bool hide)
    {
        if (!p) return;
        if (!hide && armHide.on) return;   // the Pip-Boy / zoom hide owns the shapes now; KeepTick uncovers them after it
        for (RE::NiAVObject* r : { p->Get3D(true), p->Get3D(false) })   // 1P and 3P wear the same ARMO
            if (r) PipboyHideWalk(r, hide, keep.hidden, g_ini.keepHousing);   // hide appends: shapes attached late are caught next tick
        if (!hide) keep.hidden.clear();
    }
    void KeepEvent(RE::PlayerCharacter* p, const std::string& tag)
    {
        const std::uint64_t now = GetTickCount64();
        if (tag == "weaponSheathe") {
            if (keep.shown && !keep.hideAt) { keep.hideAt = now + (std::uint64_t)g_ini.sheathHideMs; keep.sheatheAt = now; keep.wait1PLogged = false; }
            REX::INFO("arms: keep: weaponSheathe -> shapes hide in {} ms", g_ini.sheathHideMs);
            return;
        }
        // drawStart or weaponDraw: only for an arm with a visual (a gun's draw drops the visual in KeepTick)
        if (VisualFor(EquippedWeapon(p)).empty()) return;
        keep.hideAt = 0;
        if (fold.pendingShow) {   // bMantisStow3P: the fold uncovers them once our writes hold (no open-blade flash)
            REX::INFO("arms: keep: {} -> shapes wait for the stow fold's writes to hold", tag);
            return;
        }
        if (!keep.shown) {
            keep.shown = true;
            KeepCull(p, false);
            REX::INFO("arms: keep: {} -> shapes shown (worn {})", tag, visualArmo.empty() ? "none yet" : visualArmo);
        }
    }
    bool KeepShown() { return keep.shown; }
    // First person, Mantis: how far the holster has drawn the blades in, 0 = out, 1 = in the housing (min of the two), -1 = no bones.
    // The 1P UnEquip slides each blade along its own line (retarget/bladefold.py telescope_local: TELESCOPE_Y -31.3, TELESCOPE_DROP
    // -6 in hinge coords), so the mount bone's local translation is that vector times the amount: |t| / 31.87.
    float Retract1P(RE::PlayerCharacter* p)
    {
        RE::NiAVObject* r = p ? p->Get3D(true) : nullptr;
        if (!r) return -1.0f;
        float u = 2.0f;
        for (const char* b : { "AnimObjectR1", "AnimObjectL1" }) {
            RE::NiAVObject* o = r->GetObjectByName(RE::BSFixedString(b));
            if (!o) return -1.0f;
            const auto& t = o->local.translate;
            u = (std::min)(u, std::sqrt(t.x * t.x + t.y * t.y + t.z * t.z) / 31.87f);
        }
        return u;
    }
    // [Arms] iSheathHide1PMaxMs (ARMS, 2026-11-10; QD block 26: in 1P the blades were full length in T3b frames 85-86 and gone
    // in 87). The 1P UnEquip draws the blades in over its 16 frames, but the sheathe hide came at +220 ms from weaponSheathe, before the
    // retract showed. Hold the hide until both blade bones are 90 % in, or the cap: true = keep them shown this tick.
    bool Hold1PHide(RE::PlayerCharacter* p, std::uint64_t now)
    {
        if (!g_ini.sheathHide1PMaxMs || !keep.mantis || !keep.sheatheAt || !FirstPerson()) return false;
        const std::uint64_t since = now - keep.sheatheAt;
        const float u = Retract1P(p);
        if (u < 0.0f) return false;
        if (u >= 0.9f) { REX::INFO("arms: keep: 1P retract seen ({:.2f}) {} ms after weaponSheathe", u, since); return false; }
        if (since >= (std::uint64_t)g_ini.sheathHide1PMaxMs) { REX::WARN("arms: keep: 1P retract only at {:.2f} after {} ms (cap) -> hiding anyway", u, since); return false; }
        if (!keep.wait1PLogged) { keep.wait1PLogged = true; REX::INFO("arms: keep: 1P sheathe hide waits for the blade retract ({:.2f} in)", u); }
        return true;
    }
    bool KeepMantisShown() { return keep.mantis && keep.shown && !visualArmo.empty(); }
    bool StowApplies(RE::PlayerCharacter* p)
    {
        if (!p || !g_ini.mantisStow3P || !g_ini.mantisFold3P || !g_ini.visualKeep || FirstPerson()) return false;
        return WeapHas(EquippedWeapon(p), Kw("CW_AnimsCyberMantis")) || (keep.mantis && !visualArmo.empty());
    }
    void KeepShowFromFold(RE::PlayerCharacter* p, const char* why)
    {
        keep.hideAt = 0; keep.uncoverAt = 0; keep.waitLogged = false;
        if (visualArmo.empty()) return;   // not worn yet: KeepTick installs it shown (fold.pendingShow is off now)
        keep.shown = true;
        KeepCull(p, false);
        REX::INFO("arms: keep: {} -> shapes shown", why);
    }
    void KeepHideFromFold(RE::PlayerCharacter* p, const char* why)
    {
        keep.hideAt = 0; keep.waitLogged = false;
        if (!keep.shown) return;
        keep.shown = false;
        KeepCull(p, true);
        REX::INFO("arms: keep: {} -> shapes hidden", why);
    }
    void KeepTick(RE::PlayerCharacter* p, RE::TESObjectWEAP* w)
    {
        const std::uint64_t now = GetTickCount64();
        const std::string armo = VisualFor(w);
        const bool drawn = p->GetWeaponMagicDrawn();
        // swapped from a drawn arm to a gun / fists: the retract plays for iSheathHideMs before the visual comes off
        const bool swapOut = !visualArmo.empty() && armo.empty() && keep.shown;
        if (swapOut && !keep.hideAt) {
            // bMantisStow3P: the swap's Retract arrives ~335 ms after the change (P15 log 22:09:31), so the hide waits long enough
            // for that cue to start the stow fold (FoldEvent swapOut); the fold then holds the hide until the blade is stowed
            const int ms = (keep.mantis && StowApplies(p)) ? (std::max)(g_ini.sheathHideMs, 500) : g_ini.sheathHideMs;
            keep.hideAt = now + (std::uint64_t)ms;
            REX::INFO("arms: keep: weapon changed off the arm -> visual off in {} ms", ms);
        }
        if (keep.hideAt && now >= keep.hideAt) {
            if (FoldHoldsHide()) {   // bMantisStow3P: the holster fold culls the shapes itself once the blade is stowed
                if (!keep.waitLogged) { keep.waitLogged = true; REX::INFO("arms: keep: sheathe hide waits for the stow fold"); }
            } else if (Hold1PHide(p, now)) {   // first person: the hide waits until the blade bones show the retract
            } else { keep.hideAt = 0; keep.sheatheAt = 0; keep.shown = false; keep.waitLogged = false; REX::INFO("arms: keep: retract done -> shapes hidden"); }
        }
        if (armo != visualArmo && !(swapOut && keep.shown)) {   // the arm (or its element look) changed, or a non-arm weapon is in
            if (!visualArmo.empty()) { SetVisual(visualArmo, false); visualArmo.clear(); keep.hidden.clear(); }
            if (!armo.empty()) {   // installed sheathed: starts hidden
                visualArmo = armo; SetVisual(armo, true);
                keep.mantis = WeapHas(w, Kw("CW_AnimsCyberMantis"));
                // bMantisStow3P, a swap onto the Mantis: installed hidden; the draw's stow fold shows it once our writes hold
                // (KeepTick's 600 ms backup below still shows it when no draw cue comes)
                const bool viaFold = keep.mantis && StowApplies(p);
                keep.shown = drawn && (!viaFold || (fold.mode == 1 && fold.stow && !fold.pendingShow));
                if (viaFold && drawn && !keep.shown) REX::INFO("arms: keep: Mantis visual installed hidden: its draw's stow fold shows it");
            }
            visual.on = !armo.empty();
        }
        if (armo.empty()) return;
        // backup for a missed cue: drawn with no draw cue for 0.6 s shows; sheathed with nothing pending hides
        if (drawn && !keep.shown) {
            if (!keep.uncoverAt) keep.uncoverAt = now + 600;
            else if (now >= keep.uncoverAt) {
                keep.uncoverAt = 0; keep.shown = true; KeepCull(p, false);
                if (fold.pendingShow) { fold.pendingShow = false; fold.shownAt = now; REX::WARN("arms: keep: drawn 600 ms and the stow fold's writes never held -> shapes shown (backup)"); }
                else REX::INFO("arms: keep: drawn without a draw cue -> shapes shown (backup)");
            }
        } else keep.uncoverAt = 0;
        if (!drawn && keep.shown && !keep.hideAt) keep.hideAt = now + (std::uint64_t)g_ini.sheathHideMs;
        if (!keep.shown) KeepCull(p, true);                                  // every tick: the ARMO's 3D attaches a few frames after the equip
        else if (!keep.hidden.empty() && !armHide.on) KeepCull(p, false);   // shown while the Pip-Boy hide held them: uncover now
    }

    // ~30 Hz: leap readiness; 4 Hz: visual reconcile (a missed event, a load, or no hook)
    // ASK F, the arm-use hint. Raised at the install, SHOWN when the player can actually read it. core/cw_armhint.h
    // has the why: in game it was raised behind the full-screen ripperdoc menu, and the note's 2.6 s life ended
    // about 3.5 s before the menu came down, so the author never saw it once.
    cw::armhint::Hint g_armHint;
    void ArmHintTick()
    {
        if (!g_armHint.Pending()) return;
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p) return;
        cw::armhint::World w;
        w.ourViewUp = g_ui && g_view && !g_ui->IsHidden(g_view);   // GameplayMenuOpen does NOT cover our own view
        w.gameplayMenuOpen = GameplayMenuOpen();
        w.loadingSave = loadingSave;
        w.armDrawn = p->GetWeaponMagicDrawn() && IsArmWeap(EquippedWeapon(p));
        std::string text;
        if (!g_armHint.TakeIfDue(w, &text)) return;
        // It says WHICH branch it took. Tonight's recurring lesson: a path that may legitimately do nothing must
        // still say so, or its silence cannot be told apart from never having run.
        // HINT-1 (2026-11-13): route by VISIBILITY, not by the view's existence. `HudText()` alone sent this note
        // into a view that has never been seen to draw, and the log said "shown" while the screen showed nothing.
        // The hint is a ONE-SHOT answer to the author's ask F, so a note he cannot see is the same as no note at all.
        // SCOPE: this call site only. Every other hud::Event and HudText() caller is untouched on purpose - they are
        // repeating status lines, and this is the one message that gets exactly one chance to land.
        // AND IT SAYS WHICH ROUTE IT TOOK, because a log line reading "shown" with nothing on screen is what cost us
        // boot 2's hint rows: never again print "shown" without printing WHERE.
        // THE BRANCH TO OUR VIEW IS GONE, not merely unreachable (the lead's ruling, release review's condition 1):
        // there is no code path from this hint to `hud::Event`, so "the hint cannot reach the view" is something
        // you READ here rather than infer from a predicate. The compiler holds the rule too.
        static_assert(cw::armhint::HintUsesGameHudLine(false, true),
                      "HINT-1: the arm-use hint takes the game's HUD line even when our own view IS visible");
        static_assert(cw::armhint::HintUsesGameHudLine(false, false) && cw::armhint::HintUsesGameHudLine(true, true),
                      "HINT-1: and in every other combination too - nothing routes this hint to our view");
        // `hud::Visible()` is READ FOR THE LOG ONLY and must never decide the route. It is worth logging because
        // it is the one place that records whether our view was in a state to draw at the moment the hint fired -
        // which is the fact nobody had on boot 2, and the reason this cost a whole run.
        const bool ourView = hud::Visible();
        REX::INFO("arms: arm-use hint shown ('{}') via the GAME's HUD line (our HUD view visible: {}; it is never used for this hint)",
                  text, ourView ? "yes" : "NO");
        RE::SendHUDMessage::ShowHUDMessage(text.c_str(), "", false, false);
    }

    // EVERY LOAD BUMPS THIS, and the settle watch reads it as part of what the player is holding.
    // The pump is frozen across a load, so `loadingSave` is never SAMPLED true inside Tick, and an arm equipped
    // before AND after the load leaves the signature otherwise identical -- on the 1d45fbf2 boot there is no
    // settle line at all from 06:01:28 to 06:06:58, so the flags the load set simply survived. kPostLoadGame
    // always reaches OnLoad, so this is the deterministic half; the watch's tick-gap check is only a backstop.
    std::uint32_t loadGen = 0;

    void Tick(bool slow)
    {
        ArmHintTick();   // before the early-outs: a pending hint is owed even with no arm equipped right now
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p || g_map.arms.empty()) return;
        auto* w = EquippedWeapon(p);
        PipboyHide(p);
        PipCull3PTick(p, pls.on);
        if (p->moveMode & 0x100) lastSprintMs = GetTickCount64();
        if (w && !IsArmWeap(w)) lastOtherWeap = w->GetFormID();
        if (restoreAt && GetTickCount64() >= restoreAt) {   // an equipped arm was uninstalled and nothing replaced it
            restoreAt = 0;
            auto* f = lastOtherWeap ? RE::TESForm::GetFormByID(lastOtherWeap) : nullptr;
            auto* ow = f ? f->As<RE::TESObjectWEAP>() : nullptr;
            if (!w && ow && p->GetInventoryObjectCount(ow) > 0)
                if (auto* em = RE::ActorEquipManager::GetSingleton()) {
                    RE::BGSObjectInstance inst(ow, nullptr);
                    const bool ok = em->EquipObject(p, inst, 0, 1, nullptr, false, false, false, true, false);
                    REX::INFO("arms: arm uninstalled while equipped -> previous weapon {:08X} re-equipped ({})", lastOtherWeap, ok ? "ok" : "EquipObject returned false");
                }
        }
        g_cueArm.store((int)CueArmOf(w), std::memory_order_relaxed);
        snd::ChargeTick();   // an interrupted PLS throw never reached its fire event
        if (slow) {
            bool plsIn = false;
            auto* plsW = g_map.Get("CW_ProjectileLaunchSystem");
            if (auto* mh = p->currentProcess ? p->currentProcess->middleHigh : nullptr)
                for (auto& e : mh->equippedItems) if (plsW && e.item.object == plsW) plsIn = true;
            g_cuePls.store(plsIn, std::memory_order_relaxed);
            if (snd::Cues()) {   // Berserk: its timer's edge; a load only resets the state, it never fires a cue
                bool bz = false;
                for (auto& [id, n] : g_activeNative) if (n.handler.rfind("os_berserk_", 0) == 0 && TimerLeft(n.handler.c_str()) > 0) bz = true;
                if (bz != cueBerserk && !loadingSave) snd::Cue(bz ? "CW_SND_BerserkOn" : "CW_SND_BerserkOff");
                cueBerserk = bz;
            }
        }
        if (g_ini.visualKeep) KeepTick(p, w);
        if (slow && !g_ini.visualKeep) {
            std::string armo = VisualFor(w);
            if (visual.on && !armo.empty() && !visualArmo.empty() && armo != visualArmo) { Apply(-1, ""); visual.on = false; }   // element changed
            int c = visual.Reconcile(p->GetWeaponMagicDrawn(), !armo.empty(), GetTickCount64(), 1500);   // events drive it; this only backs them up
            if (c) REX::INFO("arms: reconcile -> visual {}", c > 0 ? "on" : "off");
            Apply(c, armo);
        }
        {
            const bool air = Airborne(p);
            // Zetatech Berserk: landing from a jump / fall (airborne > 0.6 s) while it runs knocks hostiles within slam_m down
            static std::uint64_t airSince = 0;
            const std::uint64_t nowMs = GetTickCount64();
            if (air && !airSince) airSince = nowMs;
            if (!air && airSince) {
                if (nowMs - airSince > 600)
                    if (auto* z = Find("os_berserk_zetatech"); z && TimerLeft("os_berserk_zetatech") > 0) {
                        const double r = Pn(*z, "slam_m", 5) * 70.0;
                        auto pp = p->GetPosition();
                        int k = 0;
                        if (auto* pl = RE::ProcessLists::GetSingleton())
                            for (auto& h : pl->highActorHandles) {
                                auto a = h.get();
                                if (!a || a.get() == p || a->lifeState != 0 || !IsHostile(a.get(), p) || !a->currentProcess) continue;
                                auto ap = a->GetPosition();
                                if ((ap.x - pp.x) * (ap.x - pp.x) + (ap.y - pp.y) * (ap.y - pp.y) > r * r) continue;
                                a->currentProcess->KnockExplosion(a.get(), pp, 1.0f);
                                ++k;
                            }
                        REX::INFO("arms: Zetatech landing slam knocked {} down", k);
                    }
                airSince = 0;
            }
            // FO4 holds a melee attack pressed in the air until the landing (in game 2026-11-08: JumpUp -> slam READY,
            // landing -> off, THEN weaponSwing), so readiness lasts kSlamCoyoteMs past the landing; the attack that
            // starts on touchdown picks the slam entry.
            static std::uint64_t lastAir = 0;
            constexpr std::uint64_t kSlamCoyoteMs = 700;
            if (air) lastAir = nowMs;
            const bool airish = air || (lastAir && nowMs - lastAir < kSlamCoyoteMs);
            // buffer fallback (P15 M8 +250): in that jump the re-pick's initiateEnd never came (READY 13.232 -> off 14.801, none
            // between), so a kept press could never replay. Replay it kSlamReplayMs after the re-pick if initiateEnd is late (T5b
            // took 481 ms, P15's +620 jump 891 ms), and once more if no swing followed within kSlamRetryMs while still airish.
            constexpr std::uint64_t kSlamReplayMs = 700, kSlamRetryMs = 250;
            if (slam.ready && !slam.swinging && airish) {
                const bool late = slam.buffered && slam.repicking && nowMs - slam.repickAt >= kSlamReplayMs;
                const bool retry = !slam.buffered && slam.replayAt && slam.replays < 2 && nowMs - slam.replayAt >= kSlamRetryMs;
                if (late || retry) {
                    if (late) { slam.buffered = false; slam.repicking = false; }
                    const bool ok = PerformPlayerAction("ActionRightAttack");
                    REX::INFO("arms: ground slam: attack pressed during the re-pick replayed ({}, airborne {}, {})", ok ? "queued" : "REFUSED", air,
                              late ? "fallback: no initiateEnd after " + std::to_string(nowMs - slam.repickAt) + " ms" : "retry: no swing after the replay");
                    slam.replayAt = nowMs; ++slam.replays;
                }
            }
            if (slam.pending && !air) { DoSlam(); slam.pending = false; slam.swinging = false; SetSlamReady(false); }
            else if (!slam.swinging && !slam.pending)
                SetSlamReady((g_ini.leapRepick || g_ini.slamNoRepick) && p->GetWeaponMagicDrawn() && airish && WeapHas(w, Kw(S(g_map.arms.value("slam", json::object()), "weaponKeyword"))));   // the slam needs a re-pick too: off with bLeapRepick
        }
        if (slow) { PlsTick(GetTickCount64()); HurtTick(); ReattachTick(GetTickCount64()); }
        {
            // sprint-stuck watchdog (the re-pick freeze, LESSONS 2026-11-08): the sprint bit is set, no lunge is
            // moving the player, but the position stays put. Always log the episode (start + recovery time); with
            // the leap seam on, also try ONE recovery per episode: drop the leap keyword and queue a re-pick.
            // A modal pause can false-trip the 700 ms timer; read these lines next to the modal clicks in the log.
            static RE::NiPoint3 stuckLast; static std::uint64_t stillSince = 0; static bool stuckLogged = false;
            const std::uint64_t nowMs = GetTickCount64();
            // The leap attack's own clip roots the player after the landing (MANTIS MOTION's retimed clip: landing at 1.17 s,
            // attackStop at 2.27 s = 1.1 s standing). A player still holding sprint through it is not stuck: without this the
            // watchdog tripped at 700 ms, undrove the graph mid-recovery (bStuckUndrive) or dropped READY and queued a re-pick
            // that replayed a draw after every leap. Bounded at 3 s from the swing, so a clip that never sends AttackStop
            // cannot switch the watchdog off for good.
            const bool leapClip = leapAttacking && nowMs - leapSwingAt < 3000;
            if ((p->moveMode & 0x100) && !lunge.active && !leapClip) {
                auto pp = p->GetPosition();
                const double d2 = (double)(pp.x - stuckLast.x) * (pp.x - stuckLast.x) + (double)(pp.y - stuckLast.y) * (pp.y - stuckLast.y);
                if (d2 > 1.0) {
                    if (stuckLogged) REX::INFO("arms: sprint unstuck after {} ms", nowMs - stillSince);
                    stillSince = 0; stuckLogged = false;
                } else if (!stillSince) stillSince = nowMs;
                else if (nowMs - stillSince > 700 && !stuckLogged) {
                    stuckLogged = true;
                    bool driven = false;
                    const std::string vars = DrivenVars(p, &driven);
                    REX::INFO("arms: sprint STUCK {} ms at ({:.0f},{:.0f},{:.0f}) moveMode {:X} leapReady {} attacking {} repickPending {} graph {}",
                              nowMs - stillSince, pp.x, pp.y, pp.z, (std::uint32_t)p->moveMode, leapReady, leapAttacking, repick.pending, vars);
                    if (g_ini.stuckUndrive && driven) Undrive(p, "sprint STUCK");   // instead of the re-pick: see whether that alone frees it
                    else {
                        // 2026-11-13: the re-pick IS the recovery for the graph-selection wedge, so it may no longer
                        // sit behind a GAMEPLAY feature key. With bLeapRepick=0 (what the author plays) this whole branch
                        // used to do nothing at all. Dropping leap READY is leap bookkeeping and stays behind the key.
                        if (g_ini.leapRepick) { SetLeapReady(false); leapRearmAfter = GetTickCount64() + 2000; }
                        // THE SECOND RECOVERY ROUTE. It never calls Decide, so kAutoRepickGraph does not reach
                        // it and flipping that constant alone would leave a live re-pick - the card would then
                        // be false where it says the mod does not try to free him (release review, 2026-11-13).
                        // The `sprint STUCK` line above is logged either way, so a wedge is still reported.
                        if constexpr (cw::stuck::kAutoRepickOnSprintStuck) RequestRepick("stuck recovery", true);
                    }
                }
                stuckLast = pp;
            } else {
                if (stuckLogged) REX::INFO("arms: sprint-stuck episode ended (sprint bit off, lunge or leap clip) after {} ms", nowMs - stillSince);
                stillSince = 0; stuckLogged = false;
            }
        }
        {
            // walk / run watchdog (cw-int-2 T4: walking under W moved 16-30 u in 1.5 s after a re-pick, before any
            // sprint). The episode state and the recovery policy live in core/cw_stuck.h (beta update 1, 2026-11-12):
            // the engine leaves the locomotion graph translation-driven around an arm weapon's equip / subgraph
            // change and nothing on E cleared it (the author's beta session: stuck 0-3 u from the install's first draw,
            // through sheathe and redraw), so a stuck episode with the graph driven now runs the SAME Undrive the
            // test key used - gated by cw::stuck::PolicyClears on nothing legitimate owning the body (attack, lunge,
            // finisher, scene, airborne, load, menus, a real animation via bAnimationDriven; the full guard list is
            // docs/integration/BETA-UPDATE1-GOOD-20261112.md). Log words unchanged: "move STUCK" / "move unstuck".
            static cw::stuck::MoveWatch mvWatch;
            static cw::stuck::EquipSettleWatch eqSettle;
            static cw::stuck::Recoveries recoveries;
            static cw::stuck::Recoveries repicks;            // counted apart from the clears: a different action, a different cost
            static std::uint64_t repickedAt = 0;   // when step one last fired, for cw::stuck::kPairWindowMs
            static bool repickStoodDownSaid = false;   // the stand-down says itself once, not every tick
            const std::uint64_t nowMs = GetTickCount64();
            cw::stuck::Probe pr;
            pr.moveMode = (std::uint32_t)p->moveMode;
            pr.armEquipped = IsArmWeap(w);
            pr.menuOpen = GameplayMenuOpen();
            pr.lungeActive = lunge.active;
            pr.attackInFlight = leapAttacking || fin.active || slam.swinging || repick.pending;
            pr.sceneBusy = scene::Active();
            pr.airborne = Airborne(p);
            pr.loadingSave = loadingSave;
            pr.inPowerArmor = InPowerArmor(p);
            pr.testUndrive = g_ini.stuckUndrive;
            pr.armDrawn = pr.armEquipped && p->GetWeaponMagicDrawn();
            pr.firstPerson = FirstPerson();
            // One clear, with its reason and the running rate. The author's rule: a recovery that fires constantly is a
            // failure even if he can move, so EVERY decision is logged and every clear is counted and rate-limited.
            // `fromMeasuredStuck` IS THE PERMISSION TO RE-PICK, and only MoveWatch's measured episode passes true.
            // A settle passes false, so Decide cannot answer RepickGraph for it and the result falls back to exactly
            // what shipped before the re-pick existed: a refusal, and nothing done. In game the equip settle routed
            // the ordinary healthy reading to a re-pick and replayed the draw twelve times (2026-11-13 09:35Z).
            cw::stuck::Decision lastDecision = cw::stuck::Decision::NoClear;   // read by the settle: a complete answer ends the retries
            const auto recover = [&](const char* why, bool fromMeasuredStuck) {
                cw::stuck::Probe pq = pr;
                pq.measuredStuck = fromMeasuredStuck;
                const auto d = cw::stuck::Decide(pq, ReadDriven(p));
                lastDecision = d;
                // THE FLAGS AS THEY READ BEFORE WE TOUCH THEM. Undrive logs the after-state, and two boots were
                // spent not knowing what the before-state had been (the lead, 2026-11-13).
                const std::string beforeVars = DrivenVars(p);
                if (cw::stuck::Repicks(d)) {
                    // CASE 6: nothing to undrive, so RE-SELECT the graph. QUEUED through RequestRepick and never
                    // called directly - an unqueued ChangeAnimFlavor landing mid-sprint or mid-attack plants the
                    // player at a fixed position (the re-pick freeze, 2026-11-08), which is to say the naive version
                    // of this fix CAUSES the bug it fixes. Frame()'s gate already waits for a neutral state.
                    // STOOD DOWN before suppressed: three attempts in a minute that have not freed him mean the
                    // re-pick is not the remedy here, and replaying a draw every 2 s for as long as he stands there
                    // would be the recovery becoming the defect. One line, then quiet until the minute rolls over.
                    if (cw::stuck::RepickStoodDown(repicks.InMinute())) {
                        if (!repickStoodDownSaid) {
                            repickStoodDownSaid = true;
                            REX::WARN("arms: {} -> {} re-picks this minute have not freed him: STANDING DOWN until the minute rolls over (he is stuck by something a re-pick cannot cure)",
                                      why, repicks.InMinute());
                        }
                        return false;
                    }
                    repickStoodDownSaid = false;
                    const bool rp = repicks.Allow(nowMs);
                    REX::INFO("arms: {} -> {}{} BEFORE {} (re-picks {} this session, {} this minute, {} suppressed)", why, cw::stuck::Word(d),
                              rp ? "" : ", SUPPRESSED (too soon after the last)", beforeVars, repicks.Total(), repicks.InMinute(), repicks.Suppressed());
                    if (rp) {
                        RequestRepick(why);
                        repickedAt = nowMs;
                        // STEP ONE ONLY. The newly selected graph is expected to set the driven flags (operator's A1:
                        // under the Test overlay the settle logged `clear (flags known set)` and THAT is what freed
                        // him), so the episode is re-armed to let its own StuckStart treat step two. Without this the
                        // episode stayed open, no second StuckStart came, and he was left one step short.
                        const auto rp2 = p->GetPosition();
                        mvWatch.ReArm(nowMs, rp2.x, rp2.y);
                    }
                    return rp;
                }
                // A clear inside the pair window is STEP TWO of the re-pick that just fired, so it must not be
                // rate-limited away (the lead's condition: the budget may not strand him between the steps).
                const bool paired = repickedAt && nowMs - repickedAt < cw::stuck::kPairWindowMs;
                const bool fires = cw::stuck::Clears(d) && recoveries.Allow(nowMs, paired);
                REX::INFO("arms: {} -> {}{} BEFORE {} (clears {} this session, {} this minute, {} suppressed)", why, cw::stuck::Word(d),
                          cw::stuck::Clears(d) && !fires ? ", SUPPRESSED (too soon after the last)" : "",
                          beforeVars, recoveries.Total(), recoveries.InMinute(), recoveries.Suppressed());
                if (fires) Undrive(p, why);
                return fires;
            };
            const auto pp = p->GetPosition();
            const auto st = mvWatch.Tick(nowMs, pp.x, pp.y, pr);
            if (st == cw::stuck::Step::StuckStart) {
                REX::INFO("arms: move STUCK {:.0f} u in {} ms at ({:.0f},{:.0f},{:.0f}) moveMode {:X} graph {}", mvWatch.LastUnits(), mvWatch.LastWindowMs(), pp.x, pp.y, pp.z, (std::uint32_t)p->moveMode, DrivenVars(p));
                (void)recover("move STUCK", true);   // THE ONLY caller permitted to re-pick: MoveWatch measured it
            } else if (st == cw::stuck::Step::Unstuck) {
                REX::INFO("arms: move unstuck ({:.0f} u in {} ms)", mvWatch.LastUnits(), mvWatch.LastWindowMs());
            }
            // the equip settle: the wedge lands with the equip's subgraph change (install -> draw -> first stuck walk
            // eleven seconds later), so check once it has settled and clear before the player ever walks (retries
            // while gated, gives up, one shot per equip edge, never armed by a load)
            // THE SETTLE'S CHOKE POINT: what the player is HOLDING, as this tick sees it. Every route that puts an
            // arm in his hands or takes one out changes this, including the ripperdoc menu's 10 ms swap that the old
            // level-only test could not see. The clock is held while a menu or our own view is up, so the 2.5 s and
            // the give-up both count from the moment he can walk rather than from the install behind the menu.
            cw::stuck::EquipSig sig;
            sig.armForm = (pr.armEquipped && w) ? (std::uint32_t)w->GetFormID() : 0u;
            sig.loadGen = loadGen;
            sig.plsHeld = g_cuePls.load(std::memory_order_relaxed);
            sig.drawn   = pr.armDrawn;
            sig.firstPerson = pr.firstPerson;   // the camera picks the graph: a POV change re-arms the settle
            const bool viewUp = pr.menuOpen || (g_ui && g_view && !g_ui->IsHidden(g_view));
            if (eqSettle.Due(nowMs, sig, viewUp, pr.loadingSave)) {
                const bool fired = recover("arm equip settle", false);   // may CLEAR; may NEVER re-pick
                // A COMPLETE ANSWER ENDS THE RETRIES. Before this the watch retried through every refusal, so a
                // healthy draw cost four identical "flags all known clear" lines; now it costs one. Only
                // RefusedGuard and RefusedIncomplete -- something owns the body, or the read was mid-transition --
                // are worth asking again about.
                if (lastDecision == cw::stuck::Decision::RefusedIncomplete) {
                    // THE READ COULD NOT BE TAKEN, so it does not count against the watch. In 1P the 3P movers
                    // answer '?' and the old watch spent all four retries on them and gave up while the player
                    // was simply in the other camera (rig operator 6). The question stays owed instead.
                    if (!eqSettle.Incomplete(nowMs))
                        REX::INFO("arms: arm equip settle -> no complete graph read in {} ms (every check came back '?', which is what first person gives for the third-person movers): STANDING DOWN - an equip, a draw, a load or a camera change arms a fresh one",
                                  eqSettle.OwedMaxMs());
                } else if (fired || cw::stuck::Settled(lastDecision)) {
                    eqSettle.Satisfied();
                }
            }
            // THE PROACTIVE DRAW RE-PICK IS GONE FROM THE RELEASE (the lead's ruling, 2026-11-13 09:37Z).
            // It pre-empted the wedge at rest so the player never felt the stuck second, and in a healthy boot it
            // did real harm instead: the equip settle's ordinary `every mover clear` reading armed it off a false
            // trigger and it then re-picked ONCE A SECOND, TWELVE TIMES, over 13.2 s, replaying the draw each time,
            // with nothing wedged and the player walking 492.6 u. Only a CLEAR ended its watch, and on a healthy
            // graph no clear ever comes - so the retry had no terminator.
            // The release is therefore REACTIVE-ONLY: the single route to a re-pick is MoveWatch's MEASURED
            // `move STUCK` above. The pre-emptive version, with its arming gate repaired, is parked for its own
            // rig turn - it must not come back without one.
        }
        if (lunge.active) return;
        bool want = false;
        // bLeapRepick off: no leap at all. Without the re-pick the keyword selects nothing, and the lunge would start on a plain swing.
        // Not gated on sprinting any more (see RepickReq): READY arms while the player walks toward a target, so the re-pick
        // lands before the sprint starts. The leap set overrides only AttackSprinting, so READY changes nothing else.
        if (g_ini.leapRepick && p->GetWeaponMagicDrawn() && WeapHas(w, Kw(S(g_map.arms, "leapWeaponKeyword")))) {
            auto pp = p->GetPosition();
            std::vector<cw::LeapCandidate> cands;
            std::vector<RE::ActorHandle> handles;
            if (auto* pl = RE::ProcessLists::GetSingleton()) {
                for (auto& h : pl->highActorHandles) {
                    auto a = h.get();
                    if (!a || a.get() == p) continue;
                    auto ap = a->GetPosition();
                    cw::LeapCandidate c;
                    c.pos = { ap.x, ap.y, ap.z };
                    c.alive = a->lifeState == 0;
                    c.hostile = c.alive && IsHostile(a.get(), p);
                    c.los = true;   // checked only for the pick below (HasLOSToTarget is not cheap)
                    cands.push_back(c);
                    handles.push_back(h);
                }
            }
            for (int tries = 0; tries < 3; ++tries) {
                int i = cw::PickLeapTarget({ pp.x, pp.y, pp.z }, p->GetHeading(), cands, leapParams);
                if (i < 0) break;
                bool pick = false;
                auto a = handles[i].get();
                if (a && p->HasLOSToTarget(a.get(), &pick)) { want = true; leapTarget = handles[i]; break; }
                cands[i].los = false;
            }
            // P15 T5a was VOID with no clue why: say, once a second, why the nearest live hostile is not a leap target
            static std::uint64_t leapWhyAt = 0;
            if (!want && g_ini.debug && GetTickCount64() - leapWhyAt >= 1000) {
                leapWhyAt = GetTickCount64();
                int n = -1; double nd = 1e18;
                for (std::size_t k = 0; k < cands.size(); ++k) {
                    if (!cands[k].alive || !cands[k].hostile) continue;
                    const double dx = cands[k].pos.x - pp.x, dy = cands[k].pos.y - pp.y, dz = cands[k].pos.z - pp.z;
                    const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
                    if (d < nd) { nd = d; n = (int)k; }
                }
                if (n >= 0 && nd < 2000) {
                    const double dx = cands[n].pos.x - pp.x, dy = cands[n].pos.y - pp.y, h = std::max(1e-6, std::sqrt(dx * dx + dy * dy));
                    const double ang = std::acos(std::clamp((dx * std::sin(p->GetHeading()) + dy * std::cos(p->GetHeading())) / h, -1.0, 1.0)) * 180.0 / 3.14159265358979;
                    bool pk = false;
                    auto a = handles[n].get();
                    const bool los = a && p->HasLOSToTarget(a.get(), &pk);
                    REX::INFO("arms: leap: no target; nearest hostile {:08X} at {:.0f} u (need {:.0f}-{:.0f}), {:.0f} deg off the heading (need <= {:.0f}), LOS {}",
                              a ? a->GetFormID() : 0u, nd, leapParams.minUnits, leapParams.maxUnits, ang, leapParams.coneDeg, los);
                }
            }
        }
        // a re-pick replays a weapon draw: hold the keyword through the attack and for a moment after, so it never cuts a clip
        // and a target flickering at the cone edge does not re-pick every tick
        static std::uint64_t leapLastWanted = 0;
        const std::uint64_t leapNow = GetTickCount64();
        if (want || leapAttacking) leapLastWanted = leapNow;
        if (!want && leapReady && leapLastWanted && leapNow - leapLastWanted < 3000) want = true;   // 3 s: each re-pick replays a draw
        if (leapNow < leapRearmAfter) want = false;
        SetLeapReady(want);
    }

    // a save was loaded / a new game: forget session state; the 4 Hz reconcile re-equips or removes the visual
    void OnLoad()
    {
        ++loadGen;   // the settle watch re-arms on this: a load re-selects the subgraph and the pump is frozen across it
        g_armHint.Forget();   // ASK F: a one-shot about something he just did, never saved state - no replay on load
        lunge = {};
        CueOnLoad();
        fold = {};      // the 3D is rebuilt: nothing of ours is on the new bones
        slam = {};      // AddKeyword is not saved: the keyword is gone with the session
        worn.clear();   // handlers re-run enable after a load and re-wear the right look
        reattach.clear();
        armHeld.clear();
        fin = {}; finNextOk = 0;
        heldKill = {};   // the loaded actor state comes from the save
        weak = {}; weakShownFor = 0; weakShownGroup = -2;
        rmods.Reset();
        wscale.inst = nullptr; wscale.hold.reset(); wscale.mReload = wscale.mSpeed = 1;   // previous session's instance; the save never held our scaling
        pls.on = false; pls.nextAt = 0; pls.last = 0; pls.lastTick = 0;   // the handler re-enables it from the save; left on, the recharge fed a save without the PLS
        leapAttacking = false;
        leapReady = false;   // AddKeyword is not saved with the game (assumed; logged when it is set again)
        repick = {};         // a queued re-pick belongs to the previous session's graph
        lastSprintMs = 0; leapRearmAfter = 0; finHeldFor = 0;
        armHide = {};        // the previous session's culled shapes are gone with its 3D
        pipCull3P = {};      // same: the next tick culls the new 3P Pip-Boy if the PLS is still installed
        keep = {};
        // State the handlers' disable never ran for (g_activeNative.clear() skips them): enable re-applies what this save has.
        element.clear();
        pls.extra = 0; pls.regenPct = 0; pls.pool = 1;   // the PLS mods add these up again on enable; left as-is they grew every load
        pls.loading = true;                               // the PLS enable that follows must not refill the saved charges
        loadingSave = true; restoreAt = 0; lastOtherWeap = 0; armSwapAt = 0;
        {   // once per save, so a lock another mod or a quest sets later survives; a new game has nothing to repair
            const bool repair = g_ini.clearEquipLocks && !g_equipLocksDone;
            const int locks = EquipLocks(repair);
            if (repair) { g_equipLocksDone = true; REX::INFO("arms: load: equip lock repair ran for this save: {} weapon stack(s) cleared", locks); }
            else if (g_ini.clearEquipLocks) REX::INFO("arms: load: equip lock repair already done for this save ({} locked weapon stack(s) left)", locks);
            else if (locks > 0) REX::INFO("arms: load: {} weapon stack(s) carried the equip lock (left: bClearEquipLocks=0)", locks);
        }
        {   // rotor: back to the record's speed (the forms outlive the session), on the equipped instance too (ARMS review R3)
            std::vector<std::string> names;
            for (auto& [wn, sp] : baseSpeed) names.push_back(wn);
            for (auto& wn : names) SetArmSpeed(wn, 1.0);
        }
        {   // tier damage: back to the record; the installed arm's enable sets its tier again after the load's Sync
            std::vector<std::string> names;
            for (auto& [wn, rec] : baseArmDamage) names.push_back(wn);
            for (auto& wn : names) SetArmDamage(wn, 0);
            if (!basePlsDamage.empty()) SetPlsDamage(0);
        }
        PlsRound("explosive");                            // the PLS projectile's explosion back to the default round
        if (auto* p0 = RE::PlayerCharacter::GetSingleton())   // the actor keywords our code sets (in case a save kept one): enable re-adds them
            for (const std::string& k : { S(g_map.arms, "leapKeyword"), S(g_map.arms.value("slam", json::object()), "keyword"), PlsS("keyword") })
                if (auto* kw = Kw(k)) p0->RemoveKeyword(kw);
        visual.on = false;
        visualArmo.clear();
        // Drawn visuals start OFF: a sheathed visual stays in the inventory (non-playable), so "in the inventory" is not "worn"
        // (the old guess took the last base ARMO found and missed element variants: the stale Monowire unequip). Unequip
        // every visual ARMO and variant the player holds; the 4 Hz reconcile puts the right one back if a weapon is drawn.
        auto* p = RE::PlayerCharacter::GetSingleton();
        const json ev = g_map.arms.value("elementVisuals", json::object());
        for (auto& [kw, armo] : g_map.arms.value("visuals", json::object()).items()) {
            if (!armo.is_string()) continue;
            std::vector<std::string> names{ armo.get<std::string>() };
            if (ev.contains(armo.get<std::string>()) && ev[armo.get<std::string>()].is_object())
                for (auto& [el, v] : ev[armo.get<std::string>()].items()) if (v.is_string()) names.push_back(v.get<std::string>());
            for (auto& n : names) {
                auto* f = g_map.Get(n);
                if (p && f && p->GetInventoryObjectCount(f->As<RE::TESBoundObject>()) > 0) SetVisual(n, false);
            }
        }
    }

    // inventory: the arm WEAP follows its installed cyberware (native handlers ar_mantis / ar_gorilla / ar_monowire)
    void RegisterHandlers()
    {
        int n = 0;
        for (auto& [edid, w] : g_map.arms.value("weapons", json::object()).items()) {
            std::string handler = S(w, "handler");
            if (handler.empty()) continue;
            g_handlers[handler] = NativeHandler{
                [handler](const cw::NativeAbility& a) {
                    if (const int dmg = cw::ArmWeaponDamage(a); dmg > 0)   // before the WEAP goes into the inventory and the hands
                        for (auto& wn : ArmWeapons(handler)) SetArmDamage(wn, dmg);   // the base and its three element variants
                    HoldArmWeapon(handler, true);
                    if (!loadingSave) {   // a real install (not a save load): the arm goes into the hands with a hint (USE-CASES row 1)
                        auto* p = RE::PlayerCharacter::GetSingleton();
                        auto* o = Obj(ArmWeapFor(handler));
                        auto* em = RE::ActorEquipManager::GetSingleton();
                        if (p && o && em && EquippedWeapon(p) != o->As<RE::TESObjectWEAP>()) {
                            RE::BGSObjectInstance inst(o, nullptr);
                            if (!em->EquipObject(p, inst, 0, 1, nullptr, false, false, false, true, false)) REX::WARN("arms: {} install: EquipObject returned false", handler);
                        }
                        const std::string nm = handler == "ar_mantis" ? "Mantis Blades" : handler == "ar_gorilla" ? "Gorilla Arms" : handler == "ar_monowire" ? "Monowire" : "Arm cyberware";
                        // ASK F: do NOT show it here. The ripperdoc menu covers the screen and the note lives
                        // 2.6 s, so it was born behind the menu and removed itself ~3.5 s before the world came
                        // back (install 03:33:20.186, menu closed 03:33:26.344, stills at +2/+4/+6 s empty).
                        g_armHint.Raise(nm);
                        REX::INFO("arms: {} equipped on install", handler);
                    }
                    // leap_m scales with the tier (T1 2.4 m = 168 u), which is below minUnits (280 u): no target ever qualified, and "leap READY"
                    // never fired in game. The leap reaches 12 m at every tier; the tier scales damage, not reach.
                    if (handler == "ar_mantis") leapParams.maxUnits = std::max(Pn(a, "leap_m", 12), 12.0) * 70.0;
                    REX::INFO("arms: {} installed ({}, t{})", handler, a.ability, a.tier);
                },
                [handler](const cw::NativeAbility& a) {
                    HoldArmWeapon(handler, false);
                    for (auto& wn : ArmWeapons(handler)) SetArmDamage(wn, 0);   // the record's damage back (an upgrade's enable follows at once)
                    REX::INFO("arms: {} uninstalled ({})", handler, a.ability);
                } };
            ++n;
        }
        // installed visuals ride on the arm handler (Gorilla) or the PLS handler
        if (auto it = g_handlers.find("ar_gorilla"); it != g_handlers.end()) {
            auto en = it->second.enable, di = it->second.disable;
            it->second = NativeHandler{ [en](const cw::NativeAbility& a) { if (en) en(a); WearInstalled("ar_gorilla", true); },
                                        [di](const cw::NativeAbility& a) { if (di) di(a); WearInstalled("ar_gorilla", false); } };
        }
        if (!PlsS("weapon").empty()) {
            g_handlers["ar_pls"] = NativeHandler{
                [](const cw::NativeAbility& a) {
                    pls.on = true; pls.base = (int)std::lround(Pn(a, "charges", 1)); pls.rechargeS = Pn(a, "recharge_s", 25);
                    if (const int dmg = cw::ArmWeaponDamage(a); dmg > 0) SetPlsDamage(dmg);   // the installed tier's explosion damage
                    auto* p = RE::PlayerCharacter::GetSingleton();
                    auto* w = Obj(PlsS("weapon"));
                    if (p && w) {   // a fresh install = full charges; a load keeps the saved count (it refilled every load: free ammo)
                        std::uint32_t c = p->GetInventoryObjectCount(w), mx = (std::uint32_t)std::max(1, pls.base + pls.extra);
                        if (c < mx && !pls.loading) p->AddObjectToContainer(w, RE::BSTSmartPointer<RE::ExtraDataList>{}, (std::int32_t)(mx - c), nullptr, RE::ITEM_REMOVE_REASON::kNone);
                        if (!pls.loading || c > 0) PlsEquip();
                        pls.last = pls.loading ? c : mx;
                    }
                    const bool afterLoad = pls.loading;
                    pls.loading = false;
                    SetKeyword(PlsS("keyword"), true);
                    PlsRepick("PLS installed", afterLoad ? 4000 : 50);   // after a load, give the world time to settle first
                    WearInstalled("ar_pls", true);
                    REX::INFO("arms: PLS installed t{}: {} charge(s) + {}, {:.1f} s recharge", a.tier, pls.base, pls.extra, pls.rechargeS);
                },
                [](const cw::NativeAbility&) {
                    pls.on = false; pls.nextAt = 0; pls.last = 0; pls.pool = 1; pls.lastTick = 0;
                    SetPlsDamage(0);
                    RemoveAll(PlsS("weapon"));
                    SetKeyword(PlsS("keyword"), false);
                    PlsRepick("PLS removed", 50);
                    WearInstalled("ar_pls", false);
                    REX::INFO("arms: PLS removed");
                } };
            g_handlers["perk_launch_capacity_override"] = NativeHandler{   // the Relic perk = the capacity mod (TweakDB +1 charge, +20% regen)
                [](const cw::NativeAbility& a) { pls.extra += (int)std::lround(Pn(a, "charges_add", 1)); pls.regenPct += Pn(a, "recharge_mult_pct", 20); },
                [](const cw::NativeAbility& a) { pls.extra = std::max(0, pls.extra - (int)std::lround(Pn(a, "charges_add", 1))); pls.regenPct = std::max(0.0, pls.regenPct - Pn(a, "recharge_mult_pct", 20)); } };
            g_handlers["mod_pls_capacity"] = NativeHandler{
                [](const cw::NativeAbility& a) { pls.extra += (int)std::lround(Pn(a, "charges_add", 1)); pls.regenPct += Pn(a, "recharge_mult_pct", 0); },
                [](const cw::NativeAbility& a) { pls.extra = std::max(0, pls.extra - (int)std::lround(Pn(a, "charges_add", 1))); pls.regenPct = std::max(0.0, pls.regenPct - Pn(a, "recharge_mult_pct", 0)); } };
            // One handler per round the CATALOG has (cw::PlsRounds, ARMORY 2026-11-10): keyed by the ability's own handler name,
            // so the elec/therm/chem -> electrical/thermal/chemical translation lives in the catalog's paramsT5.round, not here.
            // A round dropped from the catalog gets no handler. explosive stays the projectile's default whether or not its mod exists.
            if (g_engine) {
                std::vector<std::string> pp;
                for (const auto& r : cw::PlsRounds(g_engine->catalog(), &pp)) {
                    // Belt and braces over the catalog filter (ARMS). The filter is what makes an unshipped round
                    // unavailable, and this loop already honours it - "a round dropped from the catalog gets no handler" -
                    // so this should never fire; it is here for a round that reaches the engine another way, e.g. a
                    // hand-edited catalog. Gated on r.round, the LONG name, which cw::PlsRoundShips accepts: re-deriving a
                    // suffix from r.handler would put back the naming coupling this refactor removed.
                    if (!PlsRoundShips(r.round)) {
                        REX::WARN("arms: launcher round '{}' reached the engine but is not shipped by this build ([Arms] sPlsRoundsShipped='{}'): no handler registered", r.round, g_ini.plsRoundsShipped);
                        continue;
                    }
                    std::string round = r.round, look = r.look;
                    g_handlers[r.handler] = NativeHandler{
                        [round, look](const cw::NativeAbility&) { PlsRound(round); element["pls"] = look; RefreshInstalled(); },
                        [](const cw::NativeAbility&) { PlsRound("explosive"); element["pls"].clear(); RefreshInstalled(); } };
                }
                for (auto& l : pp) REX::WARN("arms: {}", l);
            }
        }
        // edges / knuckles: the arm's element look (the hit spell is the mod's CW_Nat perk; the damage conversion is pending)
        for (auto [prefix, arm] : { std::pair{ "mod_mantis_edge_", "mantis" }, std::pair{ "mod_gorilla_knuckles_", "gorilla" }, std::pair{ "mod_mono_cable_", "mono" } })
            for (auto [suf, el] : { std::pair{ "phys", "" }, std::pair{ "therm", "thermal" }, std::pair{ "chem", "chemical" }, std::pair{ "elec", "electrical" }, std::pair{ "animals", "" } }) {
                std::string a = arm, e = el;
                g_handlers[std::string(prefix) + suf] = NativeHandler{ [a, e](const cw::NativeAbility&) { element[a] = e; RefreshInstalled(); },
                                                                      [a](const cw::NativeAbility&) { element[a].clear(); RefreshInstalled(); } };
            }
        for (const char* h : { "mod_mantis_rotor_slow", "mod_mantis_rotor_fast", "mod_mantis_rotor_haming8" })
            g_handlers[h] = NativeHandler{ [](const cw::NativeAbility& a) { for (auto& wn : ArmWeapons("ar_mantis")) SetArmSpeed(wn, 1.0 + Pn(a, "attack_speed_pct", 0) / 100.0); },
                                           [](const cw::NativeAbility&) { for (auto& wn : ArmWeapons("ar_mantis")) SetArmSpeed(wn, 1.0); } };   // every element variant, not only the base
        REX::INFO("arms: PLS {}, installed visuals {}, ground slam keyword {}", PlsS("weapon").empty() ? "NOT IN THE ESP" : "OK",
                  g_map.arms.value("installedVisuals", json::object()).size(),
                  Kw(S(g_map.arms.value("slam", json::object()), "keyword")) ? "OK" : "NOT FOUND");
        REX::INFO("arms: {} arm weapon handler(s), {} visual(s), leap keyword {}", n, g_map.arms.value("visuals", json::object()).size(),
                  Kw(S(g_map.arms, "leapKeyword")) ? S(g_map.arms, "leapKeyword") + " OK" : std::string("NOT FOUND"));
    }
    void TryFinisher(std::uint32_t target, const std::vector<std::string>& weaponKws, bool power, bool staggered, bool sneak) { TryFinisherImpl(target, weaponKws, power, staggered, sneak); }
    void StartDecidedFinisher(const std::string& id, std::uint32_t target, double hpPct) { StartDecidedFinisherImpl(id, target, hpPct); }
    void OnAttackPressed() { OnAttackPressedImpl(); }
    void MonowireCleave(std::uint32_t target, const std::vector<std::string>& weaponKws, float damage) { MonowireCleaveImpl(target, weaponKws, damage); }
    void GorillaKnockback(std::uint32_t target, const std::vector<std::string>& weaponKws, bool power) { GorillaKnockbackImpl(target, weaponKws, power); }

    // ---- "hurt" family: when the player is hit (2026-11-08 batch 4). UNVERIFIED IN GAME: whether TESHitEvent's
    // totalDamage is already applied when it fires (the give-backs assume so).
    struct Deferred { float perSec; std::uint64_t until; };
    std::vector<Deferred> painDeferred;
    std::uint64_t painLast = 0;
    void WeapRestore()
    {
        if (!wscale.inst) return;
        wscale.inst->reloadSpeed = wscale.origReload; wscale.inst->speed = wscale.origSpeed;
        if (g_ini.debug) REX::INFO("weapon: instance restored (reload {:.3f}, speed {:.3f})", wscale.origReload, wscale.origSpeed);
        wscale = {};
    }
    void WeapTick()
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* mh = p && p->currentProcess ? p->currentProcess->middleHigh : nullptr;
        RE::TESObjectWEAP::InstanceData* inst = nullptr;
        RE::BSTSmartPointer<RE::TBO_InstanceData> holdNew;
        bool melee = false;
        if (mh)
            for (auto& e : mh->equippedItems)
                if (auto* w = e.item.object ? e.item.object->As<RE::TESObjectWEAP>() : nullptr; w && e.item.instanceData) {
                    inst = static_cast<RE::TESObjectWEAP::InstanceData*>(e.item.instanceData.get());
                    holdNew = e.item.instanceData;
                    melee = IsMeleeType(w);   // was IsMeleeWeapon(): sped up guns, not the arms
                    break;
                }
        double reloadPct = 0, meleePct = 0;
        for (auto& [id, n] : g_activeNative) { reloadPct += Pn(n, "reload_pct", 0); meleePct += Pn(n, "melee_speed_pct", 0); }
        const float mR = (float)(1.0 + reloadPct / 100.0), mS = melee ? (float)(1.0 + meleePct / 100.0) : 1.0f;
        if (inst == wscale.inst && mR == wscale.mReload && mS == wscale.mSpeed) return;
        WeapRestore();   // a different weapon, or the bonus changed
        if (!inst || (mR == 1.0f && mS == 1.0f)) return;
        wscale.inst = inst; wscale.hold = holdNew; wscale.origReload = inst->reloadSpeed; wscale.origSpeed = inst->speed; wscale.mReload = mR; wscale.mSpeed = mS;
        inst->reloadSpeed = wscale.origReload * mR; inst->speed = wscale.origSpeed * mS;
        if (g_ini.debug) REX::INFO("weapon: reload x{:.3f}, speed x{:.3f}", mR, mS);
    }
    // While a save is in flight every runtime modifier is OFF the player (PreSave stripped them) and the ledger holds
    // them in `stripped`. A tick in that window would see applied=0, re-apply the amount, and then PostSave's Restore
    // would apply it a SECOND time. The window belongs to every rmods user (Atomic Sensors' SpeedMult@temp,
    // Rangeguard's DamageResist@temp, now the tier deltas), so the tick stands down for its duration.
    std::atomic<bool> saving{ false };

    // F4SE kPreSaveGame / kPostSaveGame: the save holds none of our runtime changes
    void PreSave()
    {
        saving.store(true);
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* drAv = FindAV("DamageResist");
        (void)p; (void)drAv;
        for (auto& [k, v] : rmods.Strip()) ApplyLedger(k, -v);
        WeapRestore();
        finHeldFor = 0;   // a finisher victim must not be saved with its 100,000 Health buffer
        if (fin.buffed) if (auto* f = RE::TESForm::GetFormByID(fin.victim)) if (auto* v = f->As<RE::Actor>()) { FinUnbuff(v); finHeldFor = fin.victim; }
        // With bFinisherFloor there is nothing here to strip: the lift never outlives the Actor::DoHitMe call that applied
        // it, and between hits the victim holds an ordinary health value. The audit proves that on the save path itself,
        // which is where the old standing buffer had to be remembered (the lead, 2026-11-10 condition 1).
        if (fin.active && fin.floor) FinLiftAudit("pre-save", fin.victim);
    }
    void PostSave()
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* drAv = FindAV("DamageResist");
        (void)p; (void)drAv;
        for (auto& [k, v] : rmods.Restore()) ApplyLedger(k, v);
        saving.store(false);
        WeapTick();
        if (finHeldFor && fin.active && fin.victim == finHeldFor)
            if (auto* f = RE::TESForm::GetFormByID(finHeldFor)) if (auto* v = f->As<RE::Actor>()) if (auto* hp = FindAV("Health")) {
                static_cast<RE::ActorValueOwner*>(v)->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kTemporary, *hp, kFinBuffer);
                fin.buffed = true;
            }
        finHeldFor = 0;
    }
    void HealPlayer(float v)
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* hp = FindAV("Health");
        if (p && hp && v != 0) static_cast<RE::ActorValueOwner*>(p)->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *hp, v);   // > 0 heals damage
    }
    // ---- batch 8 (2026-11-08): Black Mamba, Metabolic Editor, Atomic Sensors, Microgenerator, BISF. UNVERIFIED IN GAME.
    // An actor's active effects: a detrimental Health value modifier typed to a resist AV is a DoT of that element.
    template <class F> void ForEachActiveEffect(RE::Actor* a, F fn)
    {
        auto* list = a ? static_cast<RE::MagicTarget*>(a)->GetActiveEffectList() : nullptr;
        if (!list) return;
        for (auto& ae : list->data) {
            if (!ae || !ae->effect || !ae->effect->effectSetting) continue;
            if (ae->flags.any(RE::ActiveEffect::Flags::kInactive) || ae->flags.any(RE::ActiveEffect::Flags::kDispelled) || ae->flags.any(RE::ActiveEffect::Flags::kWornOff)) continue;
            fn(*ae, *ae->effect->effectSetting);
        }
    }
    bool HasDetrimentalOf(RE::Actor* a, RE::ActorValueInfo* resist)
    {
        bool found = false;
        ForEachActiveEffect(a, [&](RE::ActiveEffect&, RE::EffectSetting& es) {
            if (es.data.resistVariable == resist && es.data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kDetrimental)) found = true;
        });
        return found;
    }
    void PoisonBonus(std::uint32_t targetId, float dealt)
    {
        const auto* bm = Find("ci_black_mamba");
        if (!bm || dealt <= 0) return;
        auto* tf = RE::TESForm::GetFormByID(targetId);
        auto* t = tf ? tf->As<RE::Actor>() : nullptr;
        auto* hp = FindAV("Health");
        if (!t || !hp || !HasDetrimentalOf(t, FindAV("PoisonResist"))) return;
        const double pct = Pn(*bm, "dmg_pct", 18);
        static_cast<RE::ActorValueOwner*>(t)->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *hp, -(float)(dealt * pct / 100.0));
        if (g_ini.debug) REX::INFO("natives: Black Mamba +{:.0f}% vs a poisoned target ({:.0f} more)", pct, dealt * pct / 100.0);
    }
    // Metabolic Editor: fire / poison DoT ticks on the player heal convert_pct of what they take (radiation excluded)
    void MetabolicTick(RE::Actor* p, double dt)
    {
        const auto* me = Find("im_metabolic_editor");
        if (!me || dt <= 0) return;
        auto* hpAv = FindAV("Health"); auto* poison = FindAV("PoisonResist"); auto* fire = FindAV("FireResist");
        double perSec = 0;
        ForEachActiveEffect(p, [&](RE::ActiveEffect& ae, RE::EffectSetting& es) {
            const auto arch = (int)es.data.archetype.underlying();
            if (arch != 0 && arch != 5 && arch != 34) return;   // value modifier / dual / peak
            if (es.data.primaryAV != hpAv || !es.data.flags.any(RE::EffectSetting::EffectSettingData::Flag::kDetrimental)) return;
            if (es.data.resistVariable != poison && es.data.resistVariable != fire) return;
            if (ae.duration <= 0) return;   // an instant hit, not a tick
            perSec += std::fabs(ae.magnitude);
        });
        if (perSec > 0) HealPlayer((float)(perSec * dt * Pn(*me, "convert_pct", 50) / 100.0));
    }
    // COMPUTE, track A (the lead's ruling, 2026-11-10). A perk exists only up to the tier clamp, so above it the
    // engine runs the clamp tier's perk. For an ability on the core's ComputedAbilities() list the DLL adds the
    // DIFFERENCE between the catalog's explicit paramsT<tier> and the clamp tier's, as an actor-value modifier on the
    // PLAYER -- an actor, never a form, which is the lead's condition (1). The numbers and the conditions both come
    // from Engine::TierDeltaAvNow(), which is in the core, so what cwtest asserts, what BETA QA's --validate reads and
    // what the game applies are one function. Nothing here decides a value.
    //
    // It rides the same rmods ledger as every other runtime modifier, so a save strips it and a load restores it, and
    // the keys are "<AV>@tiercompute" so they can never collide with another user of the same actor value (Atomic
    // Sensors already holds SpeedMult@temp, and the two sum, which is correct: they are different effects).
    std::set<std::string> tierComputeKeys;
    void TierDeltaTick()
    {
        if (!g_coreOk || !g_engine || saving.load()) return;   // see `saving`: a tick mid-save would double-apply
        auto want = g_engine->TierDeltaAvNow();
        // Jenkin's Tendons' second band rides the same ledger: it is the same kind of thing (a value the catalog states
        // that no perk can carry), and summing them per actor value here means one ModActorValue call, not two.
        for (auto& [av, v] : g_engine->SecondBandAvNow()) want[av] += v;
        std::set<std::string> live;
        for (auto& [av, v] : want) {
            const std::string key = av + "@tiercompute";
            live.insert(key);
            if (const double d = rmods.Set(key, v); d != 0) {
                ApplyLedger(key, d);
                if (g_ini.debug) REX::INFO("natives: tier compute {} {:+.2f} (now {:+.2f})", av, d, v);
            }
        }
        for (auto& key : tierComputeKeys)   // an ability removed, a tier dropped, or its condition no longer holds
            if (!live.count(key))
                if (const double d = rmods.Set(key, 0.0); d != 0) {
                    ApplyLedger(key, d);
                    if (g_ini.debug) REX::INFO("natives: tier compute {} cleared ({:+.2f})", key, d);
                }
        tierComputeKeys = live;
    }

    // COMPUTE: the seam {"cmd":"tiercompute"} prints what the DLL applies above the perk clamp, the ledger amount AND
    // the player's actual actor value read back, so the lead's save/load row is gradeable from Cyberware.log alone
    // (a seam cannot read console output). Every number here is READ, none is assumed.
    void TierComputeReport()
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!g_coreOk || !g_engine || !p) { REX::WARN("tiercompute: core or player not ready"); return; }
        const auto now = g_engine->TierDeltaAvNow();
        REX::INFO("tiercompute: {} resolved delta(s), {} actor value(s) applying now, saving={}", g_engine->tierDeltas().size(), now.size(), saving.load());
        for (auto& td : g_engine->tierDeltas()) {
            std::string when;
            for (auto& w : td.fx.when) when += std::format(" when {} {:.2f}", w.fn, w.value);
            REX::INFO("tiercompute: {} {} {:+.2f}{}{}", td.ability, td.fx.av, td.fx.value, when, td.timed ? " (timed: only while its own timer runs)" : "");
        }
        for (auto& [av, v] : now) {
            const std::string key = av + "@tiercompute";
            const double ledger = rmods.applied.count(key) ? rmods.applied.at(key) : 0.0;
            auto* a = FindAV(av);
            const double onPlayer = a ? static_cast<RE::ActorValueOwner*>(p)->GetActorValue(*a) : -1.0;
            REX::INFO("tiercompute: {} wants {:+.2f}, ledger holds {:+.2f}, the player's {} READS {:.2f}{}",
                      av, v, ledger, av, onPlayer, std::fabs(ledger - v) < 1e-6 ? "" : "  <-- LEDGER DISAGREES WITH WHAT IS WANTED");
        }
    }

    // Atomic Sensors: detected (someone's detection of the player is at or above "noticed") but not in combat -> +speed_pct_max SpeedMult
    bool atomicOn = false;
    void AtomicTick(RE::Actor* p)
    {
        const auto* as = Find("ns_atomic_sensors");
        bool want = false;
        if (as && !p->IsInCombat())
            if (auto* pl = RE::ProcessLists::GetSingleton()) {
                std::uint32_t los = 0;
                const auto lvl = pl->RequestHighestDetectionLevelAgainstActor(p, los);
                want = lvl >= 2;   // ESTIMATE: 0 unseen, 1 lost, 2 noticed, 3 very aware (the Skyrim scale; the debug log shows the raw value)
                if (g_ini.debug && want != atomicOn) REX::INFO("natives: Atomic Sensors detection level {} (los {}) -> {}", lvl, los, want ? "on" : "off");
            }
        atomicOn = want;
        if (const double d = rmods.Set("SpeedMult@temp", want && as ? Pn(*as, "speed_pct_max", 56) : 0.0); d != 0) ApplyLedger("SpeedMult@temp", d);
    }
    // Microgenerator: an empty reload (the equipped gun's magazine went 0 -> loaded) arms +bonus_energy_dmg for the next gun hit
    std::uint32_t mgLastAmmo = 0xFFFFFFFF; RE::TESForm* mgLastWeap = nullptr; bool mgArmed = false;
    void MicrogenTick(RE::Actor* p)
    {
        if (!Find("ha_microgenerator")) { mgArmed = false; return; }
        auto* mh = p->currentProcess ? p->currentProcess->middleHigh : nullptr;
        if (!mh) return;
        RE::TESForm* weap = nullptr; std::uint32_t ammo = 0xFFFFFFFF;
        for (auto& ei : mh->equippedItems)
            if (ei.item.object && ei.item.object->As<RE::TESObjectWEAP>() && ei.data) { weap = ei.item.object; ammo = static_cast<RE::EquippedWeaponData*>(ei.data.get())->ammoCount; break; }
        if (weap != mgLastWeap) { mgArmed = false; mgLastWeap = weap; mgLastAmmo = ammo; return; }
        if (weap && mgLastAmmo == 0 && ammo != 0xFFFFFFFF && ammo > 0) { mgArmed = true; if (g_ini.debug) REX::INFO("natives: Microgenerator armed (empty reload)"); }
        mgLastAmmo = ammo;
    }
    void MicrogenHit(std::uint32_t targetId, bool melee)
    {
        const auto* mg = Find("ha_microgenerator");
        if (!mg || !mgArmed || melee) return;
        auto* tf = RE::TESForm::GetFormByID(targetId);
        auto* t = tf ? tf->As<RE::Actor>() : nullptr;
        auto* hp = FindAV("Health");
        if (!t || !hp) return;
        mgArmed = false;
        static_cast<RE::ActorValueOwner*>(t)->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *hp, -(float)Pn(*mg, "bonus_energy_dmg", 40));
        if (g_ini.debug) REX::INFO("natives: Microgenerator +{:.0f} on the next hit", Pn(*mg, "bonus_energy_dmg", 40));
    }
    // BISF (the pacify key, engine cooldown): every hostile human in combat within 60 m stands down (Actor::StopCombat). FO4 has
    // no crime gold; settlement guards hold no grudge once combat ends, so this is the "end the fight" half of CP's faceplate.
    void Pacify()
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* pl = RE::ProcessLists::GetSingleton();
        if (!p || !pl) return;
        int n = 0;
        auto pp = p->GetPosition();
        for (auto& h : pl->highActorHandles) {
            auto a = h.get();
            if (!a || a.get() == p || a->lifeState != 0 || !a->IsInCombat() || !IsHostile(a.get(), p)) continue;
            if (!a->race || !a->race->HasKeywordString("ActorTypeNPC")) continue;
            auto ap = a->GetPosition();
            if ((ap.x - pp.x) * (ap.x - pp.x) + (ap.y - pp.y) * (ap.y - pp.y) > 4200.0 * 4200.0) continue;   // 60 m
            a->StopCombat();
            ++n;
        }
        REX::INFO("natives: BISF stood down {} human(s)", n);
        if (HudText()) RE::SendHUDMessage::ShowHUDMessage(("Faceplate: " + std::to_string(n) + " stood down").c_str(), "", false, false);
    }
    void PlayerHit(std::uint32_t attackerId, float dealt, bool ballistic)
    {
        if (dealt <= 0) return;
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p) return;
        std::uint64_t rnd = GetTickCount64() * 2654435761ull;
        auto roll = [&]() { rnd ^= rnd << 13; rnd ^= rnd >> 7; rnd ^= rnd << 17; return (double)(rnd % 10000) / 100.0; };
        auto* af = RE::TESForm::GetFormByID(attackerId);
        auto* att = af ? af->As<RE::Actor>() : nullptr;
        if (auto* n = Find("it_nano_plating"); n && ballistic && roll() < Pn(*n, "block_chance_pct", 7)) { HealPlayer(dealt); REX::INFO("hurt: Nano Plating negated {:.0f}", dealt); return; }
        if (auto* n = Find("it_carapace"); n && att) {   // hits from behind or the flanks: more than 60 deg off the player's facing
            auto pp = p->GetPosition(), ap = att->GetPosition();
            double ang = std::fabs(std::remainder(std::atan2(ap.x - pp.x, ap.y - pp.y) - p->GetHeading(), 2 * 3.14159265358979));
            if (ang > 60.0 * 3.14159265358979 / 180.0) HealPlayer(dealt * (float)(-Pn(*n, "dmg_taken_pct", -24) / 100.0));
        }
        if (auto* n = Find("it_painducer")) {   // defer_pct of the hit comes back now and is taken again over over_s
            float d = dealt * (float)(Pn(*n, "defer_pct", 30) / 100.0), s = (float)std::max(0.5, Pn(*n, "over_s", 5));
            HealPlayer(d);
            painDeferred.push_back({ d / s, GetTickCount64() + (std::uint64_t)(s * 1000) });
        }
        if (auto* n = Find("it_shock_n_awe"); n && roll() < Pn(*n, "chance_pct", 10)) {
            auto* hp = FindAV("Health");
            auto pp = p->GetPosition();
            const double r = Pn(*n, "radius_m", 5) * 70.0;
            int k = 0, st = 0;
            if (auto* pl = RE::ProcessLists::GetSingleton(); pl && hp)
                for (auto& h : pl->highActorHandles) {
                    auto a = h.get();
                    if (!a || a.get() == p || a->lifeState != 0 || !IsHostile(a.get(), p)) continue;
                    auto ap = a->GetPosition();
                    if ((ap.x - pp.x) * (ap.x - pp.x) + (ap.y - pp.y) * (ap.y - pp.y) > r * r) continue;
                    static_cast<RE::ActorValueOwner*>(a.get())->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *hp, (float)-Pn(*n, "dmg", 60));
                    // "and stagger them" (BETA QA truth list row 22): the knock LaunchLand gives the hostiles beside its target,
                    // 0.4, away from the player. Behind [Arms] bKnockback like every other knock. UNVERIFIED IN GAME: that 0.4
                    // reads as a stagger and not a knockdown on a human, and what it does to power armor and big creatures.
                    if (g_ini.knockback && a->currentProcess) { a->currentProcess->KnockExplosion(a.get(), pp, 0.4f); ++st; }
                    ++k;
                }
            REX::INFO("hurt: Shock-n-Awe burst hit {}", k);
            REX::INFO("hurt: Shock-n-Awe staggered {} of {} (bKnockback={})", st, k, g_ini.knockback ? 1 : 0);
        }
    }
    // 4 Hz: Painducer's deferred damage, Rangeguard's DR while no hostile is within radius_m
    void HurtTick()
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p) return;
        const std::uint64_t now = GetTickCount64();
        const double dt = painLast ? (double)(now - painLast) / 1000.0 : 0.25;
        painLast = now;
        for (auto it = painDeferred.begin(); it != painDeferred.end();) {
            HealPlayer(-(float)(it->perSec * dt));
            it = now >= it->until ? painDeferred.erase(it) : it + 1;
        }
        const auto* rg = Find("it_rangeguard");
        bool want = false;
        float dr = rg ? (float)Pn(*rg, "DamageResist", 40) : 0;
        if (rg) {
            want = true;
            const double r = Pn(*rg, "radius_m", 6) * 70.0;
            auto pp = p->GetPosition();
            if (auto* pl = RE::ProcessLists::GetSingleton())
                for (auto& h : pl->highActorHandles) {
                    auto a = h.get();
                    if (!a || a.get() == p || a->lifeState != 0 || !IsHostile(a.get(), p)) continue;
                    auto ap = a->GetPosition();
                    if ((ap.x - pp.x) * (ap.x - pp.x) + (ap.y - pp.y) * (ap.y - pp.y) <= r * r) { want = false; break; }
                }
        }
        if (const double delta = rmods.Set("DamageResist@temp", want ? dr : 0.0); delta != 0) {
            ApplyLedger("DamageResist@temp", delta);
            if (g_ini.debug) REX::INFO("hurt: Rangeguard {} ({:+.0f} DR)", want ? "on" : "off", delta);
        }
        MetabolicTick(p, dt);
        AtomicTick(p);
        TierDeltaTick();
        MicrogenTick(p);
        // Rara Avis: +Health_pct of the base max Health (kPermanent through the ledger, so a save never holds it)
        double hpPct = 0;
        if (auto* ra = Find("sk_rara_avis")) hpPct = Pn(*ra, "Health_pct", 0);
        if (auto* hpAv = FindAV("Health")) {
            const double base = static_cast<RE::ActorValueOwner*>(p)->GetBaseActorValue(*hpAv);
            if (const double d = rmods.Set("Health@perm", std::round(base * hpPct / 100.0)); d != 0) ApplyLedger("Health@perm", d);
        }
        // AP regen bonus (Adreno Trigger while it runs, Rara Avis always): restore pct of an assumed 4 AP/s base regen
        double apPct = 0;
        if (auto* ra = Find("sk_rara_avis")) apPct += Pn(*ra, "ap_regen_pct", 0);
        if (auto* ad = Find("ns_adreno_trigger"); ad && TimerLeft("ns_adreno_trigger") > 0) apPct += Pn(*ad, "ap_regen_pct", 0);
        if (apPct > 0)
            if (auto* apAv = FindAV("ActionPoints")) static_cast<RE::ActorValueOwner*>(p)->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *apAv, (float)(4.0 * apPct / 100.0 * dt));
        WeapTick();
    }

    // Vulnerability Analytics / Machine Learning on FO4 limbs (cw::WeakPoints). The vulnerable limb of the target the player
    // hits is named on the HUD when it moves; hits there get the crit + armor pen bonus; destroying it pops a 3 m EMP.
    void WeakPointHit(std::uint32_t targetId, int limb, float dealt)
    {
        // Tyrosine: +headshot_pct damage on head hits while its buff runs (FO4 HitData limb)
        if (auto* ty = Find("ns_tyrosine"); ty && cw::LimbGroup(limb) == 1 && dealt > 0 && TimerLeft("ns_tyrosine") > 0)
            if (auto* tf = RE::TESForm::GetFormByID(targetId))
                if (auto* t = tf->As<RE::Actor>(); t && t->lifeState == 0)
                    if (auto* hp = FindAV("Health")) static_cast<RE::ActorValueOwner*>(t)->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *hp, (float)(-dealt * Pn(*ty, "headshot_pct", 0) / 100.0));
        if (!Active("perk_vulnerability_analytics")) return;
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* tf = RE::TESForm::GetFormByID(targetId);
        auto* t = tf ? tf->As<RE::Actor>() : nullptr;
        auto* hp = FindAV("Health");
        if (!p || !t || !hp || !p->IsInCombat() || t->lifeState != 0 || !IsHostile(t, p)) return;
        double critMult = 1.5;
        if (auto* cd = FindAV("CW_CritDamage")) critMult += static_cast<RE::ActorValueOwner*>(p)->GetActorValue(*cd) / 100.0;
        const double maxHp = static_cast<RE::ActorValueOwner*>(t)->GetPermanentActorValue(*hp);
        auto h = weak.OnHit(targetId, limb, dealt, maxHp, (std::int64_t)GetTickCount64(), critMult, Active("perk_machine_learning"),
                            (std::uint64_t)GetTickCount64() * 2654435761ull);
        static const char* kName[] = { "torso", "head", "left arm", "right arm", "left leg", "right leg" };
        if (h.group >= 0 && (targetId != weakShownFor || h.group != weakShownGroup)) {
            weakShownFor = targetId; weakShownGroup = h.group;
            hud::Event({ { "type", "vulnerability" }, { "targetId", std::format("{:08X}", targetId) }, { "limb", kName[h.group] }, { "group", h.group } });
            if (HudText()) RE::SendHUDMessage::ShowHUDMessage((std::string("Vulnerability: ") + kName[h.group]).c_str(), "", false, false);
        }
        if (h.bonus > 0) static_cast<RE::ActorValueOwner*>(t)->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *hp, (float)-h.bonus);
        if (h.destroyed) {
            weakShownGroup = -2;
            auto tp = t->GetPosition();
            int n = 0;
            if (auto* pl = RE::ProcessLists::GetSingleton())
                for (auto& hnd : pl->highActorHandles) {
                    auto a = hnd.get();
                    if (!a || a.get() == p || a->lifeState != 0 || !IsHostile(a.get(), p)) continue;
                    auto ap = a->GetPosition();
                    if ((ap.x - tp.x) * (ap.x - tp.x) + (ap.y - tp.y) * (ap.y - tp.y) > 210.0 * 210.0) continue;   // CP 3 m
                    static_cast<RE::ActorValueOwner*>(a.get())->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *hp, -30.0f);
                    ++n;
                }
            hud::Event({ { "type", "vulnDestroyed" }, { "targetId", std::format("{:08X}", targetId) }, { "emp", n }, { "mlStacks", weak.stacks } });
            if (HudText()) RE::SendHUDMessage::ShowHUDMessage(("Vulnerability destroyed: EMP hit " + std::to_string(n) + (weak.stacks ? ", Machine Learning x" + std::to_string(weak.stacks) : std::string())).c_str(), "", false, false);
        }
    }
}  // namespace arms

// ---------------------------------------------------------------- optics (the lead, 2026-11-08: HUD + camera, no new view yet)
// Kiroshi zoom: holding the scan key divides the camera FOV by the optic's zoom, eased in steps x1.5 / 2.2 / 4 / 7 / 10 and clamped at
// x10 (ZoomFactor, cw::ZoomTarget; an older comment here said "capped at x3", which the code has not done since the x10 item text). Threat Detector: a HUD line
// for each hostile that enters combat with the player (direction + distance). Target Analysis: a HUD readout (HP, DR) of
// the hostile the player aims at down sights. UNVERIFIED IN GAME: whether the camera keeps a FOV written from a task.
namespace optics {
    float baseWorld = 0, base1P = 0; bool zoomed = false;
    double ZoomFactor()
    {
        double z = 1;
        for (auto& [id, n] : g_activeNative)
            if (n.handler.rfind("face_kiroshi_", 0) == 0)   // every Kiroshi zooms: x2.2 unless the optic's catalog entry names its own zoom
                z = std::max(z, n.params.is_object() && n.params.contains("zoom") && n.params["zoom"].is_number() ? n.params["zoom"].get<double>() : 2.2);
        return std::clamp(z, 1.0, 10.0);
    }
    // Zoom (hold the scan key): the FOV eases toward a level stepped by how long the key has been held, capped at the optic's
    // zoom (the item text's x10 on the best, cw::ZoomTarget). ZoomFrame runs every frame; SetZoom only moves the target.
    std::uint64_t zoomHeldAt = 0, zoomLast = 0; double zoomCur = 1.0; bool zoomWanted = false;
    void SetZoom(bool on)
    {
        auto* cam = RE::PlayerCamera::GetSingleton();
        if (!cam) return;
        if (on) {
            if (ZoomFactor() <= 1.0) return;
            if (!zoomed) { snd::Cue("CW_SND_KiroshiZoom"); baseWorld = cam->worldFOV; base1P = cam->firstPersonFOV; zoomed = true; zoomCur = 1.0; zoomLast = GetTickCount64(); if (g_ini.debug) REX::INFO("optics: zoom start, FOV {:.1f}", baseWorld); }
            zoomHeldAt = GetTickCount64();
            zoomWanted = true;
        } else zoomWanted = false;
    }
    void ZoomFrame()
    {
        if (!zoomed) return;
        auto* cam = RE::PlayerCamera::GetSingleton();
        if (!cam) return;
        const std::uint64_t now = GetTickCount64();
        const double dt = std::clamp((double)(now - zoomLast) / 1000.0, 0.0, 0.1), held = (double)(now - zoomHeldAt) / 1000.0;
        zoomLast = now;
        zoomCur = cw::ZoomEase(zoomCur, !zoomWanted ? 1.0 : cw::ZoomTarget(held, ZoomFactor()), dt);
        if (!zoomWanted && zoomCur < 1.01) { cam->worldFOV = baseWorld; cam->firstPersonFOV = base1P; zoomed = false; zoomCur = 1.0; return; }
        cam->worldFOV = (float)cw::ZoomFovDeg(baseWorld, zoomCur); cam->firstPersonFOV = (float)cw::ZoomFovDeg(base1P, zoomCur);
    }
    std::set<std::uint32_t> threatsShown;
    std::uint32_t lastReadout = 0; std::uint64_t lastReadoutAt = 0;
    constexpr double kThreatRangeM = 50;
    json threatsNow = nullptr, targetNow = nullptr;   // the HUD snapshot's "threats" / "target" (null: not installed / none)
    // Doomsayer / Oracle (batch 8): the scan key also finds mines (Projectile::IsMine), hazards (HAZD bases) and trap triggers
    // (ACTI bases with the trapTrigger / MonkeyTrap keyword) within trap_m / radius_m for duration_s: the vanilla
    // DetectLifeHostileFXS shader on each (UNVERIFIED on non-actor refs) + the HUD snapshot's "traps". Stalker: hostiles inside
    // the aim cone get the same shader while aiming (the cone replaces the all-around Detect Life perk).
    json trapsNow = nullptr;
    std::uint64_t trapsUntil = 0; double trapsRadiusM = 0;
    RE::TESEffectShader* DetectShader()
    {
        static RE::TESEffectShader* sh = nullptr;
        if (!sh) if (auto* f = RE::TESForm::GetFormByID(0x0022517E)) sh = f->As<RE::TESEffectShader>();   // DetectLifeHostileFXS
        return sh;
    }
    bool TrapOptic(double& radiusM, double& durationS)
    {
        bool any = false; radiusM = 0; durationS = 0;
        for (auto& [id, n] : g_activeNative) {
            if (n.handler == "face_kiroshi_doomsayer") { any = true; radiusM = std::max(radiusM, arms::Pn(n, "radius_m", 26)); durationS = std::max(durationS, arms::Pn(n, "duration_s", 60)); }
            if (n.handler == "face_kiroshi_oracle" && n.params.is_object() && n.params.contains("trap_m")) { any = true; radiusM = std::max(radiusM, arms::Pn(n, "trap_m", 26)); durationS = std::max(durationS, arms::Pn(n, "duration_s", 60)); }
        }
        return any;
    }
    void FindTraps(bool announce)
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* tes = RE::TES::GetSingleton();
        json out = json::array();
        if (!p || !tes) { trapsNow = out; return; }
        const auto pp = p->GetPosition();
        const double h = p->GetHeading();
        auto* sh = DetectShader();
        double nearest = 1e9; std::string nearestKind;
        tes->ForEachReferenceInRange(p, (float)(trapsRadiusM * 70.0), [&](RE::TESObjectREFR* r) {
            if (!r || r == p) return RE::BSContainer::ForEachResult::kContinue;
            const char* kind = nullptr;
            auto* base = r->GetObjectReference();
            if (auto* pr = r->As<RE::Projectile>(); pr && pr->IsMine()) kind = "mine";
            else if (base && base->GetFormType() == RE::ENUM_FORM_ID::kHAZD) kind = "hazard";
            else if (auto* acti = base ? base->As<RE::TESObjectACTI>() : nullptr; acti && (acti->HasKeywordString("trapTrigger") || acti->HasKeywordString("MonkeyTrap"))) kind = "trap";
            else if (auto* w = base ? base->As<RE::TESObjectWEAP>() : nullptr; w && w->HasKeywordString("WeaponTypeMine")) kind = "mine";
            if (!kind) return RE::BSContainer::ForEachResult::kContinue;
            auto rp = r->GetPosition();
            const double dx = rp.x - pp.x, dy = rp.y - pp.y, dist = std::sqrt(dx * dx + dy * dy);
            const double bearing = std::remainder(std::atan2(dx, dy) - h, 2 * 3.14159265358979) * 180.0 / 3.14159265358979;
            out.push_back({ { "id", std::format("{:08X}", r->GetFormID()) }, { "kind", kind }, { "bearing", std::round(bearing) }, { "dist_m", std::round(dist / 7.0) / 10.0 } });
            if (dist < nearest) { nearest = dist; nearestKind = kind; }
            if (sh && !r->IsDeleted() && !r->IsDisabled()) { auto* r3 = r->Get3D(); if (r3 && r3->parent) r->ApplyEffectShader(sh, 0.6f); }   // 3D gone/detached (cw-crash-sandy)
            return RE::BSContainer::ForEachResult::kContinue;
        });
        trapsNow = out;
        if (announce) {
            REX::INFO("optics: trap scan {} m -> {} found", trapsRadiusM, out.size());
            const std::string line = out.empty() ? std::string("Scan: no traps nearby")
                : std::format("Scan: {} trap(s) within {:.0f} m, nearest a {} {:.0f} m away", out.size(), trapsRadiusM, nearestKind, nearest / 70.0);
            if (HudText()) RE::SendHUDMessage::ShowHUDMessage(line.c_str(), "", false, false);
        }
    }
    void TrapScan()   // the scan key fired (Engine ok): start the trap window
    {
        double r = 0, d = 0;
        if (!TrapOptic(r, d)) return;
        trapsRadiusM = r; trapsUntil = GetTickCount64() + (std::uint64_t)std::llround(d * 1000.0);
        FindTraps(true);
    }
    void StalkerCone(RE::PlayerCharacter* p, RE::ProcessLists* pl)
    {
        const auto* st = arms::Find("face_kiroshi_stalker");
        if (!st || !(p->gunState == RE::GUN_STATE::kSighted || p->gunState == RE::GUN_STATE::kFireSighted)) return;
        auto* sh = DetectShader();
        if (!sh) return;
        const double range = arms::Pn(*st, "range_m", 47) * 70.0, half = arms::Pn(*st, "cone_pct", 22) / 100.0 * 90.0;   // ESTIMATE: cone_pct of a 90 deg half-angle
        const auto pp = p->GetPosition();
        const double h = p->GetHeading();
        for (auto& hd : pl->highActorHandles) {
            auto a = hd.get();
            if (!a || a.get() == p || a->lifeState != 0 || a->IsDeleted() || a->IsDisabled() || !arms::IsHostile(a.get(), p)) continue;
            auto* r3 = a->Get3D();
            if (!r3 || !r3->parent) continue;   // 3D gone or detached mid-pass: never apply a shader to it (cw-crash-sandy)
            auto ap = a->GetPosition();
            const double dx = ap.x - pp.x, dy = ap.y - pp.y;
            if (dx * dx + dy * dy > range * range) continue;
            const double bearing = std::fabs(std::remainder(std::atan2(dx, dy) - h, 2 * 3.14159265358979) * 180.0 / 3.14159265358979);
            if (bearing <= half) a->ApplyEffectShader(sh, 0.4f);
        }
    }
    bool hudView = false;                             // the CyberwareHUD view is up: no HUD-message fallback (unless [HUD] bMessages=1)
    const char* Dir(double ang)   // the actor's bearing relative to the player's facing
    {
        double d = std::remainder(ang, 2 * 3.14159265358979) * 180.0 / 3.14159265358979;
        if (std::fabs(d) <= 45) return "ahead";
        if (std::fabs(d) >= 135) return "behind";
        return d > 0 ? "right" : "left";
    }
    void Tick()   // 4 Hz
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* pl = RE::ProcessLists::GetSingleton();
        if (!p || !pl) return;
        const bool threat = arms::Active("mod_optics_threat_detector"), readout = arms::Active("mod_optics_target_analysis");
        threatsNow = threat ? json::array() : json(nullptr);
        targetNow = nullptr;
        {   // batch 8: the trap window and the Stalker cone
            double r = 0, d = 0;
            const bool trapOptic = TrapOptic(r, d);
            if (trapOptic && GetTickCount64() < trapsUntil) FindTraps(false);
            else trapsNow = trapOptic ? json::array() : json(nullptr);
            StalkerCone(p, pl);
        }
        if (!threat && !readout) return;
        const auto pp = p->GetPosition();
        const double h = p->GetHeading();
        if (threat) {
            std::set<std::uint32_t> now;
            for (auto& hd : pl->highActorHandles) {
                auto a = hd.get();
                if (!a || a.get() == p || a->lifeState != 0 || !a->IsInCombat() || !arms::IsHostile(a.get(), p)) continue;
                auto ap = a->GetPosition();
                double dx = ap.x - pp.x, dy = ap.y - pp.y, dist = std::sqrt(dx * dx + dy * dy);
                if (dist > kThreatRangeM * 70.0) continue;
                now.insert(a->GetFormID());
                const char* nm = a->GetDisplayFullName();
                const double bearing = std::remainder(std::atan2(dx, dy) - h, 2 * 3.14159265358979) * 180.0 / 3.14159265358979;
                threatsNow.push_back({ { "id", std::format("{:08X}", a->GetFormID()) }, { "name", nm && *nm ? nm : "hostile" },
                                       { "bearing", std::round(bearing) }, { "dist_m", std::round(dist / 7.0) / 10.0 } });
                if (threatsShown.count(a->GetFormID())) continue;
                hud::Event({ { "type", "threatNew" }, { "id", std::format("{:08X}", a->GetFormID()) }, { "name", nm && *nm ? nm : "hostile" }, { "bearing", std::round(bearing) }, { "dist_m", std::round(dist / 7.0) / 10.0 } });
                if (HudText()) RE::SendHUDMessage::ShowHUDMessage(std::format("Threat: {} {} ({:.0f} m)", nm && *nm ? nm : "hostile", Dir(std::atan2(dx, dy) - h), dist / 70.0).c_str(), "", false, false);
            }
            threatsShown = now;   // shown once per engagement
        }
        // Target Analysis: while aiming a gun, or with an arm weapon drawn (blades / fists / wire are never "sighted", so a
        // gunState-only check left "target" null in every melee fight, in game 2026-11-08)
        auto* armW = arms::EquippedWeapon(p);
        const bool armDrawn = p->GetWeaponMagicDrawn() && armW &&
            (arms::WeapHas(armW, arms::Kw("CW_AnimsCyberMantis")) || arms::WeapHas(armW, arms::Kw("CW_AnimsCyberGorilla")) || arms::WeapHas(armW, arms::Kw("CW_AnimsCyberMonowire")));
        if (readout && (armDrawn || p->gunState == RE::GUN_STATE::kSighted || p->gunState == RE::GUN_STATE::kFireSighted)) {
            auto t = g_env.FindHackTarget(60);   // the hostile nearest the view heading, in LOS
            auto* tf = t.ok ? RE::TESForm::GetFormByID(t.id) : nullptr;
            auto* a = tf ? tf->As<RE::Actor>() : nullptr;
            const std::uint64_t ms = GetTickCount64();
            if (a) {
                auto* o = static_cast<RE::ActorValueOwner*>(a);
                auto* hpA = FindAV("Health"); auto* drA = FindAV("DamageResist"); auto* erA = FindAV("EnergyResist");
                const char* nm = a->GetDisplayFullName();
                auto ap = a->GetPosition();
                static const char* kLimb[] = { "torso", "head", "left arm", "right arm", "left leg", "right leg" };
                int vg = -1;
                if (arms::Active("perk_vulnerability_analytics")) if (auto it = arms::weak.targets.find(t.id); it != arms::weak.targets.end()) vg = it->second.group;
                targetNow = { { "id", std::format("{:08X}", t.id) }, { "name", nm && *nm ? nm : "target" },
                              { "hp", std::round(hpA ? o->GetActorValue(*hpA) : 0.0f) }, { "hpMax", std::round(hpA ? o->GetPermanentActorValue(*hpA) : 0.0f) },
                              { "dr", std::round(drA ? o->GetActorValue(*drA) : 0.0f) }, { "er", std::round(erA ? o->GetActorValue(*erA) : 0.0f) },
                              { "dist_m", std::round(std::sqrt((ap.x - pp.x) * (ap.x - pp.x) + (ap.y - pp.y) * (ap.y - pp.y)) / 7.0) / 10.0 },
                              { "vulnLimb", vg >= 0 && vg < 6 ? json(kLimb[vg]) : json(nullptr) }, { "vulnLimbGroup", vg } };
            }
            if (a && HudText() && (t.id != lastReadout || ms - lastReadoutAt > 4000)) {
                lastReadout = t.id; lastReadoutAt = ms;
                RE::SendHUDMessage::ShowHUDMessage(std::format("{}: HP {:.0f}/{:.0f}  DR {:.0f}  ER {:.0f}", targetNow.value("name", std::string("target")),
                    targetNow.value("hp", 0.0), targetNow.value("hpMax", 0.0), targetNow.value("dr", 0.0), targetNow.value("er", 0.0)).c_str(), "", false, false);
            }
        }
    }
    void OnLoad() { if (zoomed) { zoomWanted = false; zoomCur = 1.0; zoomed = false; if (auto* cam = RE::PlayerCamera::GetSingleton()) { cam->worldFOV = baseWorld; cam->firstPersonFOV = base1P; } } threatsShown.clear(); lastReadout = 0; threatsNow = targetNow = trapsNow = nullptr; trapsUntil = 0; }
}  // namespace optics

#include "cw_fx.inl"

// ---------------------------------------------------------------- HUD feed (contract agreed with HUD stream,
// fallout/cyberware/docs/HUD-API.md): views/CyberwareHUD/index.html, never focused; window.cwHud(snapshot) at most 10 Hz
// when something changed (one more after everything empties, by construction); window.cwHudEvent(event) for one-offs.
namespace hud {
    PrismaView view = 0;
    bool ready = false, cfgSent = false;
    std::string last;
    std::uint64_t lastPush = 0;
    void Invoke(const char* fn, const json& j);

    // ---- the layout editor (MENU POLISH 2026-11-09; page side: views/CyberwareHUD/layout.js) ----
    // BeginEdit focuses the HUD view (game paused) and tells the page to show its editor; the page answers through
    // window.cwHudRequest: {"op":"saveLayout","layout":{...}} then {"op":"editDone"}. Esc / Tab are held like the menu's
    // when [Menu] bEscGuard=1.
    bool editing = false;
    void EndEdit(const char* why)
    {
        if (!editing) return;
        editing = false;
        if (g_ui && view && g_ui->IsValid(view)) g_ui->Unfocus(view);
        escguard::SetMenuOpen(false);
        REX::INFO("hud: layout editor closed ({})", why);
    }
    void BeginEdit(const char* via)
    {
        if (!g_ui || !view || !ready || !g_ui->IsValid(view)) { REX::WARN("hud: layout editor not available (HUD view not ready)"); return; }
        if (editing) return;
        if (GameplayMenuOpen()) { REX::INFO("hud: layout editor not opened: a game menu is up"); return; }
        editing = true;
        g_ui->Show(view);
        g_ui->Focus(view, true);
        if (g_ini.escGuard) escguard::SetMenuOpen(true);
        Invoke("cwHudEdit", json{ { "on", true }, { "layout", hudlayout::Get() } });
        REX::INFO("hud: layout editor opened (via {})", via);
    }
    // IS THE PLAYER ACTUALLY SEEING OUR HUD VIEW? (HINT-1, 2026-11-13). `optics::hudView` cannot answer this - it
    // is the CreateView handle, i.e. EXISTENCE, and it stays true through a Hide, a view that never became ready and
    // a view ordered out. This asks the three questions that can each make the view invisible on its own, with the
    // SAME calls ApplySettingsToGame and Invoke already use, so it cannot disagree with them:
    //   IsValid  - the view object is still live
    //   ready    - OnReady fired, so Invoke will not silently no-op (Invoke itself requires this)
    //   IsHidden - the Settings toggle has not hidden it (wantHud = s.hud && g_ini.hud)
    // It deliberately does NOT try to detect "composited but behind the menu": there is no PrismaUI call for that,
    // and guessing is what put us here. A view that is up but ordered under a menu is handled by the hint's own
    // `gameplayMenuOpen` wait, not by this.
    bool Visible()
    {
        return g_ui && view && g_ui->IsValid(view) && ready && !g_ui->IsHidden(view);
    }
    void ToggleEdit()
    {
        if (!editing) { BeginEdit("key"); return; }
        Invoke("cwHudEdit", json{ { "on", false }, { "save", true } });   // the page saves, then sends editDone
    }
    void OnRequest(const std::string& arg)
    {
        json r = json::parse(arg, nullptr, false);
        const std::string op = r.is_object() ? r.value("op", std::string()) : std::string();
        if (op == "saveLayout") {
            std::string err;
            const bool ok = hudlayout::Save(r.value("layout", json::object()), err);
            if (!ok) REX::WARN("hudlayout: not saved: {}", err);
            Invoke("cwHudSaved", json{ { "ok", ok }, { "error", ok ? json(nullptr) : json(err) } });
        } else if (op == "editDone") {
            EndEdit("page");
        } else {
            REX::WARN("hud: unknown request '{}'", arg.substr(0, 200));
        }
    }

    void OnReady(PrismaView v)
    {
        ready = true; cfgSent = false; last.clear();
        REX::INFO("hud: DOM ready");
        if (g_ui4) g_ui4->BindUIEvent(v, "cwHudRequest", [](const char* a) { OnRequest(a ? a : ""); });
        else g_ui->RegisterJSListener(v, "cwHudRequest", [](const char* a) {
            std::string s = a ? a : "";
            F4SE::GetTaskInterface()->AddTask([s]() { OnRequest(s); });
        });
        if (g_ui4)   // the HUD's own JS errors (Ultralight gaps) land in Cyberware.log like the menu's
            g_ui4->RegisterConsoleCallback(v, [](PrismaView, PRISMA_UI_API::ConsoleMessageLevel lvl, const char* msg) {
                if (lvl == PRISMA_UI_API::ConsoleMessageLevel::Error) REX::WARN("[hud js] {}", msg ? msg : "");
                else if (g_ini.debug) REX::INFO("[hud js] {}", msg ? msg : "");
            });
    }
    void Create()
    {
        if (!g_ui || !g_ini.hud || (view && g_ui->IsValid(view))) return;
        std::string file = "Data\\PrismaUI_F4\\views\\" + g_ini.hudView;
        std::replace(file.begin(), file.end(), '/', '\\');
        static bool warned = false;
        if (!fs::exists(file)) { if (!warned) REX::INFO("hud: {} not installed - HUD messages instead", file); warned = true; optics::hudView = false; return; }
        ready = false;
        view = g_ui->CreateView(g_ini.hudView.c_str(), OnReady);
        if (view) { g_ui->Show(view); if (g_view) g_ui->SetOrder(view, g_ui->GetOrder(g_view) - 1); }
        optics::hudView = view != 0;
        REX::INFO("hud: created {} for '{}'", view, g_ini.hudView);
    }
    void Invoke(const char* fn, const json& j)
    {
        if (!g_ui || !view || !ready || !g_ui->IsValid(view)) return;
        std::string js = std::string("if(window.") + fn + ")window." + fn + "(" + j.dump(-1, ' ', false, json::error_handler_t::replace) + ");";
        g_ui->Invoke(view, js.c_str());
    }
    void Event(const json& e) { Invoke("cwHudEvent", e); }
    std::string Kind(const std::string& h)
    {
        if (h.rfind("os_sandy_", 0) == 0 || h.rfind("os_berserk_", 0) == 0) return "os";
        if (h == "ci_blood_pump") return "heal";
        return "buff";
    }
    json Snapshot()
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        json s = { { "v", 1 } };
        bool menu = g_ui && g_view && g_ui->IsValid(g_view) && !g_ui->IsHidden(g_view);
        if (GameplayMenuOpen(false)) menu = true;   // the shared menu list; a message box over gameplay keeps the HUD
        s["visible"] = !menu;
        s["inCombat"] = p && p->IsInCombat();
        bool kiroshi = false, scanning = false;
        for (auto& [id, n] : g_activeNative)
            if (n.handler.rfind("face_kiroshi_", 0) == 0) { kiroshi = true; if (arms::TimerLeft(n.handler.c_str()) > 0) scanning = true; }
        s["zoom"] = kiroshi ? json{ { "on", optics::zoomed }, { "factor", optics::ZoomFactor() }, { "scan", scanning } } : json(nullptr);
        s["threats"] = optics::threatsNow;
        s["target"] = optics::targetNow;
        s["traps"] = optics::trapsNow;   // batch 8: [{id, kind mine|hazard|trap, bearing, dist_m}] while a Doomsayer / Oracle scan runs; null without one
        if (!arms::PlsS("weapon").empty() && arms::pls.on) {
            auto* w = arms::Obj(arms::PlsS("weapon"));
            const int charges = p && w ? (int)p->GetInventoryObjectCount(w) : 0, maxCharges = std::max(1, arms::pls.base + arms::pls.extra);
            const double rate = (1.0 + arms::pls.regenPct / 100.0) / std::max(0.1, arms::pls.rechargeS);   // whole pool per second, as cw::PlsPoolStep
            const double nextS = charges >= maxCharges ? 0.0 : std::max(0.0, ((double)(charges + 1) / maxCharges - arms::pls.pool) / rate);
            s["pls"] = { { "charges", charges }, { "max", maxCharges }, { "pool", std::round(arms::pls.pool * 100) / 100 },
                         { "slot", arms::PlsSlotNow(p, w) }, { "nextS", std::round(nextS * 10) / 10 } };
        } else s["pls"] = nullptr;
        if (g_engine && g_engine->Ram() >= 0) {
            json q = json::array();
            for (auto& h : g_engine->HackView()) {
                if (h.value("uploading", false)) { q.push_back({ { "name", h.value("name", std::string("Quickhack")) }, { "cost", nullptr }, { "state", "uploading" }, { "etaS", std::round(h.value("secondsLeft", 0.0) * 10) / 10 } }); continue; }
                q.push_back({ { "name", h.value("dot", false) ? std::string("Damage over time") : ("Debuff: " + h.value("av", std::string())) }, { "cost", nullptr },
                              { "state", "active" }, { "etaS", std::round(h.value("secondsLeft", 0.0) * 10) / 10 } });
            }
            if (auto sel = g_engine->HackSelected(); !sel.is_null())
                q.insert(q.begin(), json{ { "name", sel.value("name", std::string("Quickhack")) }, { "cost", sel.value("cost", 0.0) }, { "state", "selected" } });
            s["ram"] = { { "cur", std::round(g_engine->Ram() * 10) / 10 }, { "max", g_engine->RamMaxNow() }, { "queue", q } };
        } else s["ram"] = nullptr;
        json timers = json::array();
        bool osTaken = false;
        if (g_engine)
            for (auto& t : g_engine->NativeView()) {
                const std::string ab = t.value("ability", "");
                auto it = g_activeNative.find(ab);
                if (it == g_activeNative.end()) continue;
                const cw::NativeAbility& n = it->second;
                const double act = t.value("secondsActive", 0.0), rdy = t.value("secondsToReady", 0.0);
                if (act <= 0 && rdy <= 0) continue;
                std::string kind = Kind(n.handler);
                if (kind == "os") { if (osTaken) kind = "buff"; else osTaken = true; }
                std::string name = g_engine->catalog().abilities.count(ab) ? g_engine->catalog().abilities.at(ab).name : ab;
                auto num = [&](const char* k) { return n.params.is_object() && n.params.contains(k) && n.params[k].is_number() ? json(n.params[k].get<double>()) : json(nullptr); };
                timers.push_back({ { "ability", ab }, { "name", name }, { "kind", kind }, { "activeS", std::round(act * 10) / 10 }, { "activeMaxS", num("duration_s") },
                                   { "readyS", std::round(rdy * 10) / 10 }, { "readyMaxS", num("cooldown_s") } });
            }
        s["timers"] = timers;
        if (arms::Active("perk_vulnerability_analytics")) {
            const double left = arms::weak.stacks > 0 ? std::max<double>(0.0, (double)(arms::weak.stacksUntil - (std::int64_t)GetTickCount64()) / 1000.0) : 0.0;
            s["ml"] = { { "stacks", arms::weak.stacks }, { "max", 5 }, { "untilS", std::round(left * 10) / 10 } };
        } else s["ml"] = nullptr;
        return s;
    }
    // the HUD's colours (docs/HUD-API.md cfg.custom): the accent, the Kiroshi colour (null = stock), and whether pulses show
    json HudCustom()
    {
        if (!g_engine) return json::object();
        const json c = cw::CustomizeResolve(g_engine->state().customize);
        return { { "accent", c["targets"]["hud"]["color"] }, { "kiroshi", c["targets"]["kiroshi"]["color"] }, { "pulses", c["pulses"] } };
    }
    void Tick()   // ~30 Hz caller; pushes at most every 100 ms and only on change
    {
        if (!view || !ready) return;
        const std::uint64_t now = GetTickCount64();
        if (now - lastPush < 100) return;
        json s = Snapshot();
        if (!cfgSent) s["cfg"] = { { "threatRangeM", optics::kThreatRangeM }, { "custom", HudCustom() }, { "layout", hudlayout::Get() } };
        std::string body = s.dump();
        if (body == last) return;
        last = body; lastPush = now; cfgSent = true;
        s["t"] = now;
        Invoke("cwHud", s);
    }
    void OnLoad() { cfgSent = false; last.clear(); if (editing) { Invoke("cwHudEdit", json{ { "on", false }, { "save", false } }); EndEdit("game loaded"); } }
}  // namespace hud

// cwset's apply callback (Settings tab, BETA QA): make the game match the player's settings.
// [HUD] bEnabled in Cyberware.ini stays the master switch; settings.hud is the in-menu toggle.
// DEFERRED: setSettings arrives inside PrismaUI's BindUIEvent dispatch, and calling Show/Hide
// on another view from that stack re-enters PrismaUI and hard-hung the game (INTEGRATION
// cycle 2 T8: {"hud":false} froze, watchdog stopped, CPU 0). One task later is outside it.
void ApplySettingsToGame(const cw::Settings& s)
{
    const bool wantHud = s.hud && g_ini.hud;
    F4SE::GetTaskInterface()->AddTask([wantHud]() {
        if (g_ui && hud::view && g_ui->IsValid(hud::view)) {
            if (wantHud && g_ui->IsHidden(hud::view)) g_ui->Show(hud::view);
            else if (!wantHud && !g_ui->IsHidden(hud::view)) g_ui->Hide(hud::view);
        } else if (wantHud) {
            hud::Create();
        }
    });
}

// ---------------------------------------------------------------- colour / glow customization (tint.cpp, docs/CUSTOMIZE.md)
namespace arms {
    // every visual ARMO (base + element variants) -> the customize target that colours it
    void RegisterTintVisuals()
    {
        const json ev = g_map.arms.value("elementVisuals", json::object());
        int n = 0;
        auto regOne = [&](const std::string& edid, const char* t) { if (auto* f = g_map.Get(edid)) { tint::RegisterVisual(f->GetFormID(), t); ++n; } };
        auto regAll = [&](const std::string& base, const char* t) {
            regOne(base, t);
            if (ev.contains(base)) for (auto& [el, v] : ev[base].items()) if (v.is_string()) regOne(v.get<std::string>(), t);
        };
        for (auto& [kw, armo] : g_map.arms.value("visuals", json::object()).items())
            if (armo.is_string()) regAll(armo.get<std::string>(), kw.find("Monowire") != std::string::npos ? "monowire" : "mantis");
        for (auto& [h, armo] : g_map.arms.value("installedVisuals", json::object()).items())
            if (armo.is_string()) regAll(armo.get<std::string>(), h == "ar_gorilla" ? "gorilla" : "launcher");
        REX::INFO("tint: {} visual ARMO(s) registered", n);
    }
}
// 4 Hz: the player's choices changed (menu or a load) -> the tint and the HUD pick them up
void CustomizeTick()
{
    static std::string last;
    bool kiroshi = false;
    for (auto& [id, n] : g_activeNative) if (n.handler.rfind("face_kiroshi_", 0) == 0) kiroshi = true;
    tint::SetEyesAllowed(g_ini.kiroshiEyes && kiroshi);
    const json r = cw::CustomizeResolve(g_engine->state().customize);
    std::string d = r.dump();
    if (d == last) return;
    last = std::move(d);
    tint::Configure(r);
    hud::cfgSent = false;
}

// ---------------------------------------------------------------- test seam (bDebug=1 and [Debug] sCmdFile only)
// A controlled test drives the plugin from outside the game: write JSON lines to sCmdFile and the 4 Hz tick runs them on
// the game thread, then deletes the file. Every result goes to Cyberware.log as "cmd: ...". Lines:
//   {"op":...}                       a UI-API request, exactly as the view sends it (buy, install, debugGrant, getState ...)
//   {"cmd":"key","action":"os"}      a native ability key (os camo scan heal hack hack_next pacify), as the hotkey fires it
//   {"cmd":"open"} / {"cmd":"close"} the ripperdoc menu (optional "doc"); {"cmd":"open","setup":1} the read-only setup view
//   {"cmd":"console","text":"..."}   a console command (RE::Console::ExecuteCommand)
//   {"cmd":"aelist"}                 the player's active effects with spell + source form ids (log lines "cmd: ae ...")
//   {"cmd":"chromecrit","on":0|1}    AB-E1: take CW_ChromeCrit off the player / put it back in one boot (log lines "abe1: ...")
//   {"cmd":"tiercompute"}            COMPUTE: every tier delta above the perk clamp, the ledger amount and the AV READ BACK
//   {"cmd":"snd","edid":"CW_SND_MantisSwing","mode":"2d|3d"}   play one Cyberware SNDR by editor id (SOUND T1)
//   {"cmd":"voicesay","key":"9b016784"}   a voiced line, no view open (RIPPERDOC-VOICE.md bisect)
//   {"cmd":"voiceprobe","plugin":"Fallout4.esm","topic":"249E85","ref":"FE47AC35"}   any topic on a ref through the voice Say path
//   {"cmd":"js","view":"menu|hud","code":"..."}   JS in a view; console.log output lands in the log as [js] / [hud js]
//   {"cmd":"hud"}                    the last HUD snapshot pushed
//   {"cmd":"hudedit","on":1|0}       open the HUD layout editor / save and close it
//   {"cmd":"player"} / {"cmd":"near","r":1500}   player state / the high-process actors within r units
//   {"cmd":"pipcull3p","on":0|1}     [Arms] bPlsHidePipboy3P live: hide (1) or restore (0) the 3P Pip-Boy while the PLS is installed
//   {"cmd":"clinic"}                 Varga's schedule mover: hour, wanted side, actual side, position, wait reason, running package, editor location
//   {"cmd":"packages","ref":"FE47AC35"}   an actor's base AI packages in order (default Varga): conditions pass now and which item
//                                    fails, PSDT schedule window and whether it covers now, location/target fields, which one runs
//                                    ("cmd: package #i <ID> <EditorID> conditions=pass schedule=in running=yes ...", plugin/cw_pkgdiag.cpp)
//   {"cmd":"equip","edid":"CW_MantisBlades"} / {"cmd":"formid","edid":...}   a Cyberware.esp record by EditorID;
//   {"cmd":"equip","key":"@Cyberware - Armory.esp|802"} any loaded plugin's form (ARMORY gear)
//   {"cmd":"equip","edid":"CW_MantisBlades"} / {"cmd":"formid","edid":...}   a Cyberware.esp record by EditorID; or
//   {"cmd":"equip","form":"0004822B","count":1}   any loaded form by hex id ("count": add that many if none; then R to draw)
//   {"cmd":"action","edid":"ActionRightAttack"}   the engine performs an Action (Papyrus PlayIdleAction): a swing with no mouse
//   {"cmd":"node","names":["MantisBladeR:0","AnimObjectR1"]}   where those nodes are in the player's 1P / 3P 3D
//   {"cmd":"view","view":"hud","op":"state|show|hide|focus|unfocus|order","n":0}   PrismaUI view state and experiments
//   {"cmd":"biped"}                  what every slot of the player's 1P and 3P biped holds: form, ARMA, model, built 3D clone
//   {"cmd":"slot","edid":"CW_MantisBladesVisual","slot":61}   move an ARMO + its ARMAs to another biped slot IN MEMORY
//                                    (until the game restarts) and re-equip it if it is worn: finds a slot that renders in 1P
//   {"cmd":"unequip","edid":...}     unequip a Cyberware.esp record from the player
//   {"cmd":"track","a":"AnimObjectR1","b":"RArm_Hand","secs":4,"fp":1}   every frame for secs: the distance a<->b in the
//                                    1P (fp 1) or 3P 3D; logs the max and a sample every 0.25 s (our Mantis clips slide
//                                    AnimObject?1 ~23 units off the hand while folding; vanilla clips never move it)
//   {"cmd":"animlog","on":true}      log every animation graph event of the player ("anim: <tag> '<payload>'")
//   {"cmd":"animevent","name":"attackPowerStart"}   send one graph event to the player (a power attack without the hold input);
//                                    then waits for proof it played: "confirm: <name> played" / "NEVER PLAYED" (cw_confirm.cpp)
//   {"cmd":"expect","name":"x1-power","tags":"attackPowerStart","ms":1500}   confirm a raw input sent by cw-in the same way
//   {"cmd":"knockback","target":"<hex>"}            the Gorilla power-punch knockback on that actor (ragdoll path, no punch needed)
//   {"cmd":"animvar","name":"bAnimationDriven"}     read one player graph variable; "set":false writes it; "name":"driven" = the locomotion flags
//   {"cmd":"place","target":"FF001234","d":500}   put an actor d units straight ahead of the player, facing it
//   {"cmd":"finisher","id":"mantis_01","target":"FF001234","x":0,"y":40,"yawDeg":180}   play a finisher with no
//                                    eligibility check on target (hex form id; default the nearest live actor); x/y/yawDeg
//                                    override finishers.json's placement for tuning
//   {"cmd":"cam3p","yaw":90,"zoom":1.5}   (zoom is a small scale: 0.3 head, 1.5 knees up, 2.5 whole body; 110 or 200 put the camera hundreds of metres out)
//                                    side-on capture: in 3P, orbit the camera yaw deg around the player (the player keeps
//                                    facing the same way) and optionally set the zoom; re-applied every frame until {"cmd":"cam3p","off":1}
//   {"cmd":"shapes","prefix":"MantisHousing","cull":1}   cull / uncull every player shape whose name starts with prefix
namespace testcmd {
    // cam3p (QUALITY DIRECTOR: foot slide and leap height need a SIDE-ON 3P view; over the shoulder cannot judge feet).
    // ThirdPersonState free rotation is the camera orbit FO4 uses while the player stands still; holding it every frame
    // keeps it while the player attacks or moves.
    struct CamHold { bool on = false; float yaw = 0.0f, zoom = -1.0f; int frames = 0; } camHold;
    void CamFrame()
    {
        if (!camHold.on) return;
        if (!g_ini.debug) { camHold.on = false; return; }   // test seam only (the command file is bDebug-only too)
        auto* cam = RE::PlayerCamera::GetSingleton();
        if (!cam) return;
        auto sp = cam->GetState(RE::CameraState::k3rdPerson);
        auto* st = static_cast<RE::ThirdPersonState*>(sp.get());
        if (!st) return;
        if (!cam->QCameraEquals(RE::CameraState::k3rdPerson)) {
            // INTEGRATION: POV to 1P (or a menu / VATS camera) while held must not leave the orbit behind for the next 3P:
            // leaving 3P releases the hold and resets the free rotation. Before the first applied frame, wait for 3P instead.
            if (camHold.frames > 0) {
                st->freeRotation.x = 0.0f; st->freeRotationEnabled = false; camHold.on = false;
                REX::INFO("cmd: cam3p released (the camera left 3P)");
            }
            return;
        }
        st->freeRotationEnabled = true;
        st->freeRotation.x = camHold.yaw;
        if (camHold.zoom >= 0.0f) { st->targetZoomOffset = camHold.zoom; st->currentZoomOffset = camHold.zoom; }
        if (camHold.frames++ == 0) REX::INFO("cmd: cam3p holding yaw {:.1f} deg zoom {:.0f} (freeRotation was enabled)", camHold.yaw * 180.0f / 3.14159265f, camHold.zoom);
    }
    struct Track { bool on = false; std::uint64_t until = 0, nextLog = 0; std::string a, b; bool fp = true; float maxD = 0.0f; int frames = 0; } track;
    // place seam ground check: P15 L2 put a raider 800 u ahead in the fog test cell, past the floor; it fell out of the
    // world (smart diag: 844 u pitch -22 -> 1908 u pitch -68 a second later) and the lock test read as a smart lock FAIL.
    struct PlaceCheck { bool on = false; std::uint32_t id = 0; float z0 = 0; std::uint64_t at = 0; } placeCheck;
    void PlaceCheckFrame()
    {
        if (!placeCheck.on || GetTickCount64() < placeCheck.at) return;
        placeCheck.on = false;
        auto* f = RE::TESForm::GetFormByID(placeCheck.id);
        auto* t = f ? f->As<RE::Actor>() : nullptr;
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!t || !p) { REX::WARN("cmd: place {:08X} check: the actor is gone", placeCheck.id); return; }
        const auto tp = t->GetPosition(), pp = p->GetPosition();
        const double drop = placeCheck.z0 - tp.z;
        const double dist = std::sqrt((tp.x - pp.x) * (tp.x - pp.x) + (tp.y - pp.y) * (tp.y - pp.y));
        if (drop > 150 || drop < -150)
            REX::WARN("cmd: place {:08X} check FAIL: 1.5 s later at ({:.0f}, {:.0f}, {:.0f}), {:.0f} u off, z moved {:+.0f} u: no floor there, the actor "
                      "fell; any hit or lock test on it is void. Use open ground (READY-armory-p15b: coc SanctuaryExt)", placeCheck.id, tp.x, tp.y, tp.z, dist, -drop);
        else
            REX::INFO("cmd: place {:08X} check ok: 1.5 s later at ({:.0f}, {:.0f}, {:.0f}), {:.0f} u off, z moved {:+.0f} u (on the ground)", placeCheck.id, tp.x, tp.y,
                      tp.z, dist, -drop);
    }
    void TrackFrame()
    {
        PlaceCheckFrame();
        if (!track.on) return;
        auto* p = RE::PlayerCharacter::GetSingleton();
        const std::uint64_t now = GetTickCount64();
        RE::NiAVObject* root = p ? p->Get3D(track.fp) : nullptr;
        RE::NiAVObject* a = root ? root->GetObjectByName(RE::BSFixedString(track.a.c_str())) : nullptr;
        RE::NiAVObject* b = root ? root->GetObjectByName(RE::BSFixedString(track.b.c_str())) : nullptr;
        if (a && b) {
            const auto& x = a->world.translate; const auto& y = b->world.translate;
            const float d = std::sqrt((x.x - y.x) * (x.x - y.x) + (x.y - y.y) * (x.y - y.y) + (x.z - y.z) * (x.z - y.z));
            track.maxD = std::max(track.maxD, d);
            ++track.frames;
            if (now >= track.nextLog) { track.nextLog = now + 250; REX::INFO("cmd: track {} {}<->{} {:.1f} u", track.fp ? "1P" : "3P", track.a, track.b, d); }
        }
        if (now >= track.until) {
            track.on = false;
            REX::INFO("cmd: track {} {}<->{} done: max {:.1f} u over {} frame(s){}", track.fp ? "1P" : "3P", track.a, track.b, track.maxD, track.frames, (a && b) ? "" : " (a node is missing now)");
        }
    }
    void Biped()
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p) return;
        for (int fp = 1; fp >= 0; --fp) {
            const RE::BipedAnim* b = fp ? p->firstPersonBipedAnim.get() : p->biped.get();
            if (!b) { REX::INFO("cmd: biped {}: none", fp ? "1P" : "3P"); continue; }
            int n = 0;
            for (int i = 0; i < std::to_underlying(RE::BIPED_OBJECT::kTotal); ++i) {
                const auto& o = b->object[i];
                auto* form = o.parent.object;
                if (!form && !o.armorAddon) continue;
                ++n;
                const char* fe = form ? form->GetFormEditorID() : nullptr;
                const char* ae = o.armorAddon ? o.armorAddon->GetFormEditorID() : nullptr;
                const char* part = o.part ? o.part->GetModel() : nullptr;
                std::string clone = o.partClone ? ("'" + std::string(o.partClone->name.c_str()) + "'") : std::string("NONE");
                REX::INFO("cmd: biped {} [{}] slot {}: form {:08X} '{}' arma {:08X} '{}' part '{}' clone {} skinned {}", fp ? "1P" : "3P", i,
                          i < 32 ? std::to_string(i + 30) : std::string("-"), form ? form->GetFormID() : 0, fe ? fe : "",
                          o.armorAddon ? o.armorAddon->GetFormID() : 0, ae ? ae : "", part ? part : "", clone, o.skinned);
            }
            REX::INFO("cmd: biped {}: {} object(s)", fp ? "1P" : "3P", n);
        }
    }
    void Slot(const std::string& edid, int slot)
    {
        auto* f = edid.empty() ? nullptr : g_map.Get(edid);
        auto* armo = f ? f->As<RE::TESObjectARMO>() : nullptr;
        if (!armo || slot < 30 || slot > 61) { REX::WARN("cmd: slot: '{}' is not a Cyberware ARMO or slot {} is not 30-61", edid, slot); return; }
        const std::uint32_t mask = 1u << (slot - 30);
        const bool worn = arms::visualArmo == edid;
        if (worn) arms::SetVisual(edid, false);
        auto& bm = static_cast<RE::BGSBipedObjectForm*>(armo)->bipedModelData;
        const std::uint32_t old = bm.bipedObjectSlots;
        bm.bipedObjectSlots = mask;
        int n = 0;
        for (auto& ad : armo->modelArray)
            if (ad.armorAddon) { static_cast<RE::BGSBipedObjectForm*>(ad.armorAddon)->bipedModelData.bipedObjectSlots = mask; ++n; }
        if (worn) arms::SetVisual(edid, true);
        REX::INFO("cmd: slot {} 0x{:08X} -> 0x{:08X} (slot {}), {} ARMA(s), {}", edid, old, mask, slot, n, worn ? "re-equipped" : "not worn now (draw the weapon)");
    }

    void Player()
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p) return;
        auto pp = p->GetPosition();
        auto* w = arms::EquippedWeapon(p);
        auto* hp = FindAV("Health");
        auto* o = static_cast<RE::ActorValueOwner*>(p);
        const char* wn = w ? w->GetFormEditorID() : "";
        REX::INFO("cmd: player pos ({:.0f}, {:.0f}, {:.0f}) heading {:.1f} deg hp {:.0f}/{:.0f} drawn {} weapon {:08X} '{}' moveMode 0x{:X} combat {} third {} god {}",
                  pp.x, pp.y, pp.z, p->GetHeading() * 180.0 / 3.14159265358979, hp ? o->GetActorValue(*hp) : 0.0f, hp ? o->GetPermanentActorValue(*hp) : 0.0f,
                  p->GetWeaponMagicDrawn(), w ? w->GetFormID() : 0, wn ? wn : "", (std::uint32_t)p->moveMode, p->IsInCombat(),
                  RE::PlayerCamera::GetSingleton() && RE::PlayerCamera::GetSingleton()->QCameraEquals(RE::CameraState::k3rdPerson), p->IsGodMode());
        // every equipped WEAP with its slot, so a log can tell the grenade slot (00046AAC) from the hands; "weapon" above is the hands only
        if (auto* mh = p->currentProcess ? p->currentProcess->middleHigh : nullptr)
            for (auto& e : mh->equippedItems)
                if (auto* g = e.item.object ? e.item.object->As<RE::TESObjectWEAP>() : nullptr) {
                    const char* gn = g->GetFormEditorID();
                    // the ammo read a reload test needs (QD block 24): rounds in the magazine, the magazine size and the reserve.
                    // P14's "reload FAIL" ran with tgm on, which never spends a round, so R had nothing to reload
                    auto* wd = static_cast<RE::EquippedWeaponData*>(e.data.get());
                    auto* ammo = wd ? wd->ammo : nullptr;
                    const auto* inst = e.item.instanceData ? static_cast<const RE::TESObjectWEAP::InstanceData*>(e.item.instanceData.get()) : &g->weaponData;
                    REX::INFO("cmd: player equipped {:08X} '{}' slot {:08X} ammo {:08X} magazine {} of {} reserve {}", g->GetFormID(), gn ? gn : "",
                              e.equipSlot ? e.equipSlot->GetFormID() : 0, ammo ? ammo->GetFormID() : 0, wd ? (int)wd->ammoCount : -1,
                              inst ? (int)inst->ammoCapacity : -1, ammo ? p->GetInventoryObjectCount(ammo) : 0);
                }
        if (auto* pw = arms::PlsS("weapon").empty() ? nullptr : arms::Obj(arms::PlsS("weapon")))
            REX::INFO("cmd: player PLS charges {} installed {} pool {:.0f}%", p->GetInventoryObjectCount(pw), arms::pls.on, arms::pls.pool * 100);
    }
    void Near(double r)
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* pl = RE::ProcessLists::GetSingleton();
        if (!p || !pl) return;
        auto pp = p->GetPosition();
        int n = 0;
        for (auto& h : pl->highActorHandles) {
            auto a = h.get();
            if (!a || a.get() == p) continue;
            auto ap = a->GetPosition();
            const double d = std::sqrt((ap.x - pp.x) * (ap.x - pp.x) + (ap.y - pp.y) * (ap.y - pp.y));
            if (d > r) continue;
            const char* nm = a->GetDisplayFullName();
            auto* hpA = FindAV("Health");
            auto* o = static_cast<RE::ActorValueOwner*>(a.get());
            // pos + heading so a test can frame an NPC where she stands (cw-int-4: framing guessed from moveto missed Varga)
            REX::INFO("cmd: near {:08X} '{}' {:.0f} u hp {:.0f} life {} hostile {} combat {} at ({:.0f}, {:.0f}, {:.0f}) heading {:.1f}", a->GetFormID(), nm ? nm : "", d,
                      hpA ? o->GetActorValue(*hpA) : 0.0f, (int)a->lifeState, arms::IsHostile(a.get(), p), a->IsInCombat(),
                      ap.x, ap.y, ap.z, a->GetHeading() * 57.29578f);
            ++n;
        }
        REX::INFO("cmd: near {} actor(s) within {:.0f} u", n, r);
    }
    void Run(const std::string& line)
    {
        json j = json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.is_object()) { REX::WARN("cmd: not a JSON object: {}", line.substr(0, 200)); return; }
        if (j.contains("op") && !j.contains("cmd")) {   // a "cmd" line may carry its own "op" (view)
            if (!g_coreOk) { REX::WARN("cmd: core not loaded"); return; }
            std::string res = g_api->Handle(line);
            REX::INFO("cmd: api {} -> {}", line.substr(0, 400), res.substr(0, 1500));
            cwset::AfterOp(line, res);   // the seam is a UI-API path too: setSettings persists + applies (cycle-2 T4)
            if (g_ui && g_view && g_domReady) {
                g_ui->Invoke(g_view, ("if(window.cwOnResponse)window.cwOnResponse(" + res + ");").c_str());
                Push("cwOnEvent", g_api->Event("state"));   // an open menu re-renders from the new state
            }
            return;
        }
        const std::string c = j.value("cmd", std::string());
        if (c == "key") FireNativeKey(j.value("action", std::string()));
        else if (c == "open" && j.value("setup", 0) != 0) OpenSetup("seam");   // {"cmd":"open","setup":1}
        else if (c == "open") {
            std::string doc = j.value("doc", g_ini.openKeyDoc);
            if (doc.empty() && g_coreOk && !g_engine->catalog().ripperdocOrder.empty()) doc = g_engine->catalog().ripperdocOrder.front();
            OpenMenu(doc, j.value("via", std::string("hotkey")) == "talk" ? "talk" : "hotkey");
        } else if (c == "close") CloseMenu();
        else if (c == "voicesay") {   // a voiced line by key, with no view open: bisects the talk menu out of the path
            const std::string k = j.value("key", std::string());
            json out = voice::Say(j.value("doc", std::string()), k, false);
            REX::INFO("cmd: voicesay {} -> {}", k, out.dump());
        }
        else if (c == "voiceprobe") {   // {"cmd":"voiceprobe","plugin":"Fallout4.esm","topic":"249E85","ref":"FE47AC35"}: any topic through the same Say path
            const std::string t = j.value("topic", std::string()), r = j.value("ref", std::string());
            voice::ProbeSay(j.value("plugin", std::string("Fallout4.esm")), t.empty() ? 0 : std::stoul(t, nullptr, 16), r.empty() ? 0 : std::stoul(r, nullptr, 16));
        }
        else if (c == "console") { const std::string t = j.value("text", std::string()); REX::INFO("cmd: console '{}'", t); if (!t.empty()) RE::Console::ExecuteCommand(t.c_str()); }
        else if (c == "snd") {   // SOUND seam: {"cmd":"snd","edid":"CW_SND_MantisSwing","mode":"2d|3d"} - play one SNDR by editor id (T1: a MOD SNDR resolves by name)
            const std::string edid = j.value("edid", std::string()), mode = j.value("mode", std::string("2d"));
            REX::INFO("cmd: snd '{}' mode {}", edid, mode);
            if (!edid.empty()) {
                if (mode == "3d") { auto* sp = RE::PlayerCharacter::GetSingleton(); snd::PlayAt(edid.c_str(), sp ? sp->GetPosition() : RE::NiPoint3{}); }
                else snd::Play2D(edid.c_str());
            }
        }
        else if (c == "js") {
            const std::string code = j.value("code", std::string());
            const bool h = j.value("view", std::string("menu")) == "hud";
            PrismaView v = h ? hud::view : g_view;
            const bool ok = g_ui && v && g_ui->IsValid(v) && (h ? hud::ready : g_domReady);
            REX::INFO("cmd: js -> {} ({}): {}", h ? "hud" : "menu", ok ? "sent" : "view not ready", code.substr(0, 200));
            if (ok) g_ui->Invoke(v, code.c_str());
        } else if (c == "hud") REX::INFO("cmd: hud view {} ready {} last {}", hud::view, hud::ready, hud::last.substr(0, 3000));
        else if (c == "hudedit") {   // {"cmd":"hudedit","on":1} opens the HUD layout editor; "on":0 saves and closes it
            if (j.value("on", 1) != 0) hud::BeginEdit("seam"); else if (hud::editing) hud::ToggleEdit();
            REX::INFO("cmd: hudedit -> editing {}", hud::editing);
        }
        else if (c == "equip" || c == "formid") {   // a Cyberware.esp record by EditorID (formmap), ANY loaded form by "form":"<hex id>",
            // or a plugin form by "key":"@Cyberware - Armory.esp|802" (ARMORY gear). E.g. {"cmd":"equip","form":"0004822B","count":1}
            // for the vanilla 10mm; "count" adds that many first when the player has none. Equipping does not draw the weapon:
            // press R after it (cw-int-1 stability gate, 2026-11-08).
            std::string e = j.value("edid", std::string());
            RE::TESForm* f = nullptr;
            if (j.contains("key") && j["key"].is_string()) {
                e = j["key"].get<std::string>();
                f = ResolveFormKey(!e.empty() && e[0] == '@' ? e.substr(1) : e);
                if (!f) { REX::WARN("cmd: {} key '{}' is not loaded", c, e); return; }
            } else if (j.contains("form") && j["form"].is_string()) {
                e = j["form"].get<std::string>();
                std::uint32_t id = 0;
                const auto r = std::from_chars(e.data(), e.data() + e.size(), id, 16);
                f = r.ec == std::errc() ? RE::TESForm::GetFormByID(id) : nullptr;
                if (!f) { REX::WARN("cmd: {} form '{}' is not loaded", c, e); return; }
            } else {
                f = e.empty() ? nullptr : g_map.Get(e);
                if (!f) { REX::WARN("cmd: {} '{}' is not in the formmap", c, e); return; }
            }
            auto* o = f->As<RE::TESBoundObject>();
            if (c == "formid") { REX::INFO("cmd: formid {} = {:08X}", e, f->GetFormID()); return; }
            auto* p = RE::PlayerCharacter::GetSingleton();
            auto* em = RE::ActorEquipManager::GetSingleton();
            bool ok = false;
            if (p && o && j.contains("count") && p->GetInventoryObjectCount(o) == 0)
                p->AddObjectToContainer(o, RE::BSTSmartPointer<RE::ExtraDataList>{}, std::max(1, j.value("count", 1)), nullptr, RE::ITEM_REMOVE_REASON::kNone);
            const bool force = j.value("force", 0) != 0;   // equip-refusal test: the plugin's own equips pass force=true and still work after an arm swap
            if (p && em && o) { RE::BGSObjectInstance inst(o, nullptr); ok = em->EquipObject(p, inst, 0, 1, nullptr, false, force, true, true, false); }
            REX::INFO("cmd: equip {} {:08X} ({}{}, inventory {})", e, f->GetFormID(), ok ? "ok" : "EquipObject returned false", force ? " force" : "", p && o ? p->GetInventoryObjectCount(o) : 0);
        }
        else if (c == "chromecrit") {   // AB-E1: {"cmd":"chromecrit","on":0|1} - CW_ChromeCrit off / on the player inside one boot
            const bool on = j.value("on", 1) != 0;
            g_abE1Off.store(!on);
            g_abE1HitLog.store(true);
            if (g_coreOk) g_engine->Sync();   // re-runs ApplyDesired: removes the perk for the OFF arm, adds it back for ON
            auto* p = RE::PlayerCharacter::GetSingleton();
            auto* f = g_map.Get(cw::kChromeCritPerk);
            auto* perk = f ? f->As<RE::BGSPerk>() : nullptr;
            const int rank = (p && perk) ? (int)p->GetPerkRank(perk) : -1;
            const int entries = perk ? (int)perk->perkEntries.size() : -1;
            // the line that proves which arm a clip is: the rank is READ BACK from the player after the change, not assumed
            REX::INFO("abe1: arm={} t={} CW_ChromeCrit {} rank on player {} (asked {}), entries on the loaded perk {}, hit times {}",
                      on ? "ON" : "OFF", GetTickCount64(), perk ? "found" : "NOT FOUND", rank, on ? 1 : 0, entries,
                      arms::g_origDoHitMe ? "logged per hit (DoHitMe hook installed)" : "NOT AVAILABLE (DoHitMe hook not installed; set [Arms] bHitFromDoHitMe=1 or bDismember=1)");
        }
        else if (c == "tiercompute") arms::TierComputeReport();   // COMPUTE: the applied tier deltas, with the actor value read back
        else if (c == "aelist") {   // {"cmd":"aelist"}: the player's active effects with their spell + source forms (ARMORY C6)
            arms::ForEachActiveEffect(RE::PlayerCharacter::GetSingleton(), [&](RE::ActiveEffect& ae, RE::EffectSetting& es) {
                REX::INFO("cmd: ae spell {:08X} source {:08X} effect {:08X} mag {:.2f} dur {:.1f} elapsed {:.1f} id {}",
                          ae.spell ? ae.spell->GetFormID() : 0, ae.source ? ae.source->GetFormID() : 0, es.GetFormID(), ae.magnitude, ae.duration, ae.elapsedSeconds, ae.uniqueID);
            });
        }
        else if (c == "node") {   // 3D probe: is a named node / shape in the player's 1P and 3P scene graph, where, hidden?
            auto* p = RE::PlayerCharacter::GetSingleton();
            for (auto& nm : j.value("names", json::array())) {
                if (!nm.is_string()) continue;
                for (int fp = 1; fp >= 0; --fp) {
                    RE::NiAVObject* root = p ? p->Get3D(fp == 1) : nullptr;
                    RE::NiAVObject* o = root ? root->GetObjectByName(RE::BSFixedString(nm.get<std::string>().c_str())) : nullptr;
                    if (!o) { REX::INFO("cmd: node {} {}: not found{}", fp ? "1P" : "3P", nm.get<std::string>(), root ? "" : " (no 3D)"); continue; }
                    auto& w = o->world.translate;
                    // rotations (cw-int-3: is AnimObject*1 counter-rotating the wrist, or riding the hand?): the world matrix rows,
                    // and the local rotation's angle from identity (0 = the node rides its parent exactly)
                    auto& R = o->world.rotate.entry;
                    auto& L = o->local.rotate.entry;
                    const double tr = (double)L[0][0] + L[1][1] + L[2][2];
                    const double localDeg = std::acos(std::clamp((tr - 1.0) / 2.0, -1.0, 1.0)) * 180.0 / 3.14159265358979;
                    REX::INFO("cmd: node {} {}: world ({:.1f}, {:.1f}, {:.1f}) scale {:.2f} flags 0x{:X} culled {} rot [{:.2f} {:.2f} {:.2f} | {:.2f} {:.2f} {:.2f} | {:.2f} {:.2f} {:.2f}] local {:.1f} deg parent {}",
                              fp ? "1P" : "3P", nm.get<std::string>(), w.x, w.y, w.z, o->world.scale, o->GetFlags(), o->GetAppCulled(),
                              R[0][0], R[0][1], R[0][2], R[1][0], R[1][1], R[1][2], R[2][0], R[2][1], R[2][2], localDeg,
                              o->parent ? o->parent->name.c_str() : "(none)");
                }
            }
        }
        else if (c == "cam3p") {
            if (j.value("off", 0) != 0) {
                camHold.on = false;
                if (auto* cam = RE::PlayerCamera::GetSingleton()) {
                    auto sp = cam->GetState(RE::CameraState::k3rdPerson);
                    if (auto* st = static_cast<RE::ThirdPersonState*>(sp.get())) { st->freeRotation.x = 0.0f; st->freeRotationEnabled = false; }
                }
                REX::INFO("cmd: cam3p off");
            } else {
                camHold = { true, (float)(j.value("yaw", 90.0) * 3.14159265358979 / 180.0), (float)j.value("zoom", -1.0), 0 };
                auto* cam = RE::PlayerCamera::GetSingleton();
                REX::INFO("cmd: cam3p yaw {:.0f} zoom {:.0f} ({})", j.value("yaw", 90.0), j.value("zoom", -1.0),
                          cam && cam->QCameraEquals(RE::CameraState::k3rdPerson) ? "in 3P" : "NOT in 3P yet: switch to 3P, it applies then");
            }
        }
        else if (c == "shapes") {   // {"cmd":"shapes","prefix":"MantisHousing","cull":1}: cull / uncull every 1P+3P shape whose name starts with prefix
            // (anim-3 diagnosis: which mesh is the striped cyan sheet seen looking up - housing or blade). Logs each shape and its state.
            const std::string pre = j.value("prefix", std::string());
            const bool cull = j.value("cull", 1) != 0;
            auto* p = RE::PlayerCharacter::GetSingleton();
            int k = 0;
            std::function<void(RE::NiAVObject*, int)> walk = [&](RE::NiAVObject* o, int fp) {
                if (!o) return;
                if (auto* n = o->IsNode()) { for (const auto& ch : n->children) walk(ch.get(), fp); return; }
                const char* nm = o->name.c_str();
                if (pre.empty() || !nm || std::strncmp(nm, pre.c_str(), pre.size()) != 0) return;
                o->SetAppCulled(cull); ++k;
                REX::INFO("cmd: shapes {} '{}' culled {}", fp ? "1P" : "3P", nm, cull);
            };
            if (p && !pre.empty()) for (int fp = 1; fp >= 0; --fp) walk(p->Get3D(fp == 1), fp);
            REX::INFO("cmd: shapes prefix '{}' cull {} -> {} shape(s)", pre, cull, k);
        }
        else if (c == "view") {   // PrismaUI view state / experiments: {"cmd":"view","view":"hud|menu","op":"state|show|hide|focus|unfocus|order","n":0}
            const bool h = j.value("view", std::string("hud")) == "hud";
            PrismaView v = h ? hud::view : g_view;
            const std::string op = j.value("op", std::string("state"));
            if (!g_ui || !v || !g_ui->IsValid(v)) { REX::WARN("cmd: view {} not valid", h ? "hud" : "menu"); return; }
            if (op == "show") g_ui->Show(v);
            else if (op == "hide") g_ui->Hide(v);
            else if (op == "focus") g_ui->Focus(v, false);
            else if (op == "unfocus") g_ui->Unfocus(v);
            else if (op == "order") g_ui->SetOrder(v, j.value("n", 0));
            REX::INFO("cmd: view {} {} -> hidden {} focus {} order {} (menu order {}, hud order {})", h ? "hud" : "menu", op, g_ui->IsHidden(v), g_ui->HasFocus(v),
                      g_ui->GetOrder(v), g_view && g_ui->IsValid(g_view) ? g_ui->GetOrder(g_view) : -999, hud::view && g_ui->IsValid(hud::view) ? g_ui->GetOrder(hud::view) : -999);
        }
        else if (c == "equiplocks") {   // {"cmd":"equiplocks"} counts the equip locks on the player's weapons; "clear":1 removes them
            const bool clear = j.value("clear", 0) != 0;
            REX::INFO("cmd: equiplocks {}{}", arms::EquipLocks(clear), clear ? " (cleared)" : "");
        }
        else if (c == "player") Player();
        else if (c == "qaaudit") {
            if (!g_coreOk || !g_engine) REX::WARN("cmd: qaaudit refused: the core is not loaded (no engine to audit)");
            else cwqa::Start(g_engine.get(), &g_env);
        }
        else if (c == "near") Near(j.value("r", 1500.0));
        else if (c == "clinic") REX::INFO("cmd: clinic {}", clinic::StatusLine());
        // INSTALL SCENE (docs/INSTALL-SCENE.md). Drive and TUNE the scene with no menu involved:
        //   {"cmd":"scene","doc":"diamondcity","kind":"install","name":"Test"}   start it (the DLL picks the mode)
        //   {"cmd":"scene","what":"skip"|"abort"|"status"}
        //   {"cmd":"scene","what":"sitwatch","ms":8000}   trace the sit every frame with no scene (the positive control)
        //   {"cmd":"scene","what":"seat","dx":0,"dy":0,"dz":0,"drz":0}   move the seat ref and print the line CLINIC bakes
        //   {"cmd":"scene","what":"cam","shot":1,"yaw":150,"pitch":15,"zoom":1.6}   hold a shot and print its sShot line
        else if (c == "scene") {
            const std::string what = j.value("what", std::string("start"));
            const std::string cur = cw::VisitDoc(g_engine.get());   // null-safe: "" with the core not loaded
            const std::string doc = j.value("doc", cur.empty() ? std::string("diamondcity") : cur);
            if (what == "skip") { scene::Skip(); REX::INFO("cmd: scene skip -> {}", scene::StatusLine()); }
            else if (what == "abort") { scene::Abort("test seam"); REX::INFO("cmd: scene abort"); }
            else if (what == "status") REX::INFO("cmd: scene {}", scene::StatusLine());
            else if (what == "sitwatch") REX::INFO("cmd: scene sitwatch -> {}", scene::WatchSit((std::uint64_t)j.value("ms", 8000)));
            else if (what == "seat") REX::INFO("cmd: scene seat -> {}", scene::NudgeSeat(doc, (float)j.value("dx", 0.0), (float)j.value("dy", 0.0), (float)j.value("dz", 0.0), (float)j.value("drz", 0.0)));
            else if (what == "cam") REX::INFO("cmd: scene cam -> {}", scene::NudgeCam(j.value("shot", 1), (float)j.value("yaw", 150.0), (float)j.value("pitch", 15.0), (float)j.value("zoom", 1.6)));
            else {
                json a{ { "doc", doc }, { "kind", j.value("kind", std::string("install")) }, { "name", j.value("name", std::string("Test piece")) },
                        { "key", j.value("key", std::string()) }, { "text", j.value("text", std::string()) } };
                const json r = scene::Start(a);
                REX::INFO("cmd: scene start {} -> {}", a.dump(), r.dump());
            }
        }
        else if (c == "packages") {   // read-only; no Papyrus (the leash keeps VM dispatch in cw_voice.cpp)
            const std::string r = j.value("ref", std::string());
            RE::TESForm* f = nullptr;
            if (!r.empty()) { try { f = RE::TESForm::GetFormByID((std::uint32_t)std::stoul(r, nullptr, 16)); } catch (...) {} }
            else f = g_map.Get("CW_RipperdocRef_diamondcity");
            pkgdiag::Log(f ? f->As<RE::Actor>() : nullptr);
        }
        else if (c == "biped") Biped();
        else if (c == "tint") {   // {"cmd":"tint"} logs every held shape and the eye shapes; {"cmd":"tint","pulse":1} fires a pulse; {"cmd":"tint","where":"monowire"} says if its tinted part is drawn + on screen (TINT_WHERE_OK box for shot-gate -Region)
            if (j.contains("pulse")) { bool ok = tint::Pulse((float)j.value("pulse", 1.0)); if (ok) hud::Event(json{ { "type", "pulse" }, { "kind", "os" } }); REX::INFO("cmd: tint pulse -> {}", ok); }
            else if (j.contains("worn")) tint::Worn(j["worn"].is_string() ? j["worn"].get<std::string>() : std::string());   // {"cmd":"tint","worn":"launcher"}: why a target is or is not tinted (TINT_WORN_OK/FAIL)
            else if (j.contains("where")) tint::Where(j["where"].is_string() ? j["where"].get<std::string>() : std::string(), j.value("w", 2560), j.value("h", 1440));
            else tint::Dump();
        }
        else if (c == "slot") Slot(j.value("edid", std::string()), j.value("slot", 0));
        else if (c == "unequip") {
            const std::string e = j.value("edid", std::string());
            auto* f = e.empty() ? nullptr : g_map.Get(e);
            auto* o = f ? f->As<RE::TESBoundObject>() : nullptr;
            auto* p = RE::PlayerCharacter::GetSingleton();
            auto* em = RE::ActorEquipManager::GetSingleton();
            bool ok = false;
            if (p && em && o) { RE::BGSObjectInstance inst(o, nullptr); ok = em->UnequipObject(p, &inst, 1, nullptr, 0, false, true, false, true, nullptr); }
            if (e == arms::visualArmo) arms::visualArmo.clear();
            REX::INFO("cmd: unequip {} ({})", e, ok ? "ok" : "UnequipObject returned false / not found");
        }
        else if (c == "track") {
            track = Track{};
            track.on = true; track.a = j.value("a", std::string("AnimObjectR1")); track.b = j.value("b", std::string("RArm_Hand"));
            track.fp = j.value("fp", 1) != 0;
            track.until = GetTickCount64() + (std::uint64_t)(1000.0 * std::clamp(j.value("secs", 4.0), 0.1, 30.0));
            REX::INFO("cmd: track {} {}<->{} started", track.fp ? "1P" : "3P", track.a, track.b);
        }
        else if (c == "place") {   // put an actor d units straight ahead of the player, facing the player (melee / leap tests)
            std::uint32_t tid = 0;
            try { tid = (std::uint32_t)std::stoul(j.value("target", std::string("0")), nullptr, 16); } catch (...) {}
            auto* tf = tid ? RE::TESForm::GetFormByID(tid) : nullptr;
            auto* t = tf ? tf->As<RE::Actor>() : nullptr;
            auto* p = RE::PlayerCharacter::GetSingleton();
            if (!t || !p) { REX::WARN("cmd: place: '{}' is not a loaded actor", j.value("target", std::string())); return; }
            auto pp = p->GetPosition();
            auto pl = cw::FinisherPlace(pp.x, pp.y, p->GetHeading(), j.value("x", 0.0), j.value("d", 300.0), 180.0);
            t->SetPosition(RE::NiPoint3{ (float)pl.x, (float)pl.y, pp.z }, true);
            t->SetHeading((float)pl.heading);
            REX::INFO("cmd: place {:08X} at ({:.0f}, {:.0f}) = {:.0f} u ahead (ground check in 1.5 s)", tid, pl.x, pl.y, j.value("d", 300.0));
            placeCheck = { true, tid, pp.z, GetTickCount64() + 1500 };
        }
        else if (c == "finisher") {
            std::uint32_t tid = 0;
            try { tid = (std::uint32_t)std::stoul(j.value("target", std::string("0")), nullptr, 16); } catch (...) {}
            arms::ForceFinisher(j.value("id", std::string()), tid, j);
        }
        else if (c == "animlog") { arms::animLog = j.value("on", true); REX::INFO("cmd: animlog {}", arms::animLog.load() ? "on" : "off"); }
        else if (c == "animevent") {   // send one graph event to the player, e.g. {"cmd":"animevent","name":"attackPowerStart"}: a power
            // attack without depending on the hold-to-power input timing (cw-int-1 T2: lmb:900 only ever gave a light swing)
            auto* p = RE::PlayerCharacter::GetSingleton();
            const std::string nm = j.value("name", std::string());
            const bool ok = p && !nm.empty() && static_cast<RE::IAnimationGraphManagerHolder*>(p)->NotifyAnimationGraphImpl(RE::BSFixedString(nm.c_str()));
            REX::INFO("cmd: animevent '{}' -> {}", nm, ok ? "accepted" : "REFUSED by the graph");
            // "accepted" is not "played": wait for the confirming event (defaults in cw_confirm.cpp; "expect":"a|b","ms":n overrides)
            if (ok) confirm::Expect(nm, j.value("expect", std::string()), j.value("ms", 0));
            else REX::WARN("confirm: {} NEVER PLAYED (refused by the graph)", nm);
        }
        else if (c == "expect") {   // {"cmd":"expect","name":"x1-power","tags":"attackPowerStart","ms":1500}: confirm a RAW input (cw-in lmb/x1/key)
            confirm::Expect(j.value("name", std::string("input")), j.value("tags", std::string()), j.value("ms", 0));
        }
        else if (c == "fold3p") {   // {"cmd":"fold3p","mode":"draw"|"holster"}: start the 3P Mantis fold driver now, without the anim cue
            arms::FoldStart(j.value("mode", std::string("draw")) == "holster" ? 2 : 1, 0, "test seam");
        }
        else if (c == "bone") {     // write one node's local transform once: {"cmd":"bone","name":"AnimObjectR1","p":"3P","q":[x,y,z,w],"t":[x,y,z],"transpose":false}
            // then read it back with {"cmd":"node"} a frame or more later: unchanged = nothing else drives the bone; back to the old value = the graph does
            auto* p = RE::PlayerCharacter::GetSingleton();
            RE::NiAVObject* root = p ? p->Get3D(j.value("p", std::string("3P")) == "1P") : nullptr;
            const std::string nm = j.value("name", std::string("AnimObjectR1"));
            RE::NiAVObject* o = root ? root->GetObjectByName(RE::BSFixedString(nm.c_str())) : nullptr;
            if (!o) { REX::WARN("cmd: bone '{}' not found", nm); return; }
            const auto q = j.value("q", std::vector<float>{ 0, 0, 0, 1 });
            const auto tr = j.value("t", std::vector<float>{ 0, 0, 0 });
            if (q.size() != 4 || tr.size() != 3) { REX::WARN("cmd: bone wants q[4] and t[3]"); return; }
            RE::NiMatrix3 m = arms::QuatMatrix(q[0], q[1], q[2], q[3]);
            if (j.value("transpose", false)) m = m.Transpose();
            o->local.rotate = m;
            o->local.translate = RE::NiPoint3(tr[0], tr[1], tr[2]);
            if (o->parent) o->world = o->parent->world * o->local;
            REX::INFO("cmd: bone {} local set (q {:.3f} {:.3f} {:.3f} {:.3f}, t {:.1f} {:.1f} {:.1f}{})", nm, q[0], q[1], q[2], q[3], tr[0], tr[1], tr[2], j.value("transpose", false) ? ", transposed" : "");
        }
        else if (c == "action") {    // the ENGINE performs an action, as a real input or the AI would: {"cmd":"action","edid":"ActionRightAttack"}
            // (also ActionLeftAttack, ActionRightPowerAttack, ActionLeftPowerAttack, ActionBash...; or "fk":"Fallout4.esm|xxxxxx").
            // Papyrus Actor.PlayIdleAction through the script VM, so the attack goes through the graph's own state checks.
            // animevent sends a raw graph event and the graph refuses it in some states (P12 C2e: attackStart REFUSED in 3P),
            // and a mouse button flies the free camera, so a steady-camera swing needs this. Check the swing landed with
            // {"cmd":"animlog","on":true}: weaponSwing then HitFrame. "queued" only means the VM took the call.
            auto* p = RE::PlayerCharacter::GetSingleton();
            const std::string edid = j.value("edid", std::string("ActionRightAttack"));
            RE::TESForm* f = j.contains("fk") ? ResolveFormKey(j.value("fk", std::string())) : RE::TESForm::GetFormByEditorID(RE::BSFixedString(edid.c_str()));
            auto* act = f ? f->As<RE::BGSAction>() : nullptr;
            if (!p || !act) { REX::WARN("cmd: action '{}': {}", edid, !act ? "no such Action form" : "no player"); return; }
            const bool ok = voice::PlayIdleAction(p, act);   // legacy-safe dispatch (P14 crash: the commonlibf4 template, cw_voice.cpp)
            REX::INFO("cmd: action '{}' {:08X} -> {}", edid, act->GetFormID(), ok ? "queued (PlayIdleAction)" : "REFUSED by the VM");
            // TEST HARNESS: "queued" is not "played": wait for the confirming graph event (cw_confirm.cpp; "expect"/"ms" override)
            if (ok) confirm::Expect(edid, j.value("expect", std::string()), j.value("ms", 0));
            else REX::WARN("confirm: {} NEVER PLAYED (refused by the VM)", edid);
        }
        else if (c == "animvar") {   // read (and with "set", write) one player graph variable: {"cmd":"animvar","name":"bAnimationDriven"}
            // {"cmd":"animvar","name":"bAnimationDriven","set":false}; "name":"driven" logs the locomotion flags (cw-int-2 T4)
            auto* p = RE::PlayerCharacter::GetSingleton();
            const std::string nm = j.value("name", std::string());
            if (!p || nm.empty()) return;
            if (nm == "driven") { REX::INFO("cmd: animvar driven {}", arms::DrivenVars(p)); return; }
            auto* g = static_cast<RE::IAnimationGraphManagerHolder*>(p);
            const RE::BSFixedString f(nm.c_str());
            if (j.contains("set")) {
                const auto& v = j["set"];
                bool ok = false;
                if (v.is_boolean()) ok = g->SetGraphVariableBool(f, v.get<bool>());
                else if (v.is_number_integer() && nm[0] == 'i') ok = g->SetGraphVariableInt(f, v.get<int>());
                else if (v.is_number()) ok = nm[0] == 'b' ? g->SetGraphVariableBool(f, v.get<double>() != 0) : g->SetGraphVariableFloat(f, v.get<float>());
                REX::INFO("cmd: animvar {} set {} -> {}", nm, v.dump(), ok ? "ok" : "REFUSED");
            }
            bool b = false; std::int32_t i = 0; float fl = 0;
            const bool hb = g->GetGraphVariableImplBool(f, b), hi = g->GetGraphVariableImplInt(f, i), hf = g->GetGraphVariableImplFloat(f, fl);
            REX::INFO("cmd: animvar {} bool {} int {} float {}", nm, hb ? (b ? "1" : "0") : "?", hi ? std::to_string(i) : "?", hf ? std::format("{:.3f}", fl) : "?");
        }
        else if (c == "av") {   // {"cmd":"av","name":"SpeedMult"}: base / permanent / current of one player actor value
            REX::INFO("cmd: av {}", PlayerAVLine(j.value("name", std::string("SpeedMult")).c_str()));
        }
        else if (c == "controls") {   // the player's real binds (MainGameplay): ControlMap_Custom.txt is binary; q = substring of the event name
            auto* cm = RE::ControlMap::GetSingleton();
            auto* ctx = cm ? cm->controlMaps[std::to_underlying(RE::UserEvents::INPUT_CONTEXT_ID::kMainGameplay)] : nullptr;
            if (!ctx) { REX::WARN("cmd: controls: no MainGameplay context"); return; }
            const std::string q = j.value("q", std::string());
            static const char* kDev[] = { "keyboard", "mouse", "gamepad" };
            for (int d = 0; d < 3; ++d)
                for (auto& m : ctx->deviceMappings[d]) {
                    const char* n = m.eventID.c_str();
                    if (!q.empty() && (!n || !strstr(n, q.c_str()))) continue;
                    REX::INFO("cmd: controls {} '{}' key {} remappable {}", kDev[d], n ? n : "", m.inputKey, m.remappable);
                }
        }
        else if (c == "graph") {   // weapon / default subgraph ids the process holds; a re-pick shows as requested != current, then current moving
            auto* p = RE::PlayerCharacter::GetSingleton();
            auto* mh = p && p->currentProcess ? p->currentProcess->middleHigh : nullptr;
            if (!mh) { REX::WARN("cmd: graph: no process"); return; }
            auto ids = [](auto& a) { std::string s; for (std::uint32_t i = 0; i < a.size(); ++i) s += std::format("{:016X} ", a[i].identifier); return s.empty() ? std::string("(empty)") : s; };
            REX::INFO("cmd: graph weapon current {} requested {}", ids(mh->currentWeaponSubGraphID), ids(mh->requestedWeaponSubGraphID));
            REX::INFO("cmd: graph default current {} requested {}", ids(mh->currentDefaultSubGraphID), ids(mh->requestedDefaultSubGraphID));
        }
        else if (c == "knockback") {   // {"cmd":"knockback","target":"<hex>"}: the Gorilla power-punch knockback path on that actor, as
            // if a power punch with the Gorilla Arms had landed (cw-int-2 T2: the trigger is the Melee key, this proves the ragdoll)
            std::uint32_t tid = 0;
            const std::string t = j.value("target", std::string());
            std::from_chars(t.data(), t.data() + t.size(), tid, 16);
            const std::string wk = arms::CombatCfg().value("gorillaKnockback", json::object()).value("weaponKeyword", std::string("CW_AnimsCyberGorilla"));
            REX::INFO("cmd: knockback {:08X} (keyword {})", tid, wk);
            if (tid) arms::GorillaKnockback(tid, { wk }, true);
        }
        else if (c == "plsrepick") { g_ini.plsRepick = j.value("on", 1) != 0; REX::INFO("cmd: plsrepick {}", g_ini.plsRepick); }
        else if (c == "leaprepick") { g_ini.leapRepick = j.value("on", 1) != 0; REX::INFO("cmd: leaprepick {}", g_ini.leapRepick); }
        else if (c == "pipcull3p") { g_ini.plsHidePipboy3P = j.value("on", 1) != 0; REX::INFO("cmd: pipcull3p {} (the next tick culls or restores)", g_ini.plsHidePipboy3P); }   // board 258b7d88: shoot flare-only vs flare + Pip-Boy hidden in one boot
        else if (c == "av") {   // one actor value of the player by EditorID: base, current and each modifier (the cycle setup's SpeedMult assert)
            const std::string name = j.value("name", std::string());
            auto* p = RE::PlayerCharacter::GetSingleton();
            auto* info = FindAV(name);
            if (!p || !info) { REX::WARN("cmd: av '{}' not found", name); return; }
            auto* o = static_cast<RE::ActorValueOwner*>(p);
            REX::INFO("cmd: av {} base {:.3f} current {:.3f} permanent-value {:.3f} mod-permanent {:.3f} mod-temporary {:.3f} mod-damage {:.3f}", name,
                      o->GetBaseActorValue(*info), o->GetActorValue(*info), o->GetPermanentActorValue(*info),
                      o->GetModifier(RE::ACTOR_VALUE_MODIFIER::kPermanent, *info), o->GetModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary, *info),
                      o->GetModifier(RE::ACTOR_VALUE_MODIFIER::kDamage, *info));
        }
        else if (c == "kw") {   // add / remove a Cyberware.esp keyword on the player by EditorID (the leap / slam entries' actor keywords)
            arms::SetKeyword(j.value("edid", std::string()), j.value("on", 1) != 0);
            REX::INFO("cmd: kw {} {}", j.value("edid", std::string()), j.value("on", 1));
        }
        else REX::WARN("cmd: unknown '{}'", line.substr(0, 200));
    }
    // A background thread does the file I/O (never the game thread): every 250 ms it takes the file whole (rename),
    // reads it, deletes it and queues each line as a game-thread task. Off unless bDebug=1 and [Debug] sCmdFile is set,
    // which only build-cwtest.ps1 does (the test profile's ini).
    void Start()
    {
        if (!g_ini.debug || g_ini.cmdFile.empty()) return;
        std::thread([path = g_ini.cmdFile]() {
            const std::string run = path + ".run";
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                std::error_code ec;
                if (!fs::exists(path, ec)) continue;
                fs::remove(run, ec);
                fs::rename(path, run, ec);   // the writer renames a finished file into place; this takes it whole
                if (ec) continue;
                std::vector<std::string> lines;
                {
                    std::ifstream in(run);
                    for (std::string l; std::getline(in, l);) {
                        while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
                        if (!l.empty() && l[0] != '#') lines.push_back(l);
                    }
                }
                fs::remove(run, ec);
                for (auto& l : lines)
                    PostTask([l]() {
                        try { Run(l); } catch (const std::exception& e) { REX::ERROR("cmd: '{}' threw {}", l.substr(0, 200), e.what()); }
                    });
            }
        }).detach();
        REX::INFO("cmd: test seam on, watching {}", g_ini.cmdFile);
    }
}  // namespace testcmd

// The per-frame game-thread pump. F4SE NG (0.7+, task interface v2) has AddTaskPermanent. The rig's OG F4SE 0.6.23 has
// task interface v1 (AddTask / AddUITask only): commonlibf4's AddTaskPermanent reads a function pointer past the end of
// that struct and jumped to 0x1 at data-ready (first load, crash-2026-11-08-16-49-49.log). On v1 a background thread
// keeps exactly ONE task queued: it runs on the game thread when F4SE drains its queue (once a frame) and the next one
// is only posted after it ran, so the tick runs at most once per frame and never piles up.
const char* g_pumpKind = "none";

// Set from kPreLoadGame until kPostLoadGame. The cosave callback and PostLoad run on the load thread and rewrite the
// engine state and the player's 3D while tasks may still drain, so the pump stands down; the plugin has no locks. A load
// that never reports back cannot stop the pump for good: the flag expires after kLoadGuardMs.
std::atomic<std::uint64_t> g_loadStartedMs{ 0 }, g_saveStartedMs{ 0 };
constexpr std::uint64_t kLoadGuardMs = 300000, kSaveGuardMs = 30000;
bool LoadInProgress(std::uint64_t now)
{
    const std::uint64_t l = g_loadStartedMs.load(std::memory_order_relaxed), v = g_saveStartedMs.load(std::memory_order_relaxed);
    return (l && now - l < kLoadGuardMs) || (v && now - v < kSaveGuardMs);
}
void StartFramePump(std::function<void()> tick)
{
    auto* ti = F4SE::GetTaskInterface();
    if (!ti) { REX::ERROR("native: no F4SE task interface - no per-frame tick"); return; }
    // Only the NG loader (it never calls F4SEPlugin_Query) is trusted with AddTaskPermanent; the OG loader always relays.
    if (!g_queryRuntime && ti->Version() >= 2) { ti->AddTaskPermanent(std::move(tick)); g_pumpKind = "AddTaskPermanent (NG task interface v2)"; return; }
    static std::atomic<bool> pending{ false };
    auto shared = std::make_shared<std::function<void()>>(std::move(tick));
    std::thread([shared]() {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
            if (pending.exchange(true)) continue;
            F4SE::GetTaskInterface()->AddTask([shared]() {
                try { (*shared)(); } catch (const std::exception& e) { REX::ERROR("native: frame tick threw {}", e.what()); }
                pending = false;
            });
        }
    }).detach();
    g_pumpKind = g_queryRuntime ? "AddTask relay thread (OG loader)" : "AddTask relay thread (task interface v1)";
}

// ARMORY native consumables (4 Hz): each use leaves a hidden marker ActiveEffect whose source is the item; act once per
// uniqueID (the game's own effect timer carries the duration, so a reload mid-effect does not re-fire: the id was seen).
void ArmoryPollConsumables()
{
    if (g_armoryNative.empty() || !g_coreOk || !g_engine) return;
    auto* p = RE::PlayerCharacter::GetSingleton();
    if (!p) return;
    static std::set<std::uint32_t> seen;
    std::set<std::uint32_t> now;
    arms::ForEachActiveEffect(p, [&](RE::ActiveEffect& ae, RE::EffectSetting&) {
        for (auto& n : g_armoryNative) {
            // the consumed ALCH is the effect's `spell`; `source` is only set for weapon/enchantment sources (C6, build E3: never matched)
            if (static_cast<RE::TESForm*>(ae.spell) != n.form && static_cast<RE::TESForm*>(ae.source) != n.form) continue;
            std::uint32_t key = ((std::uint32_t)ae.uniqueID << 16) ^ (n.form->GetFormID() & 0xFFFF);
            now.insert(key);
            if (seen.count(key)) continue;
            bool ok = g_engine->NativeConsume(n.native, ae.duration);
            REX::INFO("armory: {} used -> {} ({}s): {}", n.edid, n.native, ae.duration, ok ? "applied" : "no cyberdeck");
        }
    });
    seen = std::move(now);
}

void OnF4SEMessage(F4SE::MessagingInterface::Message* m)
{
    switch (m->type) {
    case F4SE::MessagingInterface::kGameDataReady: {
        static bool once = false;
        if (once) break;
        once = true;
        LoadCore();
        LoadFormMap();
        talkgate::Init();   // VARGA TALK: reads the talk's form ids out of the formmap, or stays disarmed if it is not built
        voice::Load(g_ini.voicesPath, g_ini.voice, g_queryRuntime.has_value(), g_ini.voiceProbe);   // OG loader = OG runtime
        voice::SetFinder([](const std::string& doc) -> RE::Actor* {   // the doctor's loaded actor, for lines said from the chair
            auto* dh = RE::TESDataHandler::GetSingleton();
            auto* pl = RE::ProcessLists::GetSingleton();
            if (!dh || !pl) return nullptr;
            std::uint32_t base = 0;
            for (auto& [local, d] : g_map.npcToDoc) if (d == doc) base = dh->LookupFormID(local, g_map.plugin);
            if (!base) return nullptr;
            for (auto& h : pl->highActorHandles)
                if (auto a = h.get(); a && a->GetObjectReference() && a->GetObjectReference()->GetFormID() == base) return a.get();
            return nullptr;
        });
        // INSTALL SCENE: everything cw_scene.cpp needs from main.cpp, so it stays its own translation unit.
        scene::SetHooks(scene::Hooks{
            [](const json& ev) { Push("cwOnEvent", ev); },
            [](const char* edid, const char* vanilla) { snd::Cue(edid, vanilla); },
            [](const std::string& doc, const std::string& key) -> float {
                // The chair scene unpauses the world for its length, so paused is false there. The 2D fallback does
                // NOT unpause, so the real state has to be read rather than assumed: asking a paused game to speak
                // gets a silent line and a subtitle that looks like a voice failure. Same expression as the say op.
                const bool paused = g_menuPaused && g_ui && g_view && !g_ui->IsHidden(g_view);
                const json r = voice::Say(doc, key, paused);
                REX::INFO("scene: say {} '{}' -> {}", doc, key, r.dump());
                return (r.is_object() && r.value("ok", false) && r.contains("ms") && r["ms"].is_number())
                           ? r["ms"].get<float>() : 0.0f;
            },
            []() { if (voice::Enabled()) voice::Hush(); },
            [](const std::string& doc) -> RE::Actor* {
                auto* dh = RE::TESDataHandler::GetSingleton();
                auto* pl = RE::ProcessLists::GetSingleton();
                if (!dh || !pl) return nullptr;
                std::uint32_t base = 0;
                for (auto& [local, d] : g_map.npcToDoc) if (d == doc) base = dh->LookupFormID(local, g_map.plugin);
                if (!base) return nullptr;
                for (auto& h : pl->highActorHandles)
                    if (auto a = h.get(); a && a->GetObjectReference() && a->GetObjectReference()->GetFormID() == base) return a.get();
                return nullptr;
            },
            []() { talkface::Gesture(); },
            [](std::uint32_t refId) { talkface::Begin(refId); },   // the same 0.45 s turn the talk mode uses
            [](bool paused) { if (g_ui && g_view) { g_ui->Focus(g_view, paused); g_menuPaused = paused; } },
            []() { return g_menuPaused; },
            []() { return OpenGameplayMenu(false); },   // no message box: it sits over gameplay and takes no scene down
            []() -> float {
                auto* p = RE::PlayerCharacter::GetSingleton();
                auto* hp = FindAV("Health");
                return (p && hp) ? static_cast<RE::ActorValueOwner*>(p)->GetActorValue(*hp) : 0.0f;
            },
            [](float radius) -> bool {
                auto* p = RE::PlayerCharacter::GetSingleton();
                auto* pl = RE::ProcessLists::GetSingleton();
                if (!p || !pl) return false;
                const auto pp = p->GetPosition();
                for (auto& h : pl->highActorHandles) {
                    auto a = h.get();
                    if (!a || a.get() == p || a->lifeState != 0) continue;
                    if (!a->IsInCombat() || !arms::IsHostile(a.get(), p)) continue;
                    const auto ap = a->GetPosition();
                    if (std::hypot(ap.x - pp.x, ap.y - pp.y) <= radius) return true;
                }
                return false;
            },
            [](bool busy) { talkface::SetSceneBusy(busy); },   // CRASH's talk guard: exempt while a scene runs
            [](std::uint32_t docRef, std::uint32_t running, bool& hers) -> std::uint32_t {
                return clinic::DueInClinic(docRef, running, hers);   // 0 for anyone but Varga in her clinic
            },
        });
        IndexActorValues();
        ResolveForms();
        arms::RegisterHandlers();
        arms::RegisterTintVisuals();
        arms::InstallHook();
        {
            smartgame::Config sc;
            sc.enabled = g_ini.smartLock;
            sc.steerMode = g_ini.smartSteer;
            sc.diag = g_ini.smartDiag;
            sc.formmapPath = g_ini.armoryFormmapPath;
            sc.smartLinkInstalled = []() {
                if (!g_coreOk || !g_engine) return false;
                for (auto& [uid, inst] : g_engine->state().instances) if (inst.item == "cw_ha_smart_link" && !inst.slot.empty()) return true;
                return false;
            };
            if (g_ini.stageBAnims) sc.stageBAnim = [](std::uint32_t id) {
                auto* kf = g_map.Get("CW_AR_AnimYukimura");
                auto* kw = kf ? kf->As<RE::BGSKeyword>() : nullptr;
                auto* wf = RE::TESForm::GetFormByID(id);
                auto* w = wf ? wf->As<RE::TESObjectWEAP>() : nullptr;
                if (!kw || !w) { REX::WARN("armory: stage B keyword not applied to {:08X} (keyword {}, weapon {})", id, kw != nullptr, w != nullptr); return; }
                static_cast<RE::BGSKeywordForm*>(w)->AddKeyword(kw);
                REX::INFO("armory: stage B anims on {:08X}", id);
            };
            sc.hudEvent = [](const json& e) { hud::Event(e); };
            sc.playSound = [](const char* e) { if (auto* sp = RE::PlayerCharacter::GetSingleton()) { if (auto h = snd::PlayFollow(e, sp->Get3D()); !h.IsValid()) snd::PlayAt(e, sp->GetPosition()); } };   // follows the player: a line runs up to 8 s, a fixed spot fades out behind someone running
            sc.notify = [](const std::string& s) {
                if (optics::hudView && !g_ini.hudMessages) hud::Event({ { "type", "note" }, { "text", s } });
                else RE::SendHUDMessage::ShowHUDMessage(s.c_str(), "", false, false);
            };
            smartgame::Init(std::move(sc));
        }
        LoadArmoryGear();
        arms::InstallDismemberHook();
        g_ui4 = static_cast<PRISMA_UI_API::IVPrismaUI4*>(PRISMA_UI_API::RequestPluginAPI(PRISMA_UI_API::InterfaceVersion::V4));
        g_ui = g_ui4 ? g_ui4 : static_cast<PRISMA_UI_API::IVPrismaUI1*>(PRISMA_UI_API::RequestPluginAPI(PRISMA_UI_API::InterfaceVersion::V1));
        REX::INFO("prismaui: {}", g_ui4 ? "V4 interface" : (g_ui ? "V1 interface (no BindUIEvent)" : "NOT FOUND - the ripperdoc menu cannot open"));
        if (auto* src = RE::TESActivateEvent::GetEventSource()) { src->RegisterSink(&g_activateSink); REX::INFO("events: TESActivateEvent sink registered"); }
        if (auto* src = RE::TESHitEvent::GetEventSource()) { src->RegisterSink(&g_hitSink); REX::INFO("events: TESHitEvent sink registered"); }
        if (auto* src = RE::TESDeathEvent::GetEventSource()) { src->RegisterSink(&g_deathSink); REX::INFO("events: TESDeathEvent sink registered"); }
        if (g_ini.escGuard) escguard::Register();
        REX::INFO("ini: [Menu] bEscGuard={}", g_ini.escGuard ? 1 : 0);
        StartFramePump([]() {   // every frame: the lunge; ~30 Hz leap check; 4 Hz timed effects + visual reconcile
            // MenuControls::handlers is iterated by the input loop. kGameDataReady runs on the data-load thread, so a
            // push_back there can reallocate the array under PerformInputProcessing (crash-2026-11-08-20-47-09:
            // BSInputEventUser::HandleInputEvent reading a garbage handler, keys arriving at dataReady). The first
            // pump task runs outside the input loop.
            static bool inputRegistered = false;
            if (!inputRegistered) {
                if (auto* mc = RE::MenuControls::GetSingleton()) {   // always: the legs need the Jump button even with no keys set
                    inputRegistered = true;
                    mc->handlers.push_back(&g_hotkeySink);
                    REX::INFO("input: debug hotkey VK 0x{:X} registered; native keys OS 0x{:X} camo 0x{:X} scan 0x{:X} heal 0x{:X} hack 0x{:X} next 0x{:X}; Jump watched for the legs",
                              g_ini.openKey, g_ini.keyOS, g_ini.keyCamo, g_ini.keyScan, g_ini.keyHeal, g_ini.keyHack, g_ini.keyHackNext);
                }
            }
            static std::uint64_t next = 0, nextArms = 0;
            std::uint64_t now = GetTickCount64();
            if (!g_coreOk || !RE::PlayerCharacter::GetSingleton()) return;
            if (LoadInProgress(now)) return;   // the load or save owns the engine state and the player's 3D until it reports back
            StateLock lock(0, "pump");
            if (g_ini.stateLock && !lock.held()) return;   // never block a frame: a load or save has it, try next frame
            arms::Frame();
            tint::Frame();
            legs::Frame();
            testcmd::TrackFrame();
            smartgame::Frame();
            cwqa::Tick();
            testcmd::CamFrame();
            scene::Tick(now);   // INSTALL SCENE: the beats, the held camera shot and the watchdog (per frame on purpose)
            voice::Tick();
            talkgate::Tick();   // VARGA TALK: opens the ripperdoc view on the dialogue choice, never on the activation
            hud::Tick();
            sandy::Tick();
            fx::SweepFrame();
            optics::ZoomFrame();
            bool slow = now >= next;
            if (now >= nextArms || slow) { nextArms = now + 33; arms::Tick(slow); talkface::Tick(); talkface::Guard(); }
            {
                static std::uint64_t nextTint = 0;   // ~15 Hz: a drawn blade is recoloured within a frame or two of appearing
                if (now >= nextTint) { nextTint = now + 66; tint::Scan(); }
            }
            if (!slow) return;
            next = now + 250;
            CustomizeTick();
            g_engine->NativeTick();
            ArmoryPollConsumables();
            clinic::Tick(now, !g_engine->currentRipperdoc.empty());   // CLINIC: Varga's schedule mover + package nudge (internally ~0.2 Hz)
            confirm::Tick(now);  // TEST HARNESS: a seam action with no confirming event in time logs NEVER PLAYED
            optics::Tick();
            fx::Highlights();
            chrome::Tick();
        });
        REX::INFO("native: tick task registered (4 Hz; per-frame pump: {})", g_pumpKind);
        testcmd::Start();
        SelfTestLog("dataReady");
        break;
    }
    case F4SE::MessagingInterface::kPreLoadGame:
        g_loadStartedMs.store(GetTickCount64(), std::memory_order_relaxed);
        // the world is about to change: a stale quickhack effect must not touch whoever holds that id next (ABILITIES)
        if (g_coreOk) { StateLock lock(2000, "pre-load"); g_engine->HacksOnLoad(); }
        break;
    case F4SE::MessagingInterface::kPreSaveGame:
        g_saveStartedMs.store(GetTickCount64(), std::memory_order_relaxed);
        if (g_coreOk) { StateLock lock(2000, "pre-save"); chrome::Suspend(); arms::PreSave(); g_engine->HacksPreSave(); }
        break;
    case F4SE::MessagingInterface::kPostSaveGame:
        if (g_coreOk) { StateLock lock(2000, "post-save"); g_engine->HacksPostSave(); chrome::Resume(); arms::PostSave(); }
        g_saveStartedMs.store(0, std::memory_order_relaxed);
        break;
    case F4SE::MessagingInterface::kPostLoadGame:
    case F4SE::MessagingInterface::kNewGame: {
        StateLock lock(2000, "post-load");
        if (g_coreOk) {
            g_engine->currentRipperdoc.clear();
            if (g_ui && g_view && !g_ui->IsHidden(g_view)) { g_ui->Unfocus(g_view); g_ui->Hide(g_view); Push("cwOnEvent", json{ { "type", "close" } }); }
            escguard::SetMenuOpen(false);
            scene::OnLoad();          // INSTALL SCENE: nothing of a scene is in the save, so a load only lets go of one
            g_activeNative.clear();   // re-run every handler's enable for this save (arm weapons in the inventory)
            g_engine->HacksOnLoad();
            arms::OnLoad();
            optics::OnLoad();
            sandy::OnLoad();
            fx::OnLoad();
            chrome::OnLoad();
            hud::OnLoad();
            tint::OnLoad();
            smartgame::OnLoad();
            g_engine->Sync();
            if (m->type == F4SE::MessagingInterface::kPostLoadGame && g_pendingHacks.is_object()) {   // D2: what the save was running
                json eng = g_pendingHacks.value("engine", json::object());
                int dropped = 0;
                auto alive = [](std::uint32_t id) { auto* f = id ? RE::TESForm::GetFormByID(id) : nullptr; auto* a = f ? f->As<RE::Actor>() : nullptr; return a && a->lifeState == 0; };
                if (eng.contains("fx")) {
                    json keep = json::array();
                    for (auto& f : eng["fx"]) { if (alive(f.value("target", 0u))) keep.push_back(f); else ++dropped; }
                    eng["fx"] = keep;
                }
                if (eng.contains("upload") && !alive(eng["upload"].value("target", 0u))) { eng.erase("upload"); ++dropped; }
                const int ne = g_engine->HacksRestore(eng);
                const int nc = chrome::Restore(g_pendingHacks.value("chrome", json::object()));
                REX::INFO("load: hacks restored from the cosave: {} quickhack effect(s)/upload, {} machine hack(s); {} quickhack(s) dropped (target gone or dead)", ne, nc, dropped);
            }
            g_pendingHacks = nullptr;
            arms::pls.loading = false;   // only the enable right after the load keeps the saved PLS charges
            arms::loadingSave = false;   // later enables are real installs (equip + hint)
            {
                const std::string sm = PlayerAVLine("SpeedMult");
                REX::INFO("load: {}", sm);
                auto* pl = RE::PlayerCharacter::GetSingleton();
                auto* smi = FindAV("SpeedMult");
                if (pl && smi && static_cast<RE::ActorValueOwner*>(pl)->GetActorValue(*smi) < 30.0f)
                    REX::WARN("load: the player's SpeedMult is below 30 ({}): movement will crawl (not set by Cyberware's stat perks, which move it by at most +63/-15)", sm);
            }
            SelfTestLog(m->type == F4SE::MessagingInterface::kNewGame ? "newGame" : "postLoad");
            Push("cwOnEvent", g_api->Event("state"));
        }
        CreateView();
        hud::Create();
        if (m->type == F4SE::MessagingInterface::kNewGame) cwset::LoadDefaults();
        else cwset::Apply();   // the cosave's settings decide the HUD view's visibility
        g_loadStartedMs.store(0, std::memory_order_relaxed);   // the pump resumes only after everything above has run
        break;
    }
    default:
        break;
    }
}

}  // namespace


// The menu's uiSound op (cw_settings_plugin.cpp), one per request. A request without a fallback is a plain name and always
// plays; with one, bCues decides between our SNDR and the vanilla name. PostTask: PlayMenuSound runs on the game thread here.
namespace cwsnd {
    void UiCue(const std::string& sound, const std::string& fallback)
    {
        PostTask([sound, fallback]() {
            if (fallback.empty()) RE::UIUtils::PlayMenuSound(sound.c_str());
            else snd::Cue(sound.c_str(), fallback.c_str());
        });
    }
}  // namespace cwsnd

// The rig runs the OG game (1.10.163) with F4SE 0.6.23, whose loader only accepts a DLL that exports F4SEPlugin_Query;
// commonlibf4's plugin template writes F4SEPlugin_Version only (the NG loader's export). Without this the game logs
// "does not appear to be an F4SE plugin" and never loads us (first in-game test, 2026-11-08). The NG loader ignores
// Query and keeps reading F4SEPlugin_Version. build-rig.ps1 and build-cwtest.ps1 refuse a DLL missing either export.
F4SE_PLUGIN_QUERY(const F4SE::QueryInterface* a_f4se, F4SE::PluginInfo* a_info)
{
    a_info->infoVersion = F4SE::PluginInfo::kVersion;
    a_info->name = "Cyberware";
    a_info->version = 1;
    if (!a_f4se || a_f4se->IsEditor()) return false;
    g_queryRuntime = a_f4se->RuntimeVersion();
    return *g_queryRuntime == REL::Version(1, 10, 163, 0);   // the only OG runtime this is built and tested for
}

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_intfc)
{
    F4SE::Init(a_intfc, { .trampoline = true, .trampolineSize = 64 });   // DoHitMe call-site hook: one 14-byte JMP14 stub
    REX::INFO("CYBERWARE_BUILDID {} - Cyberware {} (apiVersion {})", kBuildId, kVersion, cw::kApiVersion);
    if (g_queryRuntime) REX::INFO("query: OG loader, runtime {} accepted", g_queryRuntime->string());
    else REX::INFO("query: not called (NG loader reads F4SEPlugin_Version)");
    ReadIni();
    auto* ser = F4SE::GetSerializationInterface();
    ser->SetUniqueID(kUid);
    ser->SetSaveCallback(OnSave);
    ser->SetLoadCallback(OnLoad);
    ser->SetRevertCallback(OnRevert);
    F4SE::GetMessagingInterface()->RegisterListener(OnF4SEMessage);
    return true;
}
