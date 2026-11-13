// cw_sound.inl - SOUND stream: play Cyberware SNDRs (Cyberware.esp core band + Cyberware - Sound.esp)
// through the game's own audio manager, so they follow the SFX/AMB category sliders instead of the
// UI slider that RE::UIUtils::PlayMenuSound uses. Included inside main.cpp's anonymous namespace.
//
// Resolution is BY EDITOR ID, the engine's own path (BSAudioManager::GetSoundHandleByName), same as
// every vanilla SNDR reference by name. T1 of the next cycle proves a MOD-ADDED SNDR resolves this
// way (uiSound op with CW_SND_UI_Select); until that boot, the 3D path is UNVERIFIED and every
// caller goes through snd::Play* which no-ops cleanly when the handle comes back invalid.
//
// Risky-defaults law: the 3D positioned path is new engine surface, so it ships behind
// [Sound] bSnd3D, OFF in Cyberware.ini and ON in Cyberware.Test.ini; when OFF, PlayAt falls back
// to the proven 2D PlayMenuSound so a cue is never silently lost. The 2D path reuses what the
// menu and abilities already do in play builds today.
namespace snd {
    // 0x1A: the usage flags UIUtils::PlayMenuSound passes to GetSoundHandleByName (decomp-derived,
    // UNVERIFIED for world sounds until the cycle boot; see sound/README.md "Wiring plan").
    constexpr std::uint32_t kUsage = 0x1A;

    inline bool Snd3D() { return g_ini.snd3d; }   // [Sound] bSnd3D (Settings above; test ini turns it on)

    inline void Play2D(const char* edid)
    {
        if (!edid || !*edid) return;
        RE::UIUtils::PlayMenuSound(edid);
    }

    // [Sound] fMasterDb: the static attenuation (hundredths of a dB) applied to a handle before it plays.
    // Handle path only: PlayMenuSound has no volume. 0 dB leaves each SNDR's own baked level untouched.
    inline void ApplyMaster(RE::BSSoundHandle& h)
    {
        if (g_ini.sndMasterDb < 0.0) h.SetStaticAttenuation((std::uint16_t)std::min(65000.0, -g_ini.sndMasterDb * 100.0));
    }
    inline void WarnOnce(const char* edid)
    {
        static std::set<std::string> warned;
        if (warned.insert(edid).second) REX::WARN("snd: no sound handle for '{}': the SNDR did not resolve", edid);
    }
    // Play through the audio manager (position optional). False when bSnd3D is off or the engine gives no handle.
    // keep: a caller that must stop this sound later gets its handle here.
    inline bool PlayHandle(const char* edid, const RE::NiPoint3* pos, RE::BSSoundHandle* keep = nullptr)
    {
        if (!Snd3D() || !edid || !*edid) return false;
        RE::BSSoundHandle h{};
        if (auto* am = RE::BSAudioManager::GetSingleton()) am->GetSoundHandleByName(h, edid, 0.0f, kUsage, nullptr);
        if (!h.IsValid()) { WarnOnce(edid); return false; }
        if (pos) h.SetPosition(*pos);
        ApplyMaster(h);
        const bool played = h.Play();
        if (played && keep) *keep = h;
        return played;
    }

    // A positioned one-shot (leap landing, slam impact). Falls back to 2D while bSnd3D is off or
    // the engine gives no handle, so the cue always sounds.
    inline void PlayAt(const char* edid, const RE::NiPoint3& pos)
    {
        if (!edid || !*edid) return;
        if (PlayHandle(edid, &pos)) return;
        if (Snd3D()) REX::INFO("snd: 3D handle for '{}' invalid, falling back to 2D", edid);
        RE::UIUtils::PlayMenuSound(edid);
    }

    // ---- The wired cue layer (sound/WIRING-SPEC.md). [Sound] bCues, OFF in Cyberware.ini, ON in the test ini.
    inline bool Cues() { return g_ini.sndCues; }

    // A cue that replaces a vanilla sound, or a new one. bCues off: only the vanilla fallback plays (the build behaves as
    // before this layer existed). bCues on: our SNDR, or the fallback if it does not resolve.
    inline void Cue(const char* edid, const char* vanilla = nullptr)
    {
        if (!Cues() || !edid || !*edid) { if (vanilla && *vanilla) RE::UIUtils::PlayMenuSound(vanilla); return; }
        if (PlayHandle(edid, nullptr)) return;
        RE::UIUtils::PlayMenuSound(vanilla && *vanilla ? vanilla : edid);
    }
    inline void CueAt(const char* edid, const RE::NiPoint3& pos)
    {
        if (!Cues() || !edid || !*edid) return;
        if (PlayHandle(edid, &pos)) return;
        RE::UIUtils::PlayMenuSound(edid);
    }

    // The PLS charge. Its wav is 3.5 s and builds up long after the launch, so it must end at the launch. The only cue
    // that keeps its handle: stopped on fire, or after kChargeMaxMs if the throw was interrupted. Skipped when there is
    // no handle, because PlayMenuSound could not stop it. Tick and the anim task can differ in thread.
    // kChargeMaxMs is the interrupt net only; a normal throw stops at weaponFire.2. The ARMS retimed 1P clips
    // (docs/integration/pls1p-retime-report.json) start the cue at 0.033 s and fire at 0.933 s (30 types) or 1.000 s
    // (7 heavy types), so the net is maxFire 1000 + 150 ms race margin (agreed with CYBERARM VISUALS).
    // REQUIRES the trimmed charge_loop.wav (sha256 09c3454a…, its 0.91-1.05 s transient capped at 0.45): with the
    // untrimmed wav (4c763cdd…, peak 0.895 at 1.00 s) this cap replays P10 S2's fire-loud interrupted throw. Everything
    // the net plus the 60 ms fade can reach (0-1.21 s of the wav) now peaks at or under 0.45, the pre-fire burst level.
    constexpr std::uint64_t kChargeMaxMs = 1150;
    constexpr std::uint16_t kChargeFadeMs = 60;
    inline std::mutex chargeLock;
    inline RE::BSSoundHandle chargeHandle{};
    inline std::uint64_t chargeUntil = 0;   // 0: nothing held
    inline void ChargeStopLocked()
    {
        if (chargeUntil && chargeHandle.IsValid()) chargeHandle.FadeOutAndRelease(kChargeFadeMs);
        chargeHandle = RE::BSSoundHandle{};
        chargeUntil = 0;
    }
    inline void ChargeStop() { std::lock_guard g(chargeLock); ChargeStopLocked(); }
    inline void ChargeStart(const char* edid)
    {
        std::lock_guard g(chargeLock);
        ChargeStopLocked();   // a second throw never stacks on the first
        if (Cues() && PlayHandle(edid, nullptr, &chargeHandle)) chargeUntil = GetTickCount64() + kChargeMaxMs;
    }
    inline void ChargeTick()
    {
        std::lock_guard g(chargeLock);
        if (chargeUntil && GetTickCount64() >= chargeUntil) ChargeStopLocked();
    }
    inline void ChargeForget()   // a load: the handle belongs to the old session
    {
        std::lock_guard g(chargeLock);
        chargeHandle = RE::BSSoundHandle{};
        chargeUntil = 0;
    }

    // A loop that follows a node (PLS charge). Caller keeps the handle and Stop()s it.
    inline RE::BSSoundHandle PlayFollow(const char* edid, const RE::NiAVObject* node)
    {
        RE::BSSoundHandle h{};
        if (!Snd3D() || !edid || !*edid) return h;
        if (auto* am = RE::BSAudioManager::GetSingleton()) {
            am->GetSoundHandleByName(h, edid, 0.0f, kUsage, nullptr);
            if (h.IsValid()) { if (node) h.SetObjectToFollow(node); ApplyMaster(h); h.Play(); }
        }
        return h;
    }
}  // namespace snd
