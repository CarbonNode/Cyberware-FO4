// cw_voice.cpp - RIPPERDOC VOICES: play a voiced line on the doctor (cw_voice.h, docs/RIPPERDOC-VOICE.md). Game thread only.
//
// The line's topic lives in "Cyberware - Voices.esp" (espgen voices). ObjectReference.Say(topic) on the doctor makes the
// engine pick the topic's one INFO and play Sound\Voice\Cyberware - Voices.esp\<her VoiceType>\<INFO id>_1.fuz from her
// head, with the .lip driving her face. Duration comes from voices.json (measured when the clip was built), so the
// subtitle can hold for it without asking the engine.
//
// A Say on an actor who is still speaking fades the old line and never starts the new one (P8, QD block 16). So Hush and a
// Say over a line both stop the line with the engine's StopCurrentDialogue, in Tick, and a waiting line is said once she is free.
//
// The engine gives no line for a topic with no owner quest, and ours arrive with none, so Say links it first (LinkQuest).
// [Voice] bProbe=1 (test ini only) logs which voice files the engine actually opens and the speaker's state after each Say.
// docs/RIPPERDOC-VOICE.md, "Varga's silence".
#include "PCH.h"

#include "cw_voice.h"

#include <cstddef>
#include <fstream>
#include <typeinfo>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

namespace voice {
namespace {
    struct Line { std::string doc; std::uint32_t topic = 0; int ms = 0; };
    bool g_enabled = false;
    std::string g_plugin;
    std::map<std::string, Line> g_lines;
    RE::ObjectRefHandle g_speaker;
    std::string g_speakerDoc;
    std::function<RE::Actor*(const std::string&)> g_finder;
    std::mutex g_cutLock;          // Say and Hush arrive from the view's callbacks; the cut and the waiting line run in Tick
    bool g_cut = false;            // stop the line playing on the next Tick
    std::optional<std::pair<std::string, Line>> g_next;   // key and line to say once she has stopped
    std::uint64_t g_nextGiveUp = 0;
    bool g_probeOn = false;        // [Voice] bProbe (test ini only): see namespace probe

    cw::json Fail(const char* reason) { return cw::json{ { "ok", false }, { "reason", reason } }; }

    RE::TESObjectREFR* SpeakerFor(const std::string& doc)
    {
        if (doc == g_speakerDoc) {
            if (auto p = g_speaker.get(); p && p->Get3D()) return p.get();
        }
        if (g_finder) {
            if (auto* a = g_finder(doc); a && a->Get3D()) {
                g_speaker = a->GetHandle();
                g_speakerDoc = doc;
                return a;
            }
        }
        return nullptr;
    }

    // ---- Papyrus dispatch on the OG runtime ------------------------------------------------------------------------
    // Our pinned commonlibf4 (a08c731) has two bugs on this path, both fixed upstream after the pin (bumping the pin is
    // shared with PrismaUI F4, so the fixes are local here):
    //  1. BSTThreadScrapFunction is a modern std::function (callable pointer at +0x38). The OG game (1.10.163) was
    //     built with the old MSVC layout and reads it at +0x18. The template's [&] lambda keeps the 4th argument's
    //     address there, so the engine called through our `Actor* = nullptr` and crashed: cw-int-4,
    //     DispatchMethodCallImpl 'call [rax+0x10]', RAX 0. Upstream 296bb0f wraps the callable in the old layout
    //     (LegacyFunction below is that class, commonlibf4 MIT).
    //  2. The CreateObject overloads are declared in slot order, so CreateObject(name, obj) compiles to slot 0x16, which
    //     is CreateObject(name, properties, obj). PackVariable reaches it for any form with no bound script (our
    //     topics). Upstream 48b957c. Checked on the author's Fallout4.exe (sha256 55f57947...) through RTTI + .pdata:
    //     VirtualMachine vtable slot 0x17 = +27333B0 (0x4B bytes, the forwarding overload), 0x16 = +2733410 (0x90),
    //     0x2E = +2736840 (DispatchMethodCall by handle; the crash frame returned into it).
    template <class> class LegacyFunction;
    template <class R, class... A>
    class LegacyFunction<R(A...)>
    {
    public:
        explicit LegacyFunction(std::function<R(A...)> f) : _fn(f ? new Impl{ std::move(f) } : nullptr)
        {
            static_assert(offsetof(LegacyFunction, _fn) == 0x18);
        }
        LegacyFunction(const LegacyFunction&) = delete;
        LegacyFunction& operator=(const LegacyFunction&) = delete;
        ~LegacyFunction() { if (_fn) _fn->delete_this(true); }

    private:
        class Proxy
        {
        public:
            virtual Proxy*                copy(void*) const = 0;    // 00
            virtual Proxy*                move(void*) = 0;          // 01
            virtual R                     do_call(A&&...) = 0;      // 02
            virtual const std::type_info& target_type() const = 0;  // 03
            virtual void                  delete_this(bool) = 0;    // 04
            virtual ~Proxy() = default;                             // 05
            virtual const void*           get() const = 0;          // 06
        };
        class Impl final : public Proxy
        {
        public:
            explicit Impl(std::function<R(A...)> f) : _f(std::move(f)) {}
            Proxy* copy(void*) const override { return new Impl{ _f }; }
            Proxy* move(void*) override { return new Impl{ std::move(_f) }; }
            R do_call(A&&... a) override { return _f(std::forward<A>(a)...); }
            const std::type_info& target_type() const override { return typeid(std::function<R(A...)>); }
            void delete_this(bool dealloc) override { if (dealloc) delete this; else this->~Impl(); }
            const void* get() const override { return std::addressof(_f); }
        private:
            std::function<R(A...)> _f;
        };
        alignas(void*) std::byte _storage[0x18]{};
        Proxy* _fn{};
    };
    static_assert(sizeof(LegacyFunction<void()>) == 0x20);

    using Args = RE::BSScrapArray<RE::BSScript::Variable>;
    using ArgsFn = std::function<bool(Args&)>;
    constexpr std::size_t kSlotCreateObject = 0x17;        // CreateObject(const BSFixedString&, BSTSmartPointer<Object>&)
    constexpr std::size_t kSlotDispatchByHandle = 0x2E;    // DispatchMethodCall(handle, objName, funcName, args, callback)
    bool g_legacyCallables = true;                         // OG loader = OG runtime; NG/AE use the modern std::function

    template <class F>
    F Slot(RE::BSScript::IVirtualMachine* vm, std::size_t i) { return reinterpret_cast<F>((*reinterpret_cast<void***>(vm))[i]); }

    // the form's bound Papyrus object, created and bound when it has none (PackVariable's job, minus bug 2)
    template <class T>
    bool BoundObject(RE::BSScript::IVirtualMachine* vm, T* form, RE::BSTSmartPointer<RE::BSScript::Object>& out)
    {
        const auto type = RE::BSScript::GetVMTypeID<T>();
        RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo> info;
        if (!vm->GetScriptObjectType(type, info) || !info) return false;
        auto& handles = vm->GetObjectHandlePolicy();
        const auto handle = handles.GetHandleForObject(type, form);
        if (handle == handles.EmptyHandle()) return false;
        if (vm->FindBoundObject(handle, info->name.c_str(), false, out, false) && out) return true;
        using create_t = bool (*)(RE::BSScript::IVirtualMachine*, const RE::BSFixedString&, RE::BSTSmartPointer<RE::BSScript::Object>&);
        if (!Slot<create_t>(vm, kSlotCreateObject)(vm, info->name, out) || !out) return false;
        vm->GetObjectBindPolicy().BindObject(out, handle);
        return true;
    }


    // ---- [Voice] bProbe (test ini only): does the engine actually open her voice file? -------------------------------
    // `says` in the log only proves DispatchMethodCall queued the call. This traces the game's own file calls, by IAT, for
    // a few seconds after each Say, so the log names the exact .fuz path the engine asked for and whether it opened. If no
    // path shows up the engine never got as far as the file (the INFO was not picked). Off in Cyberware.ini.
    namespace probe {
        std::atomic<std::uint64_t> g_until{ 0 };
        thread_local bool t_in = false;
        struct Sample { std::uint64_t at; RE::ObjectRefHandle who; int left; };
        std::optional<Sample> g_sample;

        template <class C> std::string Narrow(const C* s)
        {
            std::string o;
            if (s) for (; *s && o.size() < 300; ++s) o.push_back(static_cast<unsigned>(*s) < 128 ? static_cast<char>(*s) : '?');
            return o;
        }
        bool VoicePath(std::string p)
        {
            for (auto& c : p) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            auto ends = [&](std::string_view e) { return p.size() >= e.size() && p.compare(p.size() - e.size(), e.size(), e) == 0; };
            return p.find("sound\\voice") != std::string::npos || p.find("sound/voice") != std::string::npos || ends(".fuz") || ends(".xwm") || ends(".lip");
        }
        template <class C> void Note(const char* fn, const C* path, bool ok)
        {
            if (t_in || GetTickCount64() >= g_until.load(std::memory_order_relaxed)) return;
            t_in = true;
            const DWORD err = GetLastError();
            if (auto p = Narrow(path); VoicePath(p)) REX::INFO("voice-probe: {} '{}' -> {}", fn, p, ok ? "found" : "missing");
            SetLastError(err);
            t_in = false;
        }

        decltype(&::CreateFileW) g_cfw; decltype(&::CreateFileA) g_cfa;
        decltype(&::GetFileAttributesW) g_gaw; decltype(&::GetFileAttributesA) g_gaa;
        decltype(&::FindFirstFileW) g_ffw; decltype(&::FindFirstFileA) g_ffa;
        decltype(&::FindFirstFileExW) g_ffxw;
        decltype(&::GetFileAttributesExW) g_gaxw;

        HANDLE WINAPI CFW(LPCWSTR n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) { auto h = g_cfw(n, a, s, sa, c, f, t); Note("CreateFileW", n, h != INVALID_HANDLE_VALUE); return h; }
        HANDLE WINAPI CFA(LPCSTR n, DWORD a, DWORD s, LPSECURITY_ATTRIBUTES sa, DWORD c, DWORD f, HANDLE t) { auto h = g_cfa(n, a, s, sa, c, f, t); Note("CreateFileA", n, h != INVALID_HANDLE_VALUE); return h; }
        DWORD WINAPI GAW(LPCWSTR n) { auto r = g_gaw(n); Note("GetFileAttributesW", n, r != INVALID_FILE_ATTRIBUTES); return r; }
        BOOL WINAPI GAXW(LPCWSTR n, GET_FILEEX_INFO_LEVELS l, LPVOID d) { auto r = g_gaxw(n, l, d); Note("GetFileAttributesExW", n, r != FALSE); return r; }
        DWORD WINAPI GAA(LPCSTR n) { auto r = g_gaa(n); Note("GetFileAttributesA", n, r != INVALID_FILE_ATTRIBUTES); return r; }
        HANDLE WINAPI FFW(LPCWSTR n, LPWIN32_FIND_DATAW d) { auto h = g_ffw(n, d); Note("FindFirstFileW", n, h != INVALID_HANDLE_VALUE); return h; }
        HANDLE WINAPI FFA(LPCSTR n, LPWIN32_FIND_DATAA d) { auto h = g_ffa(n, d); Note("FindFirstFileA", n, h != INVALID_HANDLE_VALUE); return h; }
        HANDLE WINAPI FFXW(LPCWSTR n, FINDEX_INFO_LEVELS l, LPVOID d, FINDEX_SEARCH_OPS o, LPVOID f, DWORD x) { auto h = g_ffxw(n, l, d, o, f, x); Note("FindFirstFileExW", n, h != INVALID_HANDLE_VALUE); return h; }

        // swap one kernel32 import of the game exe, returning the original (nullptr: not imported by name)
        void* PatchImport(const char* func, void* hook)
        {
            auto* base = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
            auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
            const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
            if (!dir.VirtualAddress) return nullptr;
            for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
                if (_stricmp(reinterpret_cast<char*>(base + d->Name), "kernel32.dll") != 0 || !d->OriginalFirstThunk) continue;
                auto* names = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->OriginalFirstThunk);
                auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->FirstThunk);
                for (; names->u1.AddressOfData; ++names, ++slots) {
                    if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
                    if (strcmp(reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name, func) != 0) continue;
                    void* old = reinterpret_cast<void*>(slots->u1.Function);
                    DWORD prot;
                    if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &prot)) return nullptr;
                    slots->u1.Function = reinterpret_cast<ULONGLONG>(hook);
                    VirtualProtect(&slots->u1.Function, sizeof(void*), prot, &prot);
                    return old;
                }
            }
            return nullptr;
        }
        template <class F> int Hook(const char* func, F*& real, F* hook)
        {
            if (real) return 1;
            real = reinterpret_cast<F*>(PatchImport(func, reinterpret_cast<void*>(hook)));
            if (!real) REX::WARN("voice-probe: {} not imported by name, not traced", func);
            return real ? 1 : 0;
        }
        void Install()
        {
            static bool done = false;
            if (done) return;
            done = true;
            const int n = Hook("CreateFileW", g_cfw, &CFW) + Hook("CreateFileA", g_cfa, &CFA) + Hook("GetFileAttributesW", g_gaw, &GAW) + Hook("GetFileAttributesA", g_gaa, &GAA) +
                          Hook("FindFirstFileW", g_ffw, &FFW) + Hook("FindFirstFileA", g_ffa, &FFA) + Hook("FindFirstFileExW", g_ffxw, &FFXW) + Hook("GetFileAttributesExW", g_gaxw, &GAXW);
            REX::INFO("voice-probe: file trace on ({} of 8 imports hooked)", n);
        }

        bool DialogueMenuOpen() { auto* ui = RE::UI::GetSingleton(); return ui && ui->GetMenuOpen(RE::BSFixedString("DialogueMenu")); }

        void Start(RE::TESObjectREFR* who, RE::TESTopic* topic, bool hadQuest)
        {
            if (!g_probeOn || !who || !topic) return;
            g_until.store(GetTickCount64() + 6000, std::memory_order_relaxed);
            auto* npc = who->GetObjectReference() ? who->GetObjectReference()->As<RE::TESNPC>() : nullptr;
            REX::INFO("voice-probe: Say {:08X} on {:08X}: voice type {:08X}, topic infos {}, quest {:08X} flags {:04X}, DialogueMenu {}", topic->GetFormID(), who->GetFormID(),
                      npc && npc->voiceType ? npc->voiceType->GetFormID() : 0u, topic->numTopicInfos, topic->ownerQuest ? topic->ownerQuest->GetFormID() : 0u,
                      topic->ownerQuest ? topic->ownerQuest->data.flags : 0, DialogueMenuOpen() ? "OPEN" : "closed");
            // The gates of the engine's info picker (see LinkQuest): owner quest, flag bits 0x01 on and 0x80 off, no promote
            // task, and a built info tree. Any one of them failing means no line and no error anywhere.
            auto* q = topic->ownerQuest;
            auto* a = who->As<RE::Actor>();
            REX::INFO("voice-probe: picker gates: owner quest {} before this Say, now {:08X}, flags {:04X}, alreadyRun {}, promote task {}, info tree {}, process {}", hadQuest ? "set" : "EMPTY",
                      q ? q->GetFormID() : 0u, q ? q->data.flags : 0, q ? q->alreadyRun : false, q && q->promoteTask ? "BUSY" : "none", topic->infoTree ? "built" : "MISSING", a && a->currentProcess ? "yes" : "NONE");
            g_sample = Sample{ GetTickCount64() + 300, who->GetHandle(), 4 };
        }
        void Tick()
        {
            if (!g_sample || GetTickCount64() < g_sample->at) return;
            auto p = g_sample->who.get();
            auto* a = p ? p->As<RE::Actor>() : nullptr;
            REX::INFO("voice-probe: speaker {:08X} voiceTimer {} DialogueMenu {}", p ? p->GetFormID() : 0u, a ? a->voiceTimer : -1.0f, DialogueMenuOpen() ? "OPEN" : "closed");
            static const int kNext[] = { 0, 2700, 1200, 700 };   // samples at about 0.3, 1, 2.2 and 4.9 s after the Say
            if (--g_sample->left <= 0 || !p) g_sample.reset();
            else g_sample->at = GetTickCount64() + kNext[g_sample->left];
        }
    }

    // Fallout4.exe 1.10.163 +0x61A820 (TESTopic's info picker) returns "no line" when topic+0x40 (ownerQuest) is null, and
    // Say then ends quietly: no sound, no Papyrus error, ok from the VM. Every topic of this plugin reached the game with the
    // field empty though its QNAM is right (docs/RIPPERDOC-VOICE.md, "Varga's silence"), so it is filled here from the
    // plugin's own quest. kVoiceQuest is locked in data/voices.formids.lock.json.
    constexpr std::uint32_t kVoiceQuest = 0x800;
    void LinkQuest(RE::TESTopic* topic, const std::string& plugin)
    {
        if (topic->ownerQuest || plugin != g_plugin) return;
        auto* dh = RE::TESDataHandler::GetSingleton();
        auto* quest = dh ? dh->LookupForm<RE::TESQuest>(kVoiceQuest, plugin) : nullptr;
        static bool logged = false;
        if (!quest && !logged) REX::WARN("voice: quest {:03X} of '{}' does not exist in the game", kVoiceQuest, plugin);
        else if (quest && !logged) REX::INFO("voice: topics of '{}' had no owner quest; linked to {:08X}", plugin, quest->GetFormID());
        logged = true;
        topic->ownerQuest = quest;
    }

    bool SayTopicIn(RE::TESObjectREFR* speaker, std::uint32_t local, const std::string& plugin)
    {
        auto* dh = RE::TESDataHandler::GetSingleton();
        auto* topic = dh ? dh->LookupForm<RE::TESTopic>(local, plugin) : nullptr;
        if (!topic) { REX::WARN("voice: topic {:03X} not in '{}' (is the plugin ticked?)", local, plugin); return false; }
        auto* game = RE::GameVM::GetSingleton();
        auto vmPtr = game ? game->GetVM() : nullptr;
        auto* vm = vmPtr.get();
        if (!vm) { REX::WARN("voice: no Papyrus VM"); return false; }
        auto& policy = vm->GetObjectHandlePolicy();
        const auto handle = policy.GetHandleForObject(static_cast<std::uint32_t>(speaker->GetFormType()), speaker);
        if (handle == policy.EmptyHandle()) { REX::WARN("voice: no script handle for {:08X}", speaker->GetFormID()); return false; }
        RE::BSTSmartPointer<RE::BSScript::Object> topicObj;
        if (!BoundObject(vm, topic, topicObj)) { REX::WARN("voice: no script object for topic {:08X}", topic->GetFormID()); return false; }

        // ObjectReference.Say(Topic akTopicToSay, Actor akActorToSpeakAs = None, bool abSpeakInPlayersHead = false,
        // ObjectReference akTarget = None). Captured by value: the VM may copy the callable and run it later.
        ArgsFn fill = [topicObj](Args& out) {
            Args a(std::size_t{ 4 });   // explicit size: Variable has an initializer_list ctor too
            a.at(0) = topicObj;
            a.at(1) = nullptr;
            a.at(2) = false;
            a.at(3) = nullptr;
            out = std::move(a);
            return true;
        };
        const RE::BSFixedString objName{ "ObjectReference" }, funcName{ "Say" };
        const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
        using dispatch_t = bool (*)(RE::BSScript::IVirtualMachine*, std::uint64_t, const RE::BSFixedString&, const RE::BSFixedString&,
                                    const void*, const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>&);
        const auto dispatch = Slot<dispatch_t>(vm, kSlotDispatchByHandle);
        const bool hadQuest = topic->ownerQuest != nullptr;
        LinkQuest(topic, plugin);
        probe::Start(speaker, topic, hadQuest);
        if (g_legacyCallables) {
            const LegacyFunction<bool(Args&)> native{ fill };
            return dispatch(vm, handle, objName, funcName, &native, none);
        }
        return dispatch(vm, handle, objName, funcName, &fill, none);
    }
    bool SayTopic(RE::TESObjectREFR* speaker, std::uint32_t local) { return SayTopicIn(speaker, local, g_plugin); }

    bool Talking(RE::TESObjectREFR* who)
    {
        auto* a = who ? who->As<RE::Actor>() : nullptr;
        return a && a->currentProcess && a->voiceTimer > 0.0f;
    }

    bool StartLine(RE::TESObjectREFR* who, const std::string& key, const Line& l)
    {
        if (!SayTopic(who, l.topic)) return false;
        REX::INFO("voice: {} says {} (topic {:03X}, {} ms)", l.doc, key, l.topic, l.ms);
        return true;
    }
}

void Load(const std::string& path, bool enabled, bool legacyCallables, bool probe)
{
    g_enabled = enabled;
    g_probeOn = enabled && probe;
    if (g_probeOn) probe::Install();
    g_legacyCallables = legacyCallables;
    g_lines.clear();
    std::ifstream f(path);
    if (!f) { REX::INFO("voice: {} ({}) - text only", enabled ? "on" : "off", path + " not found"); g_enabled = false; return; }
    cw::json j = cw::json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.is_object()) { REX::WARN("voice: {} is not JSON - text only", path); g_enabled = false; return; }
    auto hex = [](const cw::json& v) -> std::uint32_t { return v.is_string() ? std::stoul(v.get<std::string>(), nullptr, 16) : 0; };
    g_plugin = j.value("plugin", std::string("Cyberware - Voices.esp"));
    for (auto& [k, v] : j.value("lines", cw::json::object()).items())
        g_lines[k] = Line{ v.value("doc", std::string()), hex(v.value("topic", cw::json())), v.value("ms", 0) };
    REX::INFO("voice: {} - {} line(s) from '{}'", g_enabled ? "ON" : "off ([Voice] bEnabled=0)", g_lines.size(), g_plugin);
}

void SetSpeaker(RE::TESObjectREFR* ref, const std::string& doc)
{
    g_speaker = ref ? ref->GetHandle() : RE::ObjectRefHandle();
    g_speakerDoc = ref ? doc : std::string();
}

void SetFinder(std::function<RE::Actor*(const std::string&)> finder) { g_finder = std::move(finder); }

bool Enabled() { return g_enabled; }

cw::json Say(const std::string& doc, const std::string& key, bool paused)
{
    if (!g_enabled) return Fail("disabled");
    auto it = g_lines.find(key);
    if (it == g_lines.end() || (!doc.empty() && it->second.doc != doc)) return Fail("no-clip");
    if (paused) return Fail("paused");   // Say and the face freeze under a paused menu; the subtitle reads on its own
    auto* who = SpeakerFor(it->second.doc);
    if (!who) return Fail("no-actor");
    {
        std::lock_guard lock(g_cutLock);
        if (Talking(who)) {   // she is mid-line: Tick stops it, then says this one
            g_cut = true;
            g_next.emplace(key, it->second);
            g_nextGiveUp = GetTickCount64() + 1500;
            REX::INFO("voice: {} waits to say {}: stopping the line playing", it->second.doc, key);
            return cw::json{ { "ok", true }, { "ms", it->second.ms }, { "queued", true } };
        }
        g_cut = false;   // a hush sent just before this line has nothing left to stop
        g_next.reset();
    }
    if (!StartLine(who, key, it->second)) return Fail("vm");
    return cw::json{ { "ok", true }, { "ms", it->second.ms } };
}

cw::json Hush()
{
    if (!g_enabled) return Fail("disabled");
    std::lock_guard lock(g_cutLock);
    g_next.reset();   // a line still waiting must not start after the talk closes
    auto p = g_speaker.get();
    if (!Talking(p.get())) return cw::json{ { "ok", true }, { "idle", true } };
    g_cut = true;
    return cw::json{ { "ok", true } };
}

void Tick()
{
    probe::Tick();
    bool cut = false;
    std::optional<std::pair<std::string, Line>> next;
    std::uint64_t giveUp = 0;
    {
        std::lock_guard lock(g_cutLock);
        cut = std::exchange(g_cut, false);
        next = g_next;
        giveUp = g_nextGiveUp;
    }
    if (!cut && !next) return;
    auto p = g_speaker.get();
    auto* a = p ? p->As<RE::Actor>() : nullptr;
    if (cut && a && a->currentProcess) {
        const float was = a->voiceTimer;
        a->StopCurrentDialogue(true);
        REX::INFO("voice: stopped the line playing (voiceTimer {} -> {})", was, a->voiceTimer);
    }
    if (!next) return;
    const bool free = a && !Talking(p.get());
    const bool late = GetTickCount64() >= giveUp;
    if (!free && !late) return;
    {
        std::lock_guard lock(g_cutLock);
        if (!g_next || g_next->first != next->first) return;   // a hush or a newer line replaced it
        g_next.reset();
    }
    if (!free) REX::WARN("voice: {} still speaking after the stop, saying {} anyway", next->second.doc, next->first);
    if (p && p->Get3D()) StartLine(p.get(), next->first, next->second);
}

// Actor.PlayIdleAction(Action akAction, ObjectReference akTarget = None) through the same legacy-safe dispatch as Say.
// The arm code's {"cmd":"action"} seam and the slam attack buffer called commonlibf4's DispatchMethodCall template, which
// hits both pinned-commonlibf4 bugs above: P14 crashed in DispatchMethodCallImpl reading 0xDE645 (INTEGRATION 3,
// 2026-11-09 20:21:38, ActionRightPowerAttack, stack Cyberware.dll -> F4SE task -> UpdateNonRenderSafeAITasks).
bool PlayIdleAction(RE::Actor* actor, RE::BGSAction* action)
{
    if (!actor || !action) return false;
    auto* game = RE::GameVM::GetSingleton();
    auto vmPtr = game ? game->GetVM() : nullptr;
    auto* vm = vmPtr.get();
    if (!vm) { REX::WARN("papyrus: PlayIdleAction: no Papyrus VM"); return false; }
    auto& policy = vm->GetObjectHandlePolicy();
    const auto handle = policy.GetHandleForObject(static_cast<std::uint32_t>(RE::ENUM_FORM_ID::kACHR), static_cast<RE::TESObjectREFR*>(actor));
    if (handle == policy.EmptyHandle()) { REX::WARN("papyrus: PlayIdleAction: no script handle for {:08X}", actor->GetFormID()); return false; }
    RE::BSTSmartPointer<RE::BSScript::Object> actObj;
    if (!BoundObject(vm, action, actObj)) { REX::WARN("papyrus: PlayIdleAction: no script object for action {:08X}", action->GetFormID()); return false; }
    ArgsFn fill = [actObj](Args& out) {   // by value: the VM may copy the callable
        Args a(std::size_t{ 2 });
        a.at(0) = actObj;
        a.at(1) = nullptr;
        out = std::move(a);
        return true;
    };
    const RE::BSFixedString objName{ "Actor" }, funcName{ "PlayIdleAction" };
    const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
    using dispatch_t = bool (*)(RE::BSScript::IVirtualMachine*, std::uint64_t, const RE::BSFixedString&, const RE::BSFixedString&,
                                const void*, const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor>&);
    const auto dispatch = Slot<dispatch_t>(vm, kSlotDispatchByHandle);
    if (g_legacyCallables) {
        const LegacyFunction<bool(Args&)> native{ fill };
        return dispatch(vm, handle, objName, funcName, &native, none);
    }
    return dispatch(vm, handle, objName, funcName, &fill, none);
}

// Game.ShakeCamera(None, strength, secs) + Game.ShakeController(strength, strength, secs) for the Mantis leap's landing,
// with the legacy-safe callable (the same commonlibf4 template bug as PlayIdleAction; the leap had never landed in a test,
// so this had not crashed yet). akSource None = centred on the player, so no object needs packing. DispatchStaticCall has
// a single virtual (no overload slot shuffle, unlike CreateObject / DispatchMethodCall), so the declared call is safe.
void LandingShake(float strength, float secs, bool& camera, bool& controller)
{
    camera = controller = false;
    auto* game = RE::GameVM::GetSingleton();
    auto vmPtr = game ? game->GetVM() : nullptr;
    auto* vm = vmPtr.get();
    if (!vm) return;
    using Fn = RE::BSTThreadScrapFunction<bool(Args&)>;
    const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
    auto call = [&](const char* fnName, ArgsFn fill) {
        const RE::BSFixedString cls{ "Game" }, fn{ fnName };
        if (g_legacyCallables) {
            const LegacyFunction<bool(Args&)> native{ fill };
            return vm->DispatchStaticCall(cls, fn, *reinterpret_cast<const Fn*>(&native), none);
        }
        const Fn modern{ fill };
        return vm->DispatchStaticCall(cls, fn, modern, none);
    };
    camera = call("ShakeCamera", [strength, secs](Args& out) {
        Args a(std::size_t{ 3 });
        a.at(0) = nullptr;
        a.at(1) = strength;
        a.at(2) = secs;
        out = std::move(a);
        return true;
    });
    controller = call("ShakeController", [strength, secs](Args& out) {
        Args a(std::size_t{ 3 });
        a.at(0) = strength;
        a.at(1) = strength;
        a.at(2) = secs;
        out = std::move(a);
        return true;
    });
}

// Game.ForceThirdPerson() / Game.ForceFirstPerson(), zero-argument statics, through the same legacy-safe callable as
// LandingShake. INSTALL SCENE (docs/INSTALL-SCENE.md): the chair scene is framed in third person, and the view the
// player had is restored when it ends (SCN-9). DispatchStaticCall has a single virtual, so the declared call is safe.
// Works whether or not [Voice] is enabled: this is a camera call, not a line.
bool ForcePerson(bool third)
{
    auto* game = RE::GameVM::GetSingleton();
    auto vmPtr = game ? game->GetVM() : nullptr;
    auto* vm = vmPtr.get();
    if (!vm) { REX::WARN("papyrus: ForcePerson: no Papyrus VM"); return false; }
    using Fn = RE::BSTThreadScrapFunction<bool(Args&)>;
    const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> none;
    const RE::BSFixedString cls{ "Game" }, fn{ third ? "ForceThirdPerson" : "ForceFirstPerson" };
    ArgsFn fill = [](Args& out) { out = Args(std::size_t{ 0 }); return true; };
    bool ok = false;
    if (g_legacyCallables) {
        const LegacyFunction<bool(Args&)> native{ fill };
        ok = vm->DispatchStaticCall(cls, fn, *reinterpret_cast<const Fn*>(&native), none);
    } else {
        const Fn modern{ fill };
        ok = vm->DispatchStaticCall(cls, fn, modern, none);
    }
    REX::INFO("papyrus: Game.{} -> {}", third ? "ForceThirdPerson" : "ForceFirstPerson", ok ? "queued" : "refused");
    return ok;
}

bool ProbeSay(const std::string& plugin, std::uint32_t local, std::uint32_t refId)
{
    if (!g_enabled) { REX::WARN("voice-probe: voice is off"); return false; }
    RE::TESObjectREFR* who = nullptr;
    if (refId) { if (auto* f = RE::TESForm::GetFormByID(refId)) who = f->As<RE::TESObjectREFR>(); }
    else who = SpeakerFor("diamondcity");
    if (!who || !who->Get3D()) { REX::WARN("voice-probe: no loaded speaker ({:08X})", refId); return false; }
    const bool ok = SayTopicIn(who, local, plugin);
    REX::INFO("voice-probe: ProbeSay {}:{:06X} on {:08X} -> {}", plugin, local, who->GetFormID(), ok);
    return ok;
}
}  // namespace voice
