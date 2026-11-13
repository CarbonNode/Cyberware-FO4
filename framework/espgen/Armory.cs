// espgen `armory`: builds "Cyberware - Armory.esp" (ESL, its OWN id space, master Fallout4.esm only) + armory-formmap.json
// from data/armory/armory.json. The ARMORY stream's smart guns, iconic gear and consumables, sold by Dr. Varga.
//
//   armory --data armory.json --masters <dir with Fallout4.esm> --out <dir> [--lock armory-formids.lock.json]
//          [--cp cp_armory_stats.json] [--assets <staged mod root, to mark stockReady>] [--frozen]
//          [--sounds sounds.json]   SOUND slots with "plugin": "armory" become SNDRs here (a WEAP's "fireSound" points at one;
//                                   an optional "fireSound2d" points Attack2dSound at a second, hotter slot on its own)
//
// Never touches Cyberware.esp's records or ids. The DLL finds every form here by EditorID through armory-formmap.json, so
// the whole feature is inert when this plugin is not loaded. See ../../docs/ARMORY.md.
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Fallout4;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Binary.Parameters;
using Mutagen.Bethesda.Plugins.Records;
using Noggog;

static class Armory
{
    public const string PluginName = "Cyberware - Armory.esp";
    static readonly ModKey Me = ModKey.FromFileName(PluginName);
    const double UnitsPerMetre = 69.99;   // FO4 units per metre (ARMS measurement)

    static string Str(JsonNode o, string k, string def = "") => Gen.Str(o, k, def);
    static double Num(JsonNode o, string k, double def) => Gen.Num(o, k, def);
    static IEnumerable<JsonNode> Arr(JsonNode o, string k) => Gen.Arr(o, k);

    public static int Run(Args a)
    {
        string dataPath = a.Req("data"), outDir = a.Req("out"), mastersDir = a.Req("masters");
        string lockPath = a.Opt("lock") ?? Path.Combine(Path.GetDirectoryName(Path.GetFullPath(dataPath))!, "armory-formids.lock.json");
        byte[] bytes = File.ReadAllBytes(dataPath);
        string sha = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
        var data = JsonNode.Parse(bytes)!.AsObject();
        JsonObject? cp = a.Opt("cp") is string cpp ? JsonNode.Parse(File.ReadAllText(cpp))!.AsObject() : null;
        string? assets = a.Opt("assets");
        Directory.CreateDirectory(outDir);

        string esmPath = Path.Combine(mastersDir, "Fallout4.esm");
        if (!File.Exists(esmPath)) throw new GenError("no Fallout4.esm in " + mastersDir);
        using var fo4 = Plug.Read(esmPath);
        var fo4Mk = ModKey.FromFileName("Fallout4.esm");

        var lk = IdLock.Load(lockPath);
        lk.Frozen = a.Opt("frozen") == "true";
        var mod = new Fallout4Mod(Me, Fallout4Release.Fallout4);
        mod.ModHeader.Flags |= Fallout4ModHeader.HeaderFlag.Small;   // ESL
        mod.ModHeader.Author = "Cyberware ARMORY generator";
        mod.ModHeader.Description = "Generated from armory.json sha256 " + sha[..16] + ". Do not edit by hand.";
        var forms = new SortedDictionary<string, string>(StringComparer.Ordinal);
        FormKey K(string edid) { var id = lk.Get(edid); forms[edid] = id.ToString("X3"); return new FormKey(Me, id); }
        var report = new List<string>();

        // "Fallout4.esm|00ABCD" (checked id) or a bare EditorID (looked up, must exist)
        T Find<T>(IEnumerable<T> recs, string spec, string what) where T : class, IMajorRecordGetter
        {
            if (spec.Contains('|'))
            {
                var fk = Fk.Parse(spec);
                return recs.FirstOrDefault(r => r.FormKey == fk) ?? throw new GenError($"{what}: {spec} is not in Fallout4.esm (wrong record type?)");
            }
            return recs.FirstOrDefault(r => string.Equals(r.EditorID, spec, StringComparison.OrdinalIgnoreCase))
                   ?? throw new GenError($"{what}: EditorID '{spec}' is not in Fallout4.esm");
        }
        var avByEdid = fo4.ActorValueInformation.Where(r => r.EditorID != null).ToDictionary(r => r.EditorID!, r => r.FormKey, StringComparer.OrdinalIgnoreCase);

        // An asset path counts as staged only if the file exists under --assets (the staged mod root). Without --assets
        // nothing is stockReady: the shop never sells an item whose CP mesh is not on disk.
        bool Staged(string rel) => rel != "" && assets != null && File.Exists(Path.Combine(assets, "Meshes", rel));

        // --- keyword: the DLL's "this is a smart gun" test
        string smartKw = Str(data, "smartKeyword", "CW_AR_Smart");
        var kw = mod.Keywords.AddNew(K(smartKw)); kw.EditorID = smartKw;

        // --- smart projectiles: copies of MissileProjectileSeek (FO4's own seeking projectile)
        var projByEdid = new Dictionary<string, FormKey>();
        var projOut = new JsonArray();
        foreach (var p in Arr(data, "projectiles"))
        {
            string edid = Str(p, "edid");
            var t = Find(fo4.Projectiles, Str(p, "template"), "projectile " + edid);
            var np = (Projectile)t.Duplicate(K(edid));
            np.EditorID = edid;
            double mps = Num(p, "speedMps", 40);
            string from = Str(p, "cpSpeedFrom");
            if (from != "" && cp?["weapons"]?[from]?["stats"]?["SmartGunPlayerProjectileVelocity"]?["v"] is JsonValue jv && jv.TryGetValue<double>(out var cpv) && cpv > 0)
                mps = cpv;   // CP's own number wins over the authored fallback
            np.Speed = (float)(mps * UnitsPerMetre);
            np.Gravity = (float)Num(p, "gravity", 0);
            np.Range = (float)(Num(p, "rangeM", 60) * UnitsPerMetre);
            if (p["seeksAimVector"]?.GetValue<bool>() == true) np.Flags |= Projectile.Flag.SeeksTarget;
            if (p["noExplosion"]?.GetValue<bool>() == true) { np.Explosion.SetToNull(); np.Flags &= ~Projectile.Flag.Explosion; }
            // visualsFrom: "ammo:Fallout4.esm|01F276" = the 10mm's own projectile. The missile template's rocket model, launcher
            // muzzle flash, light and loop sound read as a flame along the slide with big orange bursts (QD, cw-int-4 build D):
            // keep the Missile type + SeeksTarget (the steering), take every visual and sound field from a pistol bullet.
            string vf = Str(p, "visualsFrom");
            IProjectileGetter? vis = null;
            if (vf.StartsWith("ammo:"))
            {
                var am = Find(fo4.Ammunitions, vf[5..], "visualsFrom of " + edid);
                vis = fo4.Projectiles.FirstOrDefault(x => x.FormKey == am.Projectile.FormKey)
                      ?? throw new GenError($"projectile {edid}: ammo {vf[5..]} has no projectile in Fallout4.esm");
            }
            else if (vf != "") vis = Find(fo4.Projectiles, vf, "visualsFrom of " + edid);
            if (vis != null)
            {
                np.Model = vis.Model?.DeepCopy();
                np.Light = new FormLink<ILightGetter>(vis.Light.FormKey);
                np.MuzzleFlash = new FormLink<ILightGetter>(vis.MuzzleFlash.FormKey);
                np.MuzzleFlashModel = vis.MuzzleFlashModel;
                np.MuzzleFlashDuration = vis.MuzzleFlashDuration;
                np.Sound = new FormLink<ISoundDescriptorGetter>(vis.Sound.FormKey);
                np.SoundLevel = vis.SoundLevel;
                np.TracerFrequency = vis.TracerFrequency;
                np.DecalData = new FormLink<ITextureSetGetter>(vis.DecalData.FormKey);
                np.CountdownSound = new FormLink<ISoundDescriptorGetter>(FormKey.Null);
                np.DisaleSound = new FormLink<ISoundDescriptorGetter>(FormKey.Null);
                string Vis(IProjectileGetter x) => $"model '{x.Model?.File}', muzzle '{x.MuzzleFlashModel}' {x.MuzzleFlashDuration:0.###}s, light {x.Light.FormKey}, flash {x.MuzzleFlash.FormKey}, sound {x.Sound.FormKey}, tracer {x.TracerFrequency}";
                report.Add($"armory: PROJ {edid} visuals: template {t.EditorID}: {Vis(t)}");
                report.Add($"armory: PROJ {edid} visuals: from {vis.EditorID} ({vis.FormKey}): {Vis(vis)} -> copied (type {np.Type}, flags {np.Flags} kept)");
            }
            // muzzleFlashModel: the smart round's own small flash (a scaled copy of the 10mm's, armory/nif/build_smart_muzzle.py)
            if (Str(p, "muzzleFlashModel") is string mf && mf != "") np.MuzzleFlashModel = mf;
            string model = Str(p, "model");
            if (model != "") np.Model = new Model { File = model };
            // noTracer: a bullet tracer on a Missile-type round draws one frame from the muzzle to the screen edge, away from the target (QD build E)
            if (p["noTracer"]?.GetValue<bool>() == true) np.TracerFrequency = default;
            mod.Projectiles.Add(np);
            projByEdid[edid] = np.FormKey;
            projOut.Add(edid);
            report.Add($"armory: PROJ {edid} from {t.EditorID} ({t.FormKey}) speed {mps:0.#} m/s = {np.Speed:0} u/s, range {np.Range:0} u, flags {np.Flags}");
        }

        // --- weapons
        // --- SOUND slots hosted here (the lead, 2026-11-09: CP's own Yukimura fire sound). data/sound/sounds.json stays SOUND's
        // file; a slot with "plugin": "armory" becomes an SNDR in THIS plugin (own lock), because the WEAP that plays it lives
        // here and Armory masters only Fallout4.esm. A slot with no cut files yet makes no record, and the WEAP keeps its
        // template's sound, so this is inert until SOUND delivers.
        var sndrByEdid = new Dictionary<string, FormKey>(StringComparer.Ordinal);
        if (a.Opt("sounds") is string sndPath && File.Exists(sndPath))
        {
            var snd = JsonNode.Parse(File.ReadAllText(sndPath))!.AsObject();
            var scat = fo4.SoundCategories.Where(r => r.EditorID != null).ToDictionary(r => r.EditorID!, r => r.FormKey, StringComparer.OrdinalIgnoreCase);
            var sopm = fo4.SoundOutputModels.Where(r => r.EditorID != null).ToDictionary(r => r.EditorID!, r => r.FormKey, StringComparer.OrdinalIgnoreCase);
            foreach (var slot in Arr(snd, "slots"))
            {
                if (Str(slot, "plugin") != "armory" || Str(slot, "status") == "dropped") continue;
                string se = Str(slot, "id");
                if (!se.StartsWith("CW_SND_")) throw new GenError($"sounds: armory slot id '{se}' must start with CW_SND_");
                if (!Arr(slot, "files").Any()) { report.Add($"armory: SNDR {se} skipped (slot {Str(slot, "slot")} has no cut files yet)"); continue; }
                var r = mod.SoundDescriptors.AddNew(K(se));
                r.EditorID = se;
                Sndr.Fill(r, slot, scat, sopm);
                sndrByEdid[se] = r.FormKey;
                report.Add($"armory: SNDR {se} ({r.SoundFiles.Count} file(s)) from slot {Str(slot, "slot")}");
            }
        }

        // --- item text (the lead's ruling 2026-11-10, same shape as the cyberware card):
        //   "description" = what the item DOES, plain mechanical words.   "lore" = the flavour line, optional.
        //   "desc" = the older single mixed string, kept as a FALLBACK until RIPPERDOC LORE has split the 11 existing
        //            strings and written Skippy's. That editorial split is theirs, not ours.
        // The record's Description (what the Pip-Boy and vanilla item card show) is description then lore when the two
        // together fit the ceiling, else description alone - never a truncated sentence.
        const int kDescMax = 160;   // no engine limit; the shop's detail pane is a short paragraph and the existing strings run 60-134
        static (string description, string lore, string record) ItemText(JsonNode e)
        {
            string description = Str(e, "description"), lore = Str(e, "lore");
            if (description == "") description = Str(e, "desc");   // fallback: the old mixed single string
            // MENU/HUD AAA's invariant (2026-11-10). Their DLL reads
            //     e.value("description", e.value("desc", ""))
            // which falls back to `desc` only when the description KEY IS ABSENT - an EMPTY "description" beside a
            // non-empty `desc` reaches the page blank. The fallback above means that cannot happen today, because
            // `description` is only empty when `desc` is empty too. This assert is here so it cannot start happening:
            // it is a one-line invariant holding a cross-component contract that is invisible from either side alone.
            if (description == "" && Str(e, "desc") != "")
                throw new GenError($"armory: {Str(e, "edid")} would emit an EMPTY description beside a non-empty desc. "
                                 + "MENU's page falls back to desc only when the description KEY is absent, so this row "
                                 + "would render blank. Either give it a real description or remove the empty one.");
            string record = description;
            if (lore != "" && description != "" && description.Length + 1 + lore.Length <= kDescMax) record = description + " " + lore;
            else if (description == "") record = lore;
            return (description, lore, record);
        }

        var weaponsOut = new JsonObject();
        var stock = new JsonArray();
        foreach (var w in Arr(data, "weapons"))
        {
            string edid = Str(w, "edid");
            // copyOf: an earlier Armory WEAP (an iconic variant of a base gun keeps its mesh, projectile and keywords)
            IWeaponGetter t = Str(w, "copyOf") is string co && co != ""
                ? mod.Weapons.FirstOrDefault(x => x.EditorID == co) ?? throw new GenError($"weapon {edid}: copyOf {co} is not an earlier Armory weapon")
                : Find(fo4.Weapons, Str(w, "template"), "weapon " + edid);
            var nw = (Weapon)t.Duplicate(K(edid));
            nw.EditorID = edid;
            nw.Name = Str(w, "name", edid);
            // Item text: ONE store, in armory.json beside the item (LORE chose the store; the strings are theirs).
            var wt = ItemText(w);
            if (wt.record != "") nw.Description = wt.record;
            nw.Value = (uint)Num(w, "value", nw.Value);
            nw.Weight = (float)Num(w, "weight", nw.Weight);
            if (w["damage"] != null) nw.BaseDamage = (ushort)Num(w, "damage", nw.BaseDamage);
            if (Str(w, "ammo") != "") nw.Ammo = new FormLinkNullable<IAmmunitionGetter>(Find(fo4.Ammunitions, Str(w, "ammo"), "ammo of " + edid).FormKey);
            if (w["stripTemplateMods"]?.GetValue<bool>() == true)
            {
                // the template's default OMODs would bolt 10mm receivers / barrels onto our own single-mesh gun
                nw.ObjectTemplates = null;
                nw.AttachParentSlots = null;
                nw.EmbeddedWeaponMod.SetToNull();
            }
            string pe = Str(w, "projectile");
            if (pe != "")
            {
                if (!projByEdid.TryGetValue(pe, out var pk)) throw new GenError($"weapon {edid}: projectile {pe} is not in armory.json");
                if (nw.ExtraData == null) throw new GenError($"weapon {edid}: template has no extra data for a projectile override");
                nw.ExtraData.ProjectileOverride = new FormLinkNullable<IProjectileGetter>(pk);
            }
            nw.Keywords ??= new();
            if (w["smart"]?.GetValue<bool>() == true && !nw.Keywords.Any(k => k.FormKey == kw.FormKey)) nw.Keywords.Add(new FormLink<IKeywordGetter>(kw.FormKey));
            // materialSwap: {"Cyberware\\Armory\\Yukimura.bgsm": "Cyberware\\Armory\\YukimuraSkippy.bgsm"} (paths relative to Materials\\)
            if (w["materialSwap"] is JsonObject ms && ms.Count > 0)
            {
                string me = "CW_AR_MSWP_" + edid;
                var sw = mod.MaterialSwaps.AddNew(K(me));
                sw.EditorID = me;
                foreach (var (o, r) in ms)
                    sw.Substitutions.Add(new MaterialSubstitution { OriginalMaterial = o, ReplacementMaterial = r!.GetValue<string>() });
                nw.Model ??= new Model { File = Str(w, "model") };
                nw.Model.MaterialSwap = new FormLinkNullable<IMaterialSwapGetter>(sw.FormKey);
                report.Add($"armory: MSWP {me} ({ms.Count} substitution(s)) on {edid}");
            }
            string model = Str(w, "model");
            if (model != "") { nw.Model = new Model { File = model }; nw.FirstPersonModel = null; }
            // fireSound: a SOUND slot hosted above. The same SNDR for the 3D (NPC / third person) and 2D (player) attack sound.
            if (Str(w, "fireSound") is string fs && fs != "")
            {
                if (sndrByEdid.TryGetValue(fs, out var sk))
                {
                    nw.AttackSound.SetTo(sk);
                    nw.Attack2dSound.SetTo(sk);
                    report.Add($"armory: {edid} fire sound -> {fs}");
                }
                else report.Add($"armory: {edid} fire sound {fs} not built yet (no armory SNDR); the template's sound stays");
            }
            // fireSound2d: READY-TO-APPLY, inert until SOUND cuts the slot (the lead + SOUND, 2026-11-10).
            // Vanilla gives a gun two fire assets about 6 dB apart, 3D for NPCs/third person and 2D for the player's own
            // shot. CP's Yukimura source cannot reach vanilla's 2D level: it is a peaky transient against a dense,
            // pre-limited vanilla asset, so gain alone would need +15.1 dB and a peak of 4.5. At the +9 dB soft limiting
            // SOUND shipped it matches vanilla's NPC 3D layer exactly but sits 5.5 dB under WPN_Pistol10mm_Fire_2D, so the
            // player's own shot reads slightly soft. If QD fails that, SOUND adds ONE hotter slot and this points
            // Attack2dSound at it alone, leaving AttackSound on the 3D cut - no code change needed then, which is the point.
            // While the slot has no files, Attack2dSound keeps whatever `fireSound` set, i.e. exactly today's behaviour.
            if (Str(w, "fireSound2d") is string fs2 && fs2 != "")
            {
                if (sndrByEdid.TryGetValue(fs2, out var sk2))
                {
                    nw.Attack2dSound.SetTo(sk2);
                    // THE SPLIT MUST BE REAL. If both attack fields end up on one SNDR, the player's own shot plays the 3D
                    // cut and QD's clean-versus-louder A/B compares a file with itself - it would pass while measuring
                    // nothing, and nobody would see it in a log (the lead + SOUND, 2026-11-10). A weapon
                    // that DECLARES a 2D slot and still resolves both fields to the same record is a build failure.
                    if (nw.AttackSound.FormKey == nw.Attack2dSound.FormKey)
                        throw new GenError($"armory: {edid} declares fireSound2d '{fs2}' but AttackSound and Attack2dSound both "
                                         + $"resolve to {nw.AttackSound.FormKey} - the 2D/3D split is not real, so the player's own "
                                         + $"shot would play the 3D cut. Check that fireSound and fireSound2d name DIFFERENT slots.");
                    report.Add($"armory: {edid} 2D fire sound -> {fs2} ({sk2}) (3D stays on {Str(w, "fireSound")} ({nw.AttackSound.FormKey}))");
                }
                else report.Add($"armory: {edid} 2D fire sound {fs2} not built yet (no armory SNDR); Attack2dSound stays on the 3D cut");
            }
            mod.Weapons.Add(nw);
            bool ready = Staged(model != "" ? model : (nw.Model?.File ?? ""));
            weaponsOut[edid] = new JsonObject
            {
                ["cp"] = Str(w, "cp"), ["template"] = $"{t.FormKey}", ["smart"] = w["smart"]?.GetValue<bool>() == true,
                ["projectile"] = pe, ["model"] = model == "" ? null : model, ["modelTarget"] = Str(w, "modelTarget"), ["stockReady"] = ready,
            };
            if (w["price"] != null) stock.Add(new JsonObject { ["edid"] = edid, ["kind"] = "weapon", ["name"] = nw.Name?.String, ["price"] = (int)Num(w, "price", 0), ["doc"] = Str(w, "doc", "diamondcity"), ["stockReady"] = ready, ["description"] = wt.description, ["lore"] = wt.lore, ["desc"] = Str(w, "desc") });
            report.Add($"armory: WEAP {edid} '{nw.Name}' from {t.EditorID} ({t.FormKey}), damage {nw.BaseDamage}, projectile {(pe == "" ? "template" : pe)}, model {(model != "" ? model : Str(w, "copyOf") != "" ? "inherited " + nw.Model?.File : "TEMPLATE")}{(ready ? " (staged: stock-ready)" : " (not stock-ready)")}");
        }

        // --- consumables: copies of the vanilla item they mirror (keeps its Survival keywords, sounds and use animation)
        var mgef = new Dictionary<(string av, string kind), FormKey>();
        FormKey Mgef(string av, string kind)
        {
            if (mgef.TryGetValue((av, kind), out var fk)) return fk;
            if (!avByEdid.TryGetValue(av, out var avKey)) throw new GenError($"actor value '{av}' is not an AVIF EditorID in Fallout4.esm");
            string e = $"CW_AR_FX_{(kind == "restore" ? "Restore" : "Fortify")}{av}";
            var m = mod.MagicEffects.AddNew(K(e));
            m.EditorID = e;
            m.Name = (kind == "restore" ? "Restore " : "Fortify ") + av;
            // fortify: Recover (the change goes away when the effect ends). restore: no Recover (damage healed stays healed).
            m.Flags = (kind == "restore" ? 0 : MagicEffect.Flag.Recover) | MagicEffect.Flag.NoArea;
            m.Archetype = new MagicEffectArchetype(MagicEffectArchetype.TypeEnum.ValueModifier) { ActorValue = new FormLink<IActorValueInformationGetter>(avKey) };
            m.CastType = CastType.FireAndForget;
            m.TargetType = TargetType.Self;
            m.MagicSkill.SetToNull();
            m.ResistValue.SetToNull();
            mgef[(av, kind)] = m.FormKey;
            return m.FormKey;
        }
        var consOut = new JsonObject();
        FormKey markerFx = FormKey.Null;
        foreach (var c in Arr(data, "consumables"))
        {
            string edid = Str(c, "edid");
            var t = Find(fo4.Ingestibles, Str(c, "template"), "consumable " + edid);
            var ni = (Ingestible)t.Duplicate(K(edid));
            ni.EditorID = edid;
            ni.Name = Str(c, "name", edid);
            var ct = ItemText(c);
            if (ct.record != "") ni.Description = ct.record;   // see ItemText above: description + lore, or description alone
            ni.Value = (uint)Num(c, "value", ni.Value);
            string model = Str(c, "model");
            if (model != "") ni.Model = new Model { File = model };
            string mode = c["effects"] is JsonValue ev ? ev.GetValue<string>() : "list";
            if (mode == "template-instant")
            {
                // the template's effects, delivered at once: magnitude x duration x scale, duration 0
                double scale = Num(c, "scale", 1);
                string drop = Str(c, "dropTemplateEffects");   // e.g. Stimpak's RestoreRadsCompanion (companion-only, 200)
                if (drop != "")
                    ni.Effects.RemoveAll(e => fo4.MagicEffects.FirstOrDefault(m => m.FormKey == e.BaseEffect.FormKey)?.EditorID?.Contains(drop, StringComparison.OrdinalIgnoreCase) == true);
                foreach (var e in ni.Effects)
                    if (e.Data != null) { e.Data.Magnitude = (float)(e.Data.Magnitude * Math.Max(1, e.Data.Duration) * scale); e.Data.Duration = 0; }
            }
            else if (mode == "list")
            {
                // keepTemplateEffects: "rads" / "addiction" keeps the template's effects whose MGEF EditorID mentions it
                string keep = Str(c, "keepTemplateEffects");
                var kept = ni.Effects.Where(e => keep != "" && fo4.MagicEffects.FirstOrDefault(m => m.FormKey == e.BaseEffect.FormKey)?.EditorID?.Contains(keep, StringComparison.OrdinalIgnoreCase) == true).ToList();
                ni.Effects.Clear();
                ni.Effects.AddRange(kept);
                foreach (var ef in Arr(c, "effects"))
                {
                    string av = Str(ef, "av"), kind = Str(ef, "kind", "fortify");
                    double mag = Num(ef, "mag", 0);
                    var fx = Mgef(av, mag < 0 && kind == "fortify" ? "fortify" : kind);
                    ni.Effects.Add(new Effect { BaseEffect = new FormLinkNullable<IMagicEffectGetter>(fx), Data = new EffectData { Magnitude = (float)mag, Area = 0, Duration = (int)Num(ef, "dur", 0) } });
                }
            }
            // native consumables (RAM): a hidden zero-magnitude marker effect, so the item leaves an ActiveEffect whose
            // `source` is this ALCH; the DLL sees it (4 Hz poll, one action per uniqueID) and applies the native.
            if (Str(c, "native") != "")
            {
                if (markerFx.IsNull)
                {
                    var m = mod.MagicEffects.AddNew(K("CW_AR_FX_NativeMarker"));
                    m.EditorID = "CW_AR_FX_NativeMarker";
                    m.Name = "Cyberware: armory consumable";
                    m.Flags = MagicEffect.Flag.Recover | MagicEffect.Flag.HideInUI | MagicEffect.Flag.NoArea | MagicEffect.Flag.Painless | MagicEffect.Flag.NoHitEvent;
                    m.Archetype = new MagicEffectArchetype(MagicEffectArchetype.TypeEnum.ValueModifier) { ActorValue = new FormLink<IActorValueInformationGetter>(avByEdid["Health"]) };
                    m.CastType = CastType.FireAndForget;
                    m.TargetType = TargetType.Self;
                    m.MagicSkill.SetToNull();
                    m.ResistValue.SetToNull();
                    markerFx = m.FormKey;
                }
                ni.Effects.Add(new Effect { BaseEffect = new FormLinkNullable<IMagicEffectGetter>(markerFx), Data = new EffectData { Magnitude = 0, Area = 0, Duration = (int)Num(c, "nativeDur", 1) } });
            }
            // mode "template": the vanilla item's own effects unchanged
            mod.Ingestibles.Add(ni);
            bool ready = Staged(model);
            consOut[edid] = new JsonObject { ["cp"] = Str(c, "cp"), ["template"] = $"{t.FormKey}", ["native"] = Str(c, "native") == "" ? null : Str(c, "native"), ["nativeDur"] = Num(c, "nativeDur", 1), ["stockReady"] = ready };
            if (c["price"] != null) stock.Add(new JsonObject { ["edid"] = edid, ["kind"] = "consumable", ["name"] = ni.Name?.String, ["price"] = (int)Num(c, "price", 0), ["doc"] = Str(c, "doc", "diamondcity"), ["stockReady"] = ready, ["description"] = ct.description, ["lore"] = ct.lore, ["desc"] = Str(c, "desc") });
            report.Add($"armory: ALCH {edid} '{ni.Name}' from {t.EditorID} ({t.FormKey}), {ni.Effects.Count} effect(s){(Str(c, "native") != "" ? " + native " + Str(c, "native") : "")}, model {(model == "" ? "TEMPLATE (not stock-ready)" : model)}");
        }

        // --- smart parameters for the DLL: CP's SmartGun* stats per smart weapon, prefix dropped (cw_smart reads these names)
        var smartW = new JsonObject();
        foreach (var w in Arr(data, "weapons").Where(x => x["smart"]?.GetValue<bool>() == true))
        {
            var o = new JsonObject { ["projectile"] = Str(w, "projectile") };
            // smartCp: whose CP stats to use (a preset can name its base: extract_cp.py double-counts stat groups a preset
            // re-lists, e.g. Skippy's lock time read 2.4 s against the Yukimura's 1.2 s)
            string src = Str(w, "smartCp", Str(w, "cp"));
            if (cp?["weapons"]?[src]?["stats"] is JsonObject st)
                foreach (var (k, v) in st)
                {
                    if (!k.StartsWith("SmartGun") || v?["v"] is not JsonValue sv || !sv.TryGetValue<double>(out var d)) continue;
                    // a stat CP computes at runtime (dependsOn Quality etc.) has no usable literal: a 0 here would zero the
                    // gun (cw-int-4 C2: TargetAcquisitionRange 0 -> no target ever in range). Leave it to the DLL default.
                    string dep = v?["dependsOn"] is JsonValue dv ? dv.GetValue<string>() : "";
                    if (d == 0 && dep != "" && dep != "HasSmartLink") continue;
                    o[k["SmartGun".Length..]] = d;
                }
            smartW[Str(w, "edid")] = o;
        }

        // --- write + verify
        string espPath = Path.Combine(outDir, PluginName);
        var needed = mod.EnumerateFormLinks().Select(l => l.FormKey.ModKey).Concat(mod.EnumerateMajorRecords().Select(r => r.FormKey.ModKey))
            .Where(k => k != Me && !k.IsNull).Distinct().ToList();
        if (needed.Any(k => k != fo4Mk)) throw new GenError("the Armory plugin must master Fallout4.esm only; it references " + string.Join(", ", needed.Where(k => k != fo4Mk)));
        mod.WriteToBinary(espPath, new BinaryWriteParameters { MastersListOrdering = new MastersListOrderingByLoadOrder(new[] { fo4Mk }) });
        using (var back = Fallout4Mod.CreateFromBinaryOverlay(espPath, Fallout4Release.Fallout4))
        {
            int n = back.EnumerateMajorRecords().Count();
            if (!back.ModHeader.Flags.HasFlag(Fallout4ModHeader.HeaderFlag.Small)) throw new GenError("written plugin is not ESL-flagged");
            var ids = back.EnumerateMajorRecords().Where(r => r.FormKey.ModKey == Me).Select(r => r.FormKey.ID).ToList();
            if (ids.Any(i => i < IdLock.Min || i > IdLock.Max)) throw new GenError("a record is outside 0x800-0xFFF");
            if (ids.Count != n) throw new GenError("the Armory plugin must not override any record");
            report.Insert(0, $"wrote {espPath}: ESL, {n} records, masters [{string.Join(", ", back.ModHeader.MasterReferences.Select(m => m.Master.FileName))}]");
        }
        if (!lk.Frozen) lk.Save(lockPath, "Cyberware - Armory.esp FormID lock. Commit it. Never hand-edit an id that has shipped.");

        var map = new JsonObject
        {
            ["plugin"] = PluginName,
            ["dataSha256"] = sha,
            ["generator"] = "CyberwareEspGen armory 0.1.0",
            ["forms"] = new JsonObject(forms.Select(k => KeyValuePair.Create(k.Key, (JsonNode?)JsonValue.Create(k.Value)))),
            ["smart"] = new JsonObject { ["keyword"] = smartKw, ["projectiles"] = projOut, ["weapons"] = smartW },
            ["weapons"] = weaponsOut,
            ["consumables"] = consOut,
            ["stock"] = stock,
        };
        File.WriteAllText(Path.Combine(outDir, "armory-formmap.json"), map.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");
        foreach (var r in report) Console.WriteLine(r);
        Console.WriteLine($"armory-formmap: {forms.Count} EditorIDs; stock {stock.Count} ({stock.Count(s => s!["stockReady"]!.GetValue<bool>())} stock-ready); lock: {lockPath}");
        return 0;
    }
}
