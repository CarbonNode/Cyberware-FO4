// cw_fx.inl - included inside main.cpp's anonymous namespace, right after namespace optics.
// ABILITIES stream (2026-11-08): Sandevistan effects and Kiroshi highlights. The forms are Cyberware.esp copies of vanilla
// (espgen: CW_FxKiroshi*, CW_FxSandyGhost, CW_ImodSandy*), tinted here at runtime; nothing vanilla changes colour.
// Sounds are vanilla SNDRs by editor id. Bullet Time - Slow Time (SlowTime.esp) coexists: while a Sandevistan runs, its
// spells are removed, so there is never a double slow-time.
namespace fx {
    inline std::uint32_t ColorU32(int r, int g, int b) { return (std::uint32_t)r | ((std::uint32_t)g << 8) | ((std::uint32_t)b << 16); }
    inline RE::TESEffectShader* Efsh(const char* edid) { auto* f = g_map.Get(edid); return f ? f->As<RE::TESEffectShader>() : nullptr; }
    inline RE::TESImageSpaceModifier* Imod(const char* edid) { auto* f = g_map.Get(edid); return f ? f->As<RE::TESImageSpaceModifier>() : nullptr; }
    inline void Sound(const char* soundEditorId) { RE::UIUtils::PlayMenuSound(soundEditorId); }

    // --- scene-graph safety (cw-crash-sandy, p13c Sandevistan AV) ------------------------------------------------
    // A per-actor FX pass runs on the F4SE main-thread task (the frame pump relays through AddTask), but the scene
    // graph is churned while nearby actors are killed, disabled or unloaded: an actor's BSFadeNode can detach from the
    // scene the same frame we walk it. Validate before every dereference; never cache a NiNode across frames.
    // (The P13c AV itself was a wrong address, not churn: see WorldRoot() below.)
    inline bool PtrLooksValid(const void* p)   // a live user-space heap pointer: canonical, 8-aligned, not tiny
    {
        const auto v = reinterpret_cast<std::uintptr_t>(p);
        return v >= 0x10000 && v < 0x0000'8000'0000'0000 && (v & 0x7) == 0;
    }
    // The guard every pass uses. Null is the normal "nothing there" and stays quiet. A NON-null pointer that fails
    // the sanity test should never happen, so it is logged once per `what`: a guard that trips in silence turns a
    // feature off without a trace (the afterimages were dead for P14 and P15 exactly that way).
    inline bool PtrOk(const void* p, const char* what)
    {
        if (!p) return false;
        if (PtrLooksValid(p)) return true;
        static std::vector<const char*> said;
        if (std::find(said.begin(), said.end(), what) == said.end()) {
            said.push_back(what);
            REX::WARN("fx: guard tripped: {} is not a usable pointer ({:#018X}); whatever needs it is skipped (logged once)", what, reinterpret_cast<std::uintptr_t>(p));
        }
        return false;
    }
    // The world root, re-fetched now and sanity-checked; nullptr if it does not look like a node.
    // NEVER call RE::Main::WorldRootNode(): CommonLibF4 @a08c731 binds it to ID::Main::WorldRootCamera, which is a
    // FUNCTION, so it returns that function's first 8 code bytes as a pointer. The P13c "torn pointer"
    // 0x4804D67CA1058B48 is `48 8B 05 A1 7C D6 04 48` = `mov rax,[rip+0x04D67CA1]`, the first instruction of
    // Main::WorldRootCamera (exe+0xD3C6B0 in 1.10.163); that instruction loads exe+0x5AA4358, which is exactly
    // ID::Main::WorldRootNode (569647). Read that global ourselves (cw-sandy-worldroot).
    inline RE::SceneGraph* WorldRoot()
    {
        static REL::Relocation<RE::NiPointer<RE::SceneGraph>*> nodePtr{ RE::ID::Main::WorldRootNode };
        auto* w = nodePtr->get();
        return PtrOk(w, "the world root") ? w : nullptr;
    }
    // True when `top` is `node` or one of its ancestors. A bounded walk with every hop pointer-checked: proof that
    // the node we are about to attach under really is in the live scene this actor's 3D hangs in.
    inline bool IsAncestorOf(const RE::NiAVObject* top, const RE::NiAVObject* node)
    {
        for (int hop = 0; top && node && hop < 64; ++hop) {
            if (node == top) return true;
            const RE::NiAVObject* up = node->parent;
            if (up && !PtrOk(up, "a scene-graph parent")) return false;
            node = up;
        }
        return false;
    }
    // The actor's live, scene-attached 3D root, or nullptr to skip it. Skips deleted/disabled actors and any whose
    // 3D is gone or detached (no parent). Callers keep their own life-state test. Re-fetch each frame; never cache.
    inline RE::NiAVObject* LiveRoot(RE::Actor* a)
    {
        if (!a || a->IsDeleted() || a->IsDisabled()) return nullptr;
        auto* root = a->Get3D();
        if (!PtrOk(root, "an actor's 3D root") || !root->parent) return nullptr;
        return root;
    }

    // the player's tint (customization stream's tint.h, not merged on every branch yet): false = stock, use the default
    inline bool TintOf(const char* target, std::uint32_t& out, int* glow = nullptr)
    {
#if CW_HAS_TINT
        tint::Rgb c{};
        if (tint::Color(target, c, glow)) { out = ColorU32((int)(c.r * 255.f), (int)(c.g * 255.f), (int)(c.b * 255.f)); return true; }
#else
        (void)target; (void)out; (void)glow;
#endif
        return false;
    }
    inline bool Pulses()
    {
#if CW_HAS_TINT
        return tint::PulsesOn();
#else
        return true;
#endif
    }
    inline void ApplyColor(RE::TESEffectShader* s, std::uint32_t c) { if (!s) return; s->data.fillColor1 = s->data.fillColor2 = s->data.fillColor3 = s->data.edgeColor = c; }

    // Shaders stack when re-applied before they expire, so refresh only near the end of the last application.
    struct Mark { std::uint64_t until = 0; RE::TESEffectShader* sh = nullptr; };
    std::map<std::uint32_t, Mark> marks;
    inline void MarkRef(RE::TESObjectREFR* r, RE::TESEffectShader* sh, float dur, std::uint64_t now)
    {
        if (!r || !sh || !PtrOk(r->Get3D(), "a shaded ref's 3D")) return;   // no live 3D to shade (cw-crash-sandy)
        auto& m = marks[r->GetFormID()];
        if (m.sh == sh && now + 150 < m.until) return;
        r->ApplyEffectShader(sh, dur);
        m.sh = sh; m.until = now + (std::uint64_t)(dur * 1000.f);
    }
    inline void PruneMarks(std::uint64_t now) { for (auto it = marks.begin(); it != marks.end();) { if (it->second.until + 5000 < now) it = marks.erase(it); else ++it; } }

    enum Role { kHostile, kNeutral, kFriendly, kLegendary, kLoot, kRoles };
    RE::TESEffectShader* shaders[kRoles] = {};
    bool shadersLoaded = false;
    int roleDefaults[kRoles][3] = { { 255, 40, 40 }, { 255, 210, 40 }, { 60, 255, 90 }, { 255, 190, 30 }, { 60, 220, 255 } };
    // AAA-GAPS row 25: Second Heart refilled health with no sound and no screen effect, so a player could not tell a save from a
    // lucky miss. The save now plays SOUND's CW_SND_SecondHeart (Cyberpunk's own Second Heart sound: one hard beat, peak at
    // 0.175 s, a low swell to about 1.7 s) with a screen flash on the beat; the end of its cooldown plays the short
    // CW_SND_SecondHeartReady tone. The flash is a Fallout4.esm imagespace modifier, as the Kiroshi scan's is, and obeys the same
    // Pulses switch. Until the wavs are staged the vanilla fallbacks play (cw_sound.inl), also with [Sound] bCues=0: the
    // quickhack cues behave the same way in play today.
    void AbilityCue(const std::string& handler, const std::string& phase)
    {
        if (handler != "ci_second_heart") return;
        if (phase == "trigger") {
            snd::Cue("CW_SND_SecondHeart", "UIVATSCriticalExecuted");
            bool flashed = false;
            if (g_ini.secondHeartImod && Pulses())
                if (auto* f = RE::TESForm::GetFormByID((std::uint32_t)g_ini.secondHeartImod))
                    if (auto* m = f->As<RE::TESImageSpaceModifier>()) { RE::ImageSpaceModifierInstanceForm::Trigger(m, 1.0f, nullptr); flashed = true; }
            REX::INFO("fx: Second Heart cue: trigger (sound CW_SND_SecondHeart, flash {:08X} {})", (std::uint32_t)g_ini.secondHeartImod,
                      flashed ? "triggered" : !g_ini.secondHeartImod ? "off (iSecondHeartImod=0)" : !Pulses() ? "off (pulses are switched off)" : "NOT FOUND or not an imagespace modifier");
        } else if (phase == "ready") {
            snd::Cue("CW_SND_SecondHeartReady", "UIVATSCriticalAvailable");
            REX::INFO("fx: Second Heart cue: ready (sound CW_SND_SecondHeartReady)");
        }
    }

    void LoadShaders()
    {
        if (shadersLoaded) return;
        shadersLoaded = true;
        static const char* ids[kRoles] = { "CW_FxKiroshiHostile", "CW_FxKiroshiNeutral", "CW_FxKiroshiFriendly", "CW_FxKiroshiLegendary", "CW_FxKiroshiLoot" };
        for (int i = 0; i < kRoles; ++i) {
            shaders[i] = Efsh(ids[i]);
            if (shaders[i]) ApplyColor(shaders[i], ColorU32(roleDefaults[i][0], roleDefaults[i][1], roleDefaults[i][2]));
            else REX::WARN("fx: {} missing from Cyberware.esp (old ESP?)", ids[i]);
        }
    }

    bool IsLegendary(RE::Actor* a)
    {
        if (auto* av = FindAV("LegendaryRank"); av && static_cast<RE::ActorValueOwner*>(a)->GetActorValue(*av) > 0.f) return true;
        const char* n = a->GetDisplayFullName();
        return n && std::strstr(n, "Legendary") != nullptr;
    }
    Role Classify(RE::Actor* a, RE::PlayerCharacter* p)
    {
        if (IsLegendary(a)) return kLegendary;
        if (arms::IsHostile(a, p)) return kHostile;
        if (a->IsFollowing()) return kFriendly;
        return kNeutral;
    }

    // range by the installed Kiroshi: tier, or the catalog's highlight_m; 0 = no optics
    double HighlightRangeM()
    {
        double r = 0;
        static const double kByTier[6] = { 0, 15, 22, 32, 45, 60 };
        for (auto& [id, n] : g_activeNative) {
            if (n.handler.rfind("face_kiroshi_", 0) != 0) continue;
            const double d = kByTier[std::clamp(n.tier, 1, 5)];
            r = std::max(r, arms::Pn(n, "highlight_m", d));
        }
        return r;
    }
    double PulseSeconds()
    {
        double s = 0;
        for (auto& [id, n] : g_activeNative) if (n.handler.rfind("face_kiroshi_", 0) == 0) s = std::max(s, 6.0 + n.tier * 1.5);
        return s;
    }

    std::uint64_t pulseUntil = 0, pulseLootUntil = 0;
    constexpr std::uint64_t kSweepMs = 700;
    std::uint64_t sweepUntil = 0;
    void Highlights()   // 4 Hz, from the frame pump; 30 Hz during the sweep (SweepFrame)
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* pl = RE::ProcessLists::GetSingleton();
        const std::uint64_t now = GetTickCount64();
        if (!p || !pl || now >= pulseUntil) { PruneMarks(now); return; }
        LoadShaders();
        const double rangeM = HighlightRangeM();
        if (rangeM <= 0) return;
        const double range = rangeM * 70.0;
        // the sweep: highlights spread outward from the player over the scan's first 0.7 s
        const double reach = now < sweepUntil ? range * (std::min)(1.0, (double)(now - (sweepUntil - kSweepMs)) / (double)kSweepMs) : range;
        const auto pp = p->GetPosition();
        std::uint32_t kirColor = 0;
        const bool tinted = TintOf("kiroshi", kirColor);
        for (auto list : { &pl->highActorHandles, &pl->middleHighActorHandles }) {
            for (auto& hd : *list) {
                auto a = hd.get();
                if (!a || a.get() == p || a->lifeState != 0 || !LiveRoot(a.get())) continue;
                auto ap = a->GetPosition();
                const double dx = ap.x - pp.x, dy = ap.y - pp.y, dz = ap.z - pp.z;
                if (dx * dx + dy * dy + dz * dz > reach * reach) continue;
                Role role = Classify(a.get(), p);
                // The author's custom Kiroshi colour tints the neutral role (default-coloured ones stay readable: red/green carry meaning)
                if (role == kNeutral && tinted) ApplyColor(shaders[kNeutral], kirColor);
                MarkRef(a.get(), shaders[role], 0.8f, now);
            }
        }
        // loot: containers with something in them, nearest 20, only in the scan pulse's first 4 s
        if (now < pulseLootUntil && shaders[kLoot]) {
            auto* tes = RE::TES::GetSingleton();
            if (tes) {
                int n = 0;
                tes->ForEachReferenceInRange(p, (float)std::min(range, 25.0 * 70.0), [&](RE::TESObjectREFR* r) {
                    if (n >= 20 || !r || r->IsDeleted() || r->IsDisabled() || !PtrOk(r->Get3D(), "a loot container's 3D")) return RE::BSContainer::ForEachResult::kContinue;
                    auto* base = r->GetObjectReference();
                    if (!base || base->GetFormType() != RE::ENUM_FORM_ID::kCONT) return RE::BSContainer::ForEachResult::kContinue;
                    MarkRef(r, shaders[kLoot], 0.8f, now);
                    ++n;
                    return RE::BSContainer::ForEachResult::kContinue;
                });
            }
        }
    }

    void SweepFrame()
    {
        static std::uint64_t next = 0;
        const std::uint64_t now = GetTickCount64();
        if (now >= sweepUntil || now < next) return;
        next = now + 33;
        Highlights();
    }

    std::string LootLine(RE::Actor* a)
    {
        auto* inv = a->inventoryList;
        if (!inv) return {};
        std::vector<std::pair<std::string, std::uint32_t>> items;
        {
            const RE::BSAutoReadLock lock{ inv->rwLock };
            for (auto& it : inv->data) {
                if (!it.object) continue;
                const auto t = it.object->GetFormType();
                if (t != RE::ENUM_FORM_ID::kWEAP && t != RE::ENUM_FORM_ID::kARMO && t != RE::ENUM_FORM_ID::kALCH && t != RE::ENUM_FORM_ID::kMISC && t != RE::ENUM_FORM_ID::kAMMO) continue;
                const std::uint32_t c = it.GetCount();
                const std::string nameStr(RE::TESFullName::GetFullName(*it.object, false));
                const char* n = nameStr.c_str();
                if (n && *n && c > 0 && !std::strstr(n, "Pipboy") && !std::strstr(n, "Pip-Boy")) items.push_back({ n, c });
            }
        }
        std::sort(items.begin(), items.end(), [](auto& x, auto& y) { return x.second > y.second; });
        std::string out;
        for (size_t i = 0; i < items.size() && i < 3; ++i) { if (i) out += ", "; out += items[i].first; if (items[i].second > 1) out += " x" + std::to_string(items[i].second); }
        return out;
    }
    void ScanReadout()   // the aimed-at actor: level, weakness and what it carries, as a one-shot HUD note
    {
        auto t = g_env.FindHackTarget(60);
        auto* tf = t.ok ? RE::TESForm::GetFormByID(t.id) : nullptr;
        auto* a = tf ? tf->As<RE::Actor>() : nullptr;
        if (!a) return;
        static const std::pair<const char*, const char*> kResists[] = { { "DamageResist", "ballistic" }, { "EnergyResist", "energy" }, { "RadResistExposure", "radiation" }, { "PoisonResist", "poison" } };
        const char* weak = nullptr; float low = 1e9f;
        for (auto& [av, label] : kResists) {
            auto* info = FindAV(av);
            if (!info) continue;
            const float v = static_cast<RE::ActorValueOwner*>(a)->GetActorValue(*info);
            if (v < low) { low = v; weak = label; }
        }
        const char* name = a->GetDisplayFullName();
        const std::string loot = LootLine(a);
        // Health now / full (BETA QA truth list row 16: every Kiroshi's scan, Mk.1 included, which has no mod slot for
        // Target Analysis). The same two reads Target Analysis uses.
        auto* hpA = FindAV("Health");
        const float hpNow = hpA ? std::max(0.0f, static_cast<RE::ActorValueOwner*>(a)->GetActorValue(*hpA)) : 0.0f;
        const float hpFull = hpA ? static_cast<RE::ActorValueOwner*>(a)->GetPermanentActorValue(*hpA) : 0.0f;
        const std::string hpText = hpA ? std::format(" - HP {:.0f}/{:.0f}", hpNow, hpFull) : std::string();
        const std::string line = std::format("{} (Lv {}){}{}{}{}{}", name && *name ? name : "target", a->GetLevel(), hpText,
                                             weak ? " - weak to " : "", weak ? weak : "", loot.empty() ? "" : " - carries ", loot);
        hud::Event({ { "type", "note" }, { "text", line } });
        if (HudText()) RE::SendHUDMessage::ShowHUDMessage(line.c_str(), "", false, false);
        REX::INFO("optics: scan '{}'", line);
    }
    void OnScanKey()   // the scan key was tapped (game thread)
    {
        const double secs = PulseSeconds();
        if (secs <= 0) return;
        const std::uint64_t now = GetTickCount64();
        pulseUntil = now + (std::uint64_t)(secs * 1000.0);
        pulseLootUntil = now + 4000;
        sweepUntil = now + kSweepMs;
        snd::Cue("CW_SND_KiroshiScan", "UIVATSReadySD");
        if (auto* f = RE::TESForm::GetFormByID(0x00024590); f && Pulses()) if (auto* m = f->As<RE::TESImageSpaceModifier>()) RE::ImageSpaceModifierInstanceForm::Trigger(m, 0.5f, nullptr);   // VATSScanImod
        Highlights();
        ScanReadout();
    }
    void OnLoad() { marks.clear(); pulseUntil = 0; pulseLootUntil = 0; sweepUntil = 0; }
}  // namespace fx

namespace sandy {
    constexpr const char* kSndEnter = "UIVATSEnterSD", * kSndExit = "UIVATSExitSD", * kSndBeat = "UIHealthHeartbeatALP";
    struct Palette { const char* suffix; int r, g, b; RE::TESImageSpaceModifier* in, * loop, * out; };
    Palette palette[] = { { "", 46, 255, 140 }, { "_Cyan", 40, 220, 255 }, { "_Blue", 60, 110, 255 }, { "_Violet", 170, 70, 255 }, { "_Red", 255, 60, 70 }, { "_Amber", 255, 170, 40 }, { "_White", 235, 240, 245 } };
    RE::TESImageSpaceModifier* imIn = nullptr, * imLoop = nullptr, * imOut = nullptr;   // the variant chosen when this activation started
    int lastPalette = 0;
    RE::TESEffectShader* ghost = nullptr;
    bool loaded = false, on = false, light = false;   // light = Kerenzikov / Reflex Tuner: a shorter flourish of the same FX
    std::string handler;
    std::uint64_t startedAt = 0, nextBeat = 0, nextGhost = 0, loopUntil = 0;
    int tier = 1;

    void Load()
    {
        if (loaded) return;
        loaded = true;
        for (auto& p : palette) {
            p.in = fx::Imod((std::string("CW_ImodSandyIn") + p.suffix).c_str());
            p.loop = fx::Imod((std::string("CW_ImodSandyLoop") + p.suffix).c_str());
            p.out = fx::Imod((std::string("CW_ImodSandyOut") + p.suffix).c_str());
        }
        imIn = palette[0].in; imLoop = palette[0].loop; imOut = palette[0].out;
        ghost = fx::Efsh("CW_FxSandyGhost");
        // HologramShaunFXS, the source of this copy, holds its edge glow for 20 s whatever duration Play is given: end it with each application
        if (ghost) { ghost->data.edgeAlphaFullTime = 1.2f; ghost->data.edgeAlphaFadeOutTime = 0.3f; ghost->data.fillAlphaFadeOutTime = 0.3f; }
        REX::INFO("fx: sandevistan forms in={} loop={} out={} ghost={} (palette 7)", imIn != nullptr, imLoop != nullptr, imOut != nullptr, ghost != nullptr);
    }
    // Bullet Time - Slow Time (SlowTime.esp): SlowTimePlayer + SlowTimeAPSpell are constant abilities on the player while it runs.
    bool RemoveBulletTime(RE::PlayerCharacter* p)
    {
        auto* dh = RE::TESDataHandler::GetSingleton();
        if (!dh || !p) return false;
        static bool checked = false, present = false;
        static std::uint32_t slowId = 0, apId = 0;
        if (!checked) {
            checked = true;
            if (auto* s = dh->LookupForm(0x800, "SlowTime.esp")) { slowId = s->GetFormID(); present = true; }
            if (auto* a = dh->LookupForm(0x805, "SlowTime.esp")) apId = a->GetFormID();
            if (present) REX::INFO("fx: Bullet Time found (SPEL {:08X}, AP {:08X}): Sandevistan takes priority over it", slowId, apId);
        }
        if (!present) return false;
        bool removed = false;
        for (auto* sp : p->addedSpells) {
            if (!sp) continue;
            const auto id = sp->GetFormID();
            if (id == slowId || (apId && id == apId)) {
                RE::Console::ExecuteCommand(std::format("player.removespell {:08X}", id).c_str());
                removed = true;
            }
        }
        return removed;
    }
    double Strength(int t, bool lightFx) { return (lightFx ? 0.45 : 0.55) + std::clamp(t, 1, 5) * (lightFx ? 0.04 : 0.09); }
    void GhostActors(RE::PlayerCharacter* p, std::uint64_t now)
    {
        if (!ghost) return;
        fx::ApplyColor(ghost, [&]() { std::uint32_t c = fx::ColorU32(palette[lastPalette].r, palette[lastPalette].g, palette[lastPalette].b); fx::TintOf("sandevistan", c); return c; }());
        fx::MarkRef(p, ghost, 1.2f, now);   // the player's own 3rd-person trail
        auto* pl = RE::ProcessLists::GetSingleton();
        if (!pl) return;
        const auto pp = p->GetPosition();
        const double range = 45.0 * 70.0;
        for (auto& hd : pl->highActorHandles) {
            auto a = hd.get();
            if (!a || a.get() == p || a->lifeState != 0 || !fx::LiveRoot(a.get())) continue;
            auto ap = a->GetPosition();
            const double dx = ap.x - pp.x, dy = ap.y - pp.y;
            if (dx * dx + dy * dy > range * range) continue;
            fx::MarkRef(a.get(), ghost, 1.2f, now);
        }
    }
    // True afterimages ([Sandevistan] bAfterimages, off by default): every 120 ms a frozen copy of up to 3 moving actors' current pose stays
    // behind for 450 ms in the Sandevistan colour. The copy is a scene-graph clone (no actor, no AI, collision removed), held only in memory.
    struct Echo { RE::NiPointer<RE::NiAVObject> node; std::uint64_t killAt; };
    std::vector<Echo> echoes;
    std::map<std::uint32_t, RE::NiPoint3> lastPos;
    std::uint64_t nextEcho = 0;
    int echoLogs = 0;
    void StripCollision(RE::NiAVObject* o)
    {
        if (!o) return;
        o->collisionObject.reset();
        if (auto* n = o->IsNode()) for (auto& c : n->children) StripCollision(c.get());
    }
    RE::NiAVObject* CloneTree(RE::NiAVObject* src)
    {
        RE::NiCloningProcess cp{};
        cp.copyType = RE::NiCloningProcess::CopyType::kCopyExact;
        cp.appendChar = '$';
        cp.scale = { 1.0f, 1.0f, 1.0f };
        auto* c = src->CreateClone(cp);
        if (!c) return nullptr;
        src->ProcessClone(cp);
        return static_cast<RE::NiAVObject*>(c);
    }
    constexpr std::size_t kMaxEchoes = 12;   // hard cap on live afterimage clones attached to the world at once
    void ClearEchoes()
    {
        auto* world = fx::WorldRoot();
        for (auto& e : echoes) if (world && e.node) world->DetachChild(e.node.get());
        echoes.clear();
        lastPos.clear();
    }
    void EchoFrame(RE::PlayerCharacter* p, std::uint64_t now)
    {
        auto* world = fx::WorldRoot();
        auto* pl = RE::ProcessLists::GetSingleton();
        if (!world || !pl || !ghost) {   // say why once per activation: a silent return hid a dead feature for two builds
            if (echoLogs++ < 1) REX::WARN("fx: afterimages cannot run (world root {}, process lists {}, ghost shader {})", world ? "ok" : "MISSING", pl ? "ok" : "MISSING", ghost ? "ok" : "MISSING");
            return;
        }
        for (auto it = echoes.begin(); it != echoes.end();) {
            if (now >= it->killAt) { if (it->node) world->DetachChild(it->node.get()); it = echoes.erase(it); } else ++it;
        }
        if (now < nextEcho || echoes.size() >= kMaxEchoes) return;
        nextEcho = now + 120;
        const auto pp = p->GetPosition();
        std::vector<std::pair<double, RE::Actor*>> moving;
        for (auto& hd : pl->highActorHandles) {
            auto a = hd.get();
            if (!a || a.get() == p || a->lifeState != 0 || !fx::LiveRoot(a.get())) continue;
            const auto ap = a->GetPosition();
            auto [seen, fresh] = lastPos.try_emplace(a->GetFormID(), ap);
            if (fresh) continue;   // first sight this activation: no earlier position, so nothing has moved yet (a map default of 0,0,0 made every actor "moving" on the first pass)
            auto& lp = seen->second;
            const double mx = ap.x - lp.x, my = ap.y - lp.y;
            lp = ap;
            const double dx = ap.x - pp.x, dy = ap.y - pp.y, d2 = dx * dx + dy * dy;
            if (mx * mx + my * my < 4.0 || d2 > 2500.0 * 2500.0) continue;   // standing still leaves no trail
            moving.push_back({ d2, a.get() });
        }
        std::sort(moving.begin(), moving.end(), [](auto& x, auto& y) { return x.first < y.first; });
        for (size_t i = 0; i < moving.size() && i < 3; ++i) {
            if (echoes.size() >= kMaxEchoes) break;
            auto* a = moving[i].second;
            auto* root = fx::LiveRoot(a);   // re-validate THIS actor now: a kill/disable/unload this frame may have freed its 3D
            if (!root) continue;
            auto* w = fx::WorldRoot();      // re-fetch the world root for this attach; never reuse one cached across the loop
            if (!w) break;                  // the scene root is being rebuilt: abandon the afterimage pass this frame
            if (!fx::IsAncestorOf(w, root)) {   // this actor's 3D does not hang under the node we resolved: never attach there
                if (echoLogs++ < 6) REX::WARN("fx: afterimage skipped for {:08X}: its 3D is not under the world root", a->GetFormID());
                continue;
            }
            const auto t0 = std::chrono::steady_clock::now();
            auto* c = CloneTree(root);
            if (!c) { if (echoLogs++ < 4) REX::WARN("fx: afterimage clone failed for {:08X}", a->GetFormID()); continue; }
            StripCollision(c);
            c->local = root->world;
            w->AttachChild(c, true);
            RE::NiUpdateData ud{};
            c->UpdateWorldData(&ud);
            a->ApplyEffectShader(ghost, 0.45f, nullptr, false, false, c);
            echoes.push_back({ RE::NiPointer<RE::NiAVObject>(c), now + 450 });
            if (echoLogs++ < 6) REX::INFO("fx: afterimage of {:08X}: clone {} us, {} live", a->GetFormID(), std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count(), echoes.size());
        }
    }
    // the player's Sandevistan colour: tint.h target "sandevistan" (default green when stock), nearest baked palette entry
    int PickPalette()
    {
        std::uint32_t c = 0;
        if (!fx::TintOf("sandevistan", c)) return 0;
        const int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
        int best = 0, bestD = 1 << 30, i = 0;
        for (auto& p : palette) { int d = (p.r - r) * (p.r - r) + (p.g - g) * (p.g - g) + (p.b - b) * (p.b - b); if (d < bestD) { bestD = d; best = i; } ++i; }
        return best;
    }
    void Start(const std::string& h, int t, bool lightFx)
    {
        Load();
        on = true; handler = h; tier = t; light = lightFx;
        const std::uint64_t now = GetTickCount64();
        startedAt = now; nextBeat = now + 250; nextGhost = now; loopUntil = 0;
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!lightFx && RemoveBulletTime(p)) REX::INFO("fx: Bullet Time ended for the Sandevistan");
        lastPalette = PickPalette();
        imIn = palette[lastPalette].in; imLoop = palette[lastPalette].loop; imOut = palette[lastPalette].out;
        const float strength = (float)Strength(t, lightFx);
        if (imIn && fx::Pulses()) RE::ImageSpaceModifierInstanceForm::Trigger(imIn, strength, nullptr);
        snd::Cue(lightFx ? "CW_SND_Kerenzikov" : "CW_SND_SandyEnter", kSndEnter);
        hud::Event({ { "type", "sandyOn" }, { "tier", t } });
        REX::INFO("fx: sandevistan ON ({}, T{}, strength {:.2f}, light {})", h, t, strength, lightFx);
        echoLogs = 0;   // the afterimage lines are budgeted per activation, so every test run shows them
        if (!lightFx && g_ini.sandyEchoes) REX::INFO("fx: afterimages on (world root {})", fx::WorldRoot() ? "ok" : "MISSING");
    }
    void Stop()
    {
        if (!on) return;
        on = false;
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (imLoop) RE::ImageSpaceModifierInstanceForm::Stop(imLoop);
        ClearEchoes();
        int cleared = 0;
        if (ghost) {   // every ghost ends with the Sandevistan, not when the shader's own timers run out
            const std::uint32_t id = ghost->GetFormID();
            const auto clear = [&](std::uint32_t ref) { if (RE::TESForm::GetFormByID(ref)) { RE::Console::ExecuteCommand(std::format("{:08X}.StopMagicShaderVisuals {:08X}", ref, id).c_str()); ++cleared; } };
            for (auto it = fx::marks.begin(); it != fx::marks.end();) {
                if (it->second.sh == ghost) { clear(it->first); it = fx::marks.erase(it); } else ++it;
            }
        }
        if (imOut && fx::Pulses()) RE::ImageSpaceModifierInstanceForm::Trigger(imOut, (float)Strength(tier, light), nullptr);
        if (light) fx::Sound(kSndExit); else snd::Cue("CW_SND_SandyExit", kSndExit);
        if (p) RemoveBulletTime(p);   // a Bullet Time toggled during the Sandevistan never resumes under it
        hud::Event({ { "type", "sandyOff" } });
        REX::INFO("fx: sandevistan OFF (ghost cleared on {} actor(s))", cleared);
    }
    void Tick()   // every frame from the pump (self-throttled)
    {
        static std::uint64_t nextPoll = 0;
        const std::uint64_t now = GetTickCount64();
        if (now < nextPoll) return;
        nextPoll = now + 40;
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!p) return;
        const cw::NativeAbility* found = nullptr;
        bool lightFx = false;
        for (auto& [id, n] : g_activeNative) {
            const bool isSandy = n.handler.rfind("os_sandy_", 0) == 0;
            const bool isLight = n.handler == "ns_kerenzikov" || n.handler == "ns_reflex_tuner";
            if (!isSandy && !isLight) continue;
            if (arms::TimerLeft(n.handler.c_str()) <= 0) continue;
            if (!found || isSandy) { found = &n; lightFx = !isSandy; }
            if (isSandy) break;
        }
        if (found && (!on || found->handler != handler)) { if (on) Stop(); Start(found->handler, found->tier, lightFx); }
        else if (!found && on) { Stop(); return; }
        if (!on) return;
        if (!light && now >= nextGhost) { nextGhost = now + 260; GhostActors(p, now); RemoveBulletTime(p); }
        if (!light && now >= nextBeat) { nextBeat = now + 900; fx::Sound(kSndBeat); }
        if (!light && g_ini.sandyEchoes) EchoFrame(p, now);
        // the loop imagespace as the 0.25 s fade-in peaks, re-triggered before its 13 s end
        const std::uint64_t loopDelay = 200;
        if (imLoop && now >= startedAt + loopDelay && now >= loopUntil) {
            RE::ImageSpaceModifierInstanceForm::Trigger(imLoop, (float)Strength(tier, light), nullptr);
            loopUntil = now + 12000;
        }
    }
    void OnLoad() { ClearEchoes(); if (on) { on = false; if (imLoop) RE::ImageSpaceModifierInstanceForm::Stop(imLoop); } }
}  // namespace sandy

// Fallout-native chrome (ABILITIES stream, 2026-11-08). Hack key context on machines, follower focus fire, rad purge, Apex Lens ping.
// The turret / robot state is put back at kPreSaveGame so a save never holds a turned turret or a shut-down robot (RuntimeMods rule).
// D2 (2026-11-10): ...but a save never CANCELS a machine hack either. Suspend() at pre-save gives the world state back and keeps each
// hack with its time left; Resume() at post-save takes it again. The cosave ('HAKS') carries them, and Restore() re-applies them after a
// load. A Core Overload fuse resumes with its time left; if its target is gone after a load, the recharge is refunded.
namespace chrome {
    struct Hacked { std::uint32_t id; std::uint64_t until; int mode; std::int64_t leftMs = -1; };   // mode 0: turret on our side, 1: robot shut down, 2: Core Overload fuse; leftMs >= 0 while suspended for a save
    bool suspended = false;
    std::int64_t overrideLeftMs = 0, overloadLeftMs = 0;   // recharge left, frozen while suspended
    std::vector<Hacked> hacked;
    std::uint64_t overrideReadyAt = 0, overloadReadyAt = 0;
    void RefConsole(std::uint32_t id, const std::string& cmd) { RE::Console::ExecuteCommand(std::format("{:08X}.{}", id, cmd).c_str()); }
    void Say(const std::string& line) { hud::Event({ { "type", "note" }, { "text", line } }); if (HudText()) RE::SendHUDMessage::ShowHUDMessage(line.c_str(), "", false, false); REX::INFO("chrome: {}", line); }
    void Revert(const Hacked& h)
    {
        if (h.mode == 0) { RefConsole(h.id, "removefac 1c21c"); RefConsole(h.id, "stopcombat"); }
        else RefConsole(h.id, "setunconscious 0");
        REX::INFO("chrome: machine hack on {:08X} ended ({})", h.id, h.mode == 0 ? "turret back to its own side" : h.mode == 1 ? "robot switched back on" : "fuse cancelled");   // the rows need the end as well as the start
    }
    // Quickhack presentation: the aimed hostile wears a cyan lock (the locked target during an upload) and each hack lands with its own
    // vanilla shader, stopped when the hack's effect ends. Vanilla shaders run on their own timers, so the stop is explicit.
    struct HackShow { std::uint32_t ref; RE::TESEffectShader* sh; std::uint64_t until; };
    std::vector<HackShow> hackShows;
    std::uint32_t uploadTarget = 0;
    void StopShow(const HackShow& s) { if (s.sh && RE::TESForm::GetFormByID(s.ref)) RefConsole(s.ref, std::format("StopMagicShaderVisuals {:08X}", s.sh->GetFormID())); }
    // ---- READY-abilities-aaa-1 D4/D5: what a quickhack's item text promises beyond an actor value.
    // BLIND (Reboot Optics, "Target is blinded: loses detection for 12 s"): for that time the target starts no fight and helps nobody
    // (Aggression and Assistance to 0, the values the vanilla Calm effects use) and the fight it is in is stopped. A hit on it still
    // wakes it (the engine's assault response), as a blinded man still feels a blade. Both values are given back when the time is up
    // and before a save (a save never holds a pacified actor: RuntimeMods rule), and taken again after the save.
    // Gated by [Quickhacks] bHackEffects. Unverified in game: see READY-ab-d45-effects-1.
    struct Blind { std::uint32_t id = 0; std::uint64_t until = 0; float aggr = 0, assist = 0; bool held = false; };
    std::vector<Blind> blinds;
    void BlindSet(Blind& b, bool on, const char* why)
    {
        auto* f = RE::TESForm::GetFormByID(b.id);
        auto* a = f ? f->As<RE::Actor>() : nullptr;
        if (!a || a->IsDeleted() || on == b.held) return;
        auto* o = static_cast<RE::ActorValueOwner*>(a);
        auto* ag = FindAV("Aggression");
        auto* as = FindAV("Assistance");
        if (!ag || !as) { REX::WARN("chrome: blind: actor value Aggression or Assistance not found, {:08X} is not blinded", b.id); return; }
        if (on) {
            b.aggr = o->GetActorValue(*ag); b.assist = o->GetActorValue(*as);
            if (b.aggr > 0) o->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *ag, -b.aggr);
            if (b.assist > 0) o->ModActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage, *as, -b.assist);
            const bool fought = a->IsInCombat();
            if (a->lifeState == 0) RefConsole(b.id, "stopcombat");
            REX::INFO("chrome: blind ON {:08X} ({}): Aggression {:.0f} -> {:.0f}, Assistance {:.0f} -> {:.0f}, in combat before {}, {:.1f} s left", b.id, why, b.aggr, o->GetActorValue(*ag),
                      b.assist, o->GetActorValue(*as), fought, b.until > GetTickCount64() ? (double)(b.until - GetTickCount64()) / 1000.0 : 0.0);
        } else {
            if (b.aggr > 0) o->RestoreActorValue(*ag, b.aggr);       // gives back damage only, so it can never raise the value past its own
            if (b.assist > 0) o->RestoreActorValue(*as, b.assist);
            REX::INFO("chrome: blind OFF {:08X} ({}): Aggression {:.0f}, Assistance {:.0f}, alive {}, in combat {}", b.id, why, o->GetActorValue(*ag), o->GetActorValue(*as), a->lifeState == 0, a->IsInCombat());
        }
        b.held = on;
    }
    // STAGGER (Cyberware Malfunction, "Target is staggered"): one hit reaction as the hack lands. First the graph's own stagger
    // (the variables and event the engine's hit staggers drive); if the target's graph refuses the event (a turret, a creature
    // without one), the small knock the arms use for targets that must not ragdoll. The log says which one ran.
    void Stagger(std::uint32_t id)
    {
        auto* f = RE::TESForm::GetFormByID(id);
        auto* a = f ? f->As<RE::Actor>() : nullptr;
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!a || !p || a->IsDeleted() || a->lifeState != 0 || !fx::PtrOk(a->Get3D(), "a staggered target's 3D")) return;
        auto* g = static_cast<RE::IAnimationGraphManagerHolder*>(a);
        const bool dir = g->SetGraphVariableFloat(RE::BSFixedString("staggerDirection"), 0.0f);
        const bool mag = g->SetGraphVariableFloat(RE::BSFixedString("staggerMagnitude"), 0.75f);
        const bool ev = g->NotifyAnimationGraphImpl(RE::BSFixedString("staggerStart"));
        bool knock = false;
        if (!ev && g_ini.knockback && a->currentProcess) { auto from = p->GetPosition(); a->currentProcess->KnockExplosion(a, from, 0.4f); knock = true; }
        REX::INFO("chrome: stagger {:08X}: graph event {} (direction var {}, magnitude var {}){}", id, ev ? "accepted" : "refused", dir, mag,
                  ev ? "" : knock ? ", fell back to the small knock" : ", no fallback (bKnockback off or no process)");
    }
    // PING ("Highlights all enemies within 30 m of the target for 30 s"): every living hostile within the radius of the centre wears
    // the Kiroshi hostile highlight for the hack's time. The centre follows the pinged actor while it lives (then stays where it
    // died); with no target it is the player. Not gated: it is the same shader path the Kiroshi scan uses.
    struct PingShow { std::uint32_t centre = 0; RE::NiPoint3 at; std::uint64_t until = 0; double radius = 0; bool said = false; } ping;
    void PingTick(RE::PlayerCharacter* p, RE::ProcessLists* pl, std::uint64_t now)
    {
        if (now >= ping.until) return;
        fx::LoadShaders();
        if (!ping.centre) ping.at = p->GetPosition();
        else if (auto* f = RE::TESForm::GetFormByID(ping.centre))
            if (auto* c = f->As<RE::Actor>(); c && !c->IsDeleted() && c->Get3D()) ping.at = c->GetPosition();
        int n = 0;
        for (auto list : { &pl->highActorHandles, &pl->middleHighActorHandles }) {
            for (auto& hd : *list) {
                auto a = hd.get();
                if (!a || a.get() == p || a->lifeState != 0 || !fx::LiveRoot(a.get()) || !arms::IsHostile(a.get(), p)) continue;
                const auto ap = a->GetPosition();
                const double dx = ap.x - ping.at.x, dy = ap.y - ping.at.y, dz = ap.z - ping.at.z;
                if (dx * dx + dy * dy + dz * dz > ping.radius * ping.radius) continue;
                fx::MarkRef(a.get(), fx::shaders[fx::kHostile], 0.8f, now);
                ++n;
            }
        }
        if (!ping.said) { ping.said = true; Say(std::format("Ping: {} hostile{} within {:.0f} m", n, n == 1 ? "" : "s", ping.radius / 70.0)); }
    }
    void HackEffect(const std::string& kind, std::uint32_t target, double seconds, double radiusM)
    {
        const std::uint64_t now = GetTickCount64();
        if (kind == "ping") {
            ping = {};
            ping.centre = target; ping.until = now + (std::uint64_t)(seconds * 1000.0); ping.radius = radiusM * 70.0;
            if (auto* p = RE::PlayerCharacter::GetSingleton()) ping.at = p->GetPosition();
            REX::INFO("chrome: ping for {:.0f} s, {:.0f} m around {}", seconds, radiusM, target ? std::format("{:08X}", target) : std::string("the player"));
            return;
        }
        if (!g_ini.hackEffects) { if (g_ini.debug) REX::INFO("chrome: hack effect '{}' on {:08X} skipped ([Quickhacks] bHackEffects=0)", kind, target); return; }
        if (kind == "stagger") { Stagger(target); return; }
        if (kind == "blind") {
            const std::uint64_t until = now + (std::uint64_t)(seconds * 1000.0);
            for (auto& b : blinds) if (b.id == target) { b.until = (std::max)(b.until, until); BlindSet(b, true, "refreshed"); return; }
            blinds.push_back({ target, until });
            BlindSet(blinds.back(), true, "Reboot Optics");
        }
    }
    // ---- D2 (save and load) over D4/D5 (blind, stagger, ping), stacked 2026-11-10 by CRASH.
    // One pair of save hooks, D2's: Suspend() before the save gives back machine hacks AND blinds, Resume() after it takes
    // both again (Resume is what D4/D5 called PostSave). A LOAD: OnLoad() forgets both lists (the loaded actors come from a
    // save that held neither); machine hacks come back from the cosave through Restore() below, and a blind comes back through
    // the core, whose restored Reboot Optics debuff asks for its blind again with the time it has left (Engine::HacksRestore).
    void RevertAll()   // everything a hack holds on an actor is given back and forgotten: machine hacks (not a running fuse) and blinds
    {
        for (auto& h : hacked) if (h.mode != 2) Revert(h);
        hacked.clear();
        for (auto& s : hackShows) StopShow(s);
        hackShows.clear();
        for (auto& b : blinds) BlindSet(b, false, "revert all");
        blinds.clear();
    }
    void Apply(const Hacked& h)
    {
        if (h.mode == 0) { RefConsole(h.id, "stopcombat"); RefConsole(h.id, "addfac 1c21c 1"); }
        else if (h.mode == 1) RefConsole(h.id, "setunconscious 1");
    }
    std::int64_t LeftOf(std::uint64_t at, std::uint64_t now) { return at > now ? (std::int64_t)(at - now) : 0; }
    void Suspend()   // kPreSaveGame
    {
        if (suspended) return;
        const std::uint64_t now = GetTickCount64();
        for (auto& h : hacked) { h.leftMs = LeftOf(h.until, now); if (h.mode != 2) Revert(h); }
        overrideLeftMs = LeftOf(overrideReadyAt, now); overloadLeftMs = LeftOf(overloadReadyAt, now);
        for (auto& s : hackShows) StopShow(s);   // vanilla shaders: cosmetic, not carried across the save
        hackShows.clear();
        for (auto& b : blinds) BlindSet(b, false, "before the save");   // D4: a save never holds a pacified actor
        suspended = true;
    }
    void Resume()    // kPostSaveGame: the save took no time out of any hack
    {
        if (!suspended) return;
        const std::uint64_t now = GetTickCount64();
        for (auto& h : hacked) { h.until = now + (std::uint64_t)std::max<std::int64_t>(0, h.leftMs); h.leftMs = -1; if (h.mode != 2) Apply(h); }
        overrideReadyAt = now + (std::uint64_t)overrideLeftMs; overloadReadyAt = now + (std::uint64_t)overloadLeftMs;
        suspended = false;
        for (auto& b : blinds) if (now < b.until) BlindSet(b, true, "after the save");   // D4: the blind is taken again; its clock ran on through the save, so one that ran out during it is not taken for a tick (ARMS)
        if (!hacked.empty()) REX::INFO("chrome: {} machine hack(s) resumed after the save", hacked.size());
    }
    json SaveJson()  // the cosave form: time LEFT, never clock times
    {
        const std::uint64_t now = GetTickCount64();
        json a = json::array();
        for (auto& h : hacked) a.push_back({ { "id", h.id }, { "mode", h.mode }, { "leftMs", h.leftMs >= 0 ? h.leftMs : LeftOf(h.until, now) } });
        return { { "hacked", a },
                 { "overrideLeftMs", suspended ? overrideLeftMs : LeftOf(overrideReadyAt, now) },
                 { "overloadLeftMs", suspended ? overloadLeftMs : LeftOf(overloadReadyAt, now) } };
    }
    int Restore(const json& j)   // after a load, once OnLoad has cleared everything; ids are already resolved
    {
        if (!j.is_object()) return 0;
        const std::uint64_t now = GetTickCount64();
        overrideReadyAt = now + (std::uint64_t)std::max<std::int64_t>(0, j.value("overrideLeftMs", (std::int64_t)0));
        overloadReadyAt = now + (std::uint64_t)std::max<std::int64_t>(0, j.value("overloadLeftMs", (std::int64_t)0));
        int n = 0;
        for (auto& e : j.value("hacked", json::array())) {
            if (!e.is_object()) continue;
            Hacked h{ e.value("id", 0u), 0, e.value("mode", 0), -1 };
            const std::int64_t left = e.value("leftMs", (std::int64_t)0);
            auto* f = h.id ? RE::TESForm::GetFormByID(h.id) : nullptr;
            auto* a = f ? f->As<RE::Actor>() : nullptr;
            if (!a || a->lifeState != 0) {   // gone or dead after the load
                if (h.mode == 2) { overloadReadyAt = 0; Say("Core Overload: the target is gone after the load; recharge refunded"); }
                REX::INFO("chrome: load: machine hack on {:08X} (mode {}) not restored: target gone or dead", h.id, h.mode);
                continue;
            }
            if (left <= 0 && h.mode != 2) continue;   // ran out during the save
            h.until = now + (std::uint64_t)std::max<std::int64_t>(0, left);
            Apply(h);
            hacked.push_back(h);
            ++n;
        }
        if (n) REX::INFO("chrome: {} machine hack(s) restored after the load", n);
        return n;
    }
    void OnQuickhack(std::uint32_t target);
    void HackVisual(const std::string& phase, const std::string& id, std::uint32_t target, double seconds)
    {
        if (phase == "upload") { uploadTarget = target; snd::Cue("CW_SND_QuickhackCast", "UIVATSTargetLock"); return; }
        uploadTarget = 0;
        if (phase == "cancel") { snd::Cue("CW_SND_QhUploadStop", "UIVATSExitSD"); return; }   // interrupted: Companion Uplink does nothing (D3)
        if (phase == "applied") OnQuickhack(target);   // D3: the hack LANDED on this target; followers focus exactly it
        // Fallout4.esm EFSH: GammaGunHit, LaserFireHit, CryoFreeze, ShockDamage, FireFXS, PoisonDamage, ShockDamageConstant, CritPlasma
        static const std::map<std::string, std::uint32_t> kLook = { { "qh_reboot_optics", 0x21C768 }, { "qh_cyberware_malfunction", 0x216352 }, { "qh_cripple_movement", 0x18C35B },
                                                                    { "qh_short_circuit", 0x06F122 }, { "qh_overheat", 0x01D22B }, { "qh_contagion", 0x06F123 },
                                                                    { "qh_synapse_burnout", 0x2494E4 }, { "qh_blackwall_gateway", 0x139F97 } };
        const auto it = kLook.find(id);
        static const std::map<std::string, const char*> kSnd = { { "qh_overheat", "CW_SND_QhOverheat" }, { "qh_short_circuit", "CW_SND_QhShortCircuit" }, { "qh_ping", "CW_SND_QhPing" },
                                                                 { "qh_reboot_optics", "CW_SND_QhRebootOptics" }, { "qh_cyberware_malfunction", "CW_SND_QhCyberMalfunction" },
                                                                 { "qh_cripple_movement", "CW_SND_QhCrippleMovement" }, { "qh_contagion", "CW_SND_QhContagion" },
                                                                 { "qh_synapse_burnout", "CW_SND_QhSynapseBurnout" }, { "qh_blackwall_gateway", "CW_SND_QhBlackwall" } };
        const auto sit = kSnd.find(id);
        snd::Cue(sit != kSnd.end() ? sit->second : "", "UIVATSReadySD");
        if (it == kLook.end()) return;
        auto* tf = RE::TESForm::GetFormByID(target);
        auto* a = tf ? tf->As<RE::Actor>() : nullptr;
        auto* sf = RE::TESForm::GetFormByID(it->second);
        auto* sh = sf ? sf->As<RE::TESEffectShader>() : nullptr;
        if (!a || !sh || a->IsDeleted() || a->IsDisabled() || !fx::PtrOk(a->Get3D(), "a quickhack target's 3D")) return;   // quickhack target gone/detached (cw-crash-sandy)
        a->ApplyEffectShader(sh, (float)seconds);
        hackShows.push_back({ target, sh, GetTickCount64() + (std::uint64_t)(seconds * 1000.0) });
    }
    bool IsTurret(RE::Actor* a) { auto* r = a->race; const char* e = r ? r->GetFormEditorID() : nullptr; return e && std::strstr(e, "Turret"); }

    // The robot or turret nearest the view heading (12 deg, 40 m). Its own search, wider than the quickhack picker: turrets are often
    // in the middle-high process list and their small hulls fail LOS at close range, so LOS is only required past 12 m.
    RE::Actor* AimedMachine()
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        auto* pl = RE::ProcessLists::GetSingleton();
        if (!p || !pl) return nullptr;
        static const char* kKw[] = { "Fallout4.esm|02CB73", "Fallout4.esm|0B2BF3" };   // ActorTypeRobot, ActorTypeTurret
        auto machine = [&](RE::Actor* a) {
            for (auto* fk : kKw) {
                auto* f = ResolveFormKey(fk);
                auto* kw = f ? f->As<RE::BGSKeyword>() : nullptr;
                if (!kw) continue;
                if ((a->race && static_cast<const RE::BGSKeywordForm*>(a->race)->HasKeyword(kw)) ||
                    (a->GetNPC() && static_cast<const RE::BGSKeywordForm*>(a->GetNPC())->HasKeyword(kw))) return true;
            }
            return false;
        };
        const auto pp = p->GetPosition();
        const double h = p->GetHeading(), maxU = 40.0 * 70.0, near_ = 12.0 * 70.0;
        const double deg = 180.0 / 3.14159265358979, cone = 12.0 / deg, hull = 90.0;
        RE::Actor* best = nullptr;
        double bestErr = cone, bestDist = maxU;
        int logged = 0;
        const std::pair<const char*, const RE::BSTArray<RE::ActorHandle>*> lists[] = {
            { "high", &pl->highActorHandles }, { "middleHigh", &pl->middleHighActorHandles }, { "middleLow", &pl->middleLowActorHandles }, { "low", &pl->lowActorHandles } };
        for (auto& [listName, list] : lists) {
            for (auto& hd : *list) {
                auto a = hd.get();
                if (!a || a.get() == p) continue;
                auto ap = a->GetPosition();
                const double dx = ap.x - pp.x, dy = ap.y - pp.y, dist = std::sqrt(dx * dx + dy * dy);
                if (dist < 1 || dist > maxU) continue;
                // The angle to the hull's edge, not its centre: a robot charging at arm's length sits far off a flat cone yet fills the view.
                const double err = cw::AimError(pp.x, pp.y, h, ap.x, ap.y, hull);
                const bool isMachine = machine(a.get()), hostile = arms::IsHostile(a.get(), p), following = a->IsFollowing(), has3D = a->Get3D() != nullptr;
                const char* why = !isMachine ? "not a machine" : a->lifeState != 0 ? "not alive" : (following && !hostile) ? "follower" : err >= cone ? "off aim" : nullptr;
                if (!why && dist > near_) { bool pick = false; if (!has3D || !p->HasLOSToTarget(a.get(), &pick)) why = "no line of sight"; }
                if (g_ini.debug && logged < 16) {
                    ++logged;
                    const char* race = a->race ? a->race->GetFormEditorID() : nullptr;
                    REX::INFO("chrome: aim {:08X} '{}' list={} formType={} machine={} life={} knock={} 3D={} hostile={} following={} dist={:.0f} err={:.1f}: {}", a->GetFormID(), race ? race : "?", listName, (int)a->GetFormType(),
                              isMachine, (int)a->lifeState, (int)a->knockState, has3D, hostile, following, dist, err * deg, why ? why : "candidate");
                }
                if (why) continue;
                if (cw::BetterAim(err, dist, bestErr, bestDist)) { best = a.get(); bestErr = err; bestDist = dist; }
            }
        }
        return best;
    }
    bool OnHackKey()   // true = handled here: the aimed target was a machine and an installed uplink took the key
    {
        const auto* over = arms::Find("fc_robco_override");
        const auto* core = arms::Find("fc_core_overload");
        if (!over && !core) return false;
        auto* a = AimedMachine();
        if (g_ini.debug) REX::INFO("chrome: hack key: uplink over={} core={} aimed machine {:08X}", over != nullptr, core != nullptr, a ? a->GetFormID() : 0);
        if (!a) return false;
        struct { std::uint32_t id; } t{ a->GetFormID() };
        const std::uint64_t now = GetTickCount64();
        const char* nm = a->GetDisplayFullName();
        const std::string name = nm && *nm ? nm : "target";
        const bool turret = IsTurret(a);
        if (core && !turret && now >= overloadReadyAt) {
            overloadReadyAt = now + (std::uint64_t)(arms::Pn(*core, "cooldown_s", 45) * 1000);
            hacked.push_back({ t.id, now + (std::uint64_t)(arms::Pn(*core, "fuse_s", 3) * 1000), 2 });
            Say(std::format("Core Overload: {} detonates in {:.0f} s", name, arms::Pn(*core, "fuse_s", 3)));
            return true;
        }
        if (!over) { if (core && !turret) Say("Core Overload recharging"); return core && !turret; }
        if (now < overrideReadyAt) { Say(std::format("RobCo Override recharging: {} s", (overrideReadyAt - now) / 1000 + 1)); return true; }
        overrideReadyAt = now + (std::uint64_t)(arms::Pn(*over, "cooldown_s", 30) * 1000);
        if (turret) {
            RefConsole(t.id, "stopcombat");
            RefConsole(t.id, "addfac 1c21c 1");
            hacked.push_back({ t.id, now + (std::uint64_t)(arms::Pn(*over, "turret_s", 60) * 1000), 0 });
            Say(std::format("RobCo Override: {} is on your side for {:.0f} s", name, arms::Pn(*over, "turret_s", 60)));
        } else {
            RefConsole(t.id, "setunconscious 1");
            hacked.push_back({ t.id, now + (std::uint64_t)(arms::Pn(*over, "robot_s", 20) * 1000), 1 });
            Say(std::format("RobCo Override: {} shut down for {:.0f} s", name, arms::Pn(*over, "robot_s", 20)));
        }
        fx::Sound("UIVATSTargetLock");
        return true;
    }
    // An uplink but no cyberdeck, and no machine in the sights: say so instead of a silent key.
    void OnNoTarget() { if (arms::Find("fc_robco_override") || arms::Find("fc_core_overload")) Say("No machine in your sights"); }
    // A quickhack LANDED on `target` (HackVisual "applied", once per upload, main target only): Companion Uplink followers focus it.
    // D3 (2026-11-10): it used to fire when the upload STARTED and re-pick whatever was under the crosshair, so followers attacked even
    // when the upload was then interrupted, and Ping (no target) sent them in too. Now: complete uploads only, the hacked target only.
    void OnQuickhack(std::uint32_t target)
    {
        const auto* up = arms::Find("fc_companion_uplink");
        auto* pl = RE::ProcessLists::GetSingleton();
        auto* p = RE::PlayerCharacter::GetSingleton();
        if (!up || !pl || !p || !target) return;
        auto* tf = RE::TESForm::GetFormByID(target);
        auto* ta = tf ? tf->As<RE::Actor>() : nullptr;
        if (!ta || ta->lifeState != 0) return;   // the hack killed it (Short Circuit, Blackwall): nobody to focus
        struct { std::uint32_t id; } t{ target };
        const double r = arms::Pn(*up, "focus_m", 40) * 70.0;
        const auto pp = p->GetPosition();
        int n = 0;
        for (auto& hd : pl->highActorHandles) {
            auto a = hd.get();
            if (!a || a.get() == p || a->lifeState != 0 || !a->IsFollowing()) continue;
            auto ap = a->GetPosition();
            if ((ap.x - pp.x) * (ap.x - pp.x) + (ap.y - pp.y) * (ap.y - pp.y) > r * r) continue;
            RefConsole(a->GetFormID(), std::format("startcombat {:08X}", t.id));
            ++n;
        }
        if (n) Say(std::format("Companion Uplink: {} follower(s) on your target", n));
    }
    std::set<std::uint32_t> apexSeen;
    void ApexPing(RE::PlayerCharacter* p, RE::ProcessLists* pl, std::uint64_t now)
    {
        const auto* apex = arms::Find("mod_optics_apex");
        if (!apex) return;
        fx::LoadShaders();
        const double range = arms::Pn(*apex, "apex_m", 120) * 70.0;
        const auto pp = p->GetPosition();
        std::set<std::uint32_t> here;
        for (auto list : { &pl->highActorHandles, &pl->middleHighActorHandles }) {
            for (auto& hd : *list) {
                auto a = hd.get();
                if (!a || a.get() == p || a->lifeState != 0 || !fx::LiveRoot(a.get()) || !fx::IsLegendary(a.get())) continue;
                auto ap = a->GetPosition();
                const double dx = ap.x - pp.x, dy = ap.y - pp.y;
                if (dx * dx + dy * dy > range * range) continue;
                here.insert(a->GetFormID());
                fx::MarkRef(a.get(), fx::shaders[fx::kLegendary], 1.2f, now);
                if (!apexSeen.count(a->GetFormID())) {
                    const char* nm = a->GetDisplayFullName();
                    Say(std::format("Apex Lens: {} {:.0f} m away", nm && *nm ? nm : "legendary", std::sqrt(dx * dx + dy * dy) / 70.0));
                }
            }
        }
        apexSeen = here;
    }
    void Tick()   // 4 Hz
    {
        auto* p = RE::PlayerCharacter::GetSingleton();
        const std::uint64_t showNow = GetTickCount64();
        for (auto it = hackShows.begin(); it != hackShows.end();) { if (showNow >= it->until) { StopShow(*it); it = hackShows.erase(it); } else ++it; }
        if (p && g_engine && g_engine->Ram() >= 0) {   // a deck is installed: show what the hack key would upload to
            fx::LoadShaders();
            std::uint32_t lock = uploadTarget;
            if (!lock) { const auto t = g_env.FindHackTarget(60); if (t.ok) lock = t.id; }
            if (auto* lf = lock ? RE::TESForm::GetFormByID(lock) : nullptr)
                if (auto* la = lf->As<RE::Actor>(); la && la->lifeState == 0) fx::MarkRef(la, fx::shaders[fx::kLoot], 0.8f, showNow);
        }
        auto* pl = RE::ProcessLists::GetSingleton();
        if (!p || !pl) return;
        const std::uint64_t now = GetTickCount64();
        for (auto it = hacked.begin(); !suspended && it != hacked.end();) {
            if (now < it->until) { ++it; continue; }
            if (it->mode == 2) {   // Core Overload: the fuse ran out
                if (auto* f = RE::TESForm::GetFormByID(it->id)) if (auto* a = f->As<RE::Actor>(); a && a->lifeState == 0) {
                    if (auto* boom = g_map.Get("CW_GorillaSlamShockwave")) RefConsole(it->id, std::format("placeatme {:08X}", boom->GetFormID()));
                    RefConsole(it->id, "kill");
                    REX::INFO("chrome: Core Overload: {:08X} detonated", it->id);
                }
            } else Revert(*it);
            it = hacked.erase(it);
        }
        for (auto it = blinds.begin(); it != blinds.end();) { if (now >= it->until) { BlindSet(*it, false, "time up"); it = blinds.erase(it); } else ++it; }
        PingTick(p, pl, now);
        ApexPing(p, pl, now);
        if (const auto* rs = arms::Find("ci_rad_scrubber")) {
            if (auto* av = FindAV("Rads"); av) {
                auto* o = static_cast<RE::ActorValueOwner*>(p);
                if (o->GetActorValue(*av) > 0.f) o->RestoreActorValue(*av, (float)(arms::Pn(*rs, "rad_purge_per_s", 0.5) * 0.25));
            }
        }
    }
    void OnLoad() { hacked.clear(); hackShows.clear(); uploadTarget = 0; apexSeen.clear(); overrideReadyAt = overloadReadyAt = 0; suspended = false; overrideLeftMs = overloadLeftMs = 0; blinds.clear(); ping = {}; }
}  // namespace chrome
