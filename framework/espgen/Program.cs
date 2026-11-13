// CyberwareEspGen - builds Cyberware.esp (ESL-flagged) + formmap.json from cyberware.json. Reproducible: the same
// catalog + lock file + masters give the same FormIDs. Commands:
//
//   gen    --catalog cyberware.json --masters <dir with Fallout4.esm> --out <dir> [--lock formids.lock.json]
//          [--subgraphs subgraphs.json --subgraph-base <plugin holding the WINNING HumanRaceSubGraphData>]
//          [--placement ripperdoc-placement.json --cell-base "Fallout4.esm|022884=<plugin>;..."]
//          [--door-unlock "Fallout4.esm|0537C3=<plugin with its winning copy>;..."]   (placementExtra.unlockDoors)
//          [--arms weapons_arms.json]   (ARMS WEAPs, the visual blade ARMO+ARMA, the leap actor keyword)
//          [--sounds sounds.json]       (SOUND: one SNDR per slot, FormID band 0xFE0-0xFFF; see ../../sound/README.md)
//          --native native-effects.json  (cwtest --native-effects: every CW_Nat perk's effects + CW_ChromeCrit; required
//                                          when the catalog has native abilities)
//          [--strings <dir with Fallout4_en.STRINGS/.DLSTRINGS/.ILSTRINGS>]   (needed for CELL overrides; any command)
//          [--load-order plugins.txt] [--frozen]   (--frozen: refuse to allocate FormIDs the committed lock lacks)
//   winner --profile <MO2 profile dir> --mods <MO2 mods dir> --data <Stock Game Data dir> --form "Fallout4.esm|166729;Fallout4.esm|022884"
//          [--write file]  prints (and writes) the plugin file that wins that record in the profile's load order
//   armory --data armory.json --masters <dir> --out <dir> [--lock armory-formids.lock.json] [--cp cp_armory_stats.json]
//          [--assets <staged mod root>] [--frozen]   builds "Cyberware - Armory.esp" (own ESL, own lock): see Armory.cs
//   voices --manifest ../data/ripperdoc-voice.json --out <dir> [--lock voices.formids.lock.json] [--frozen]
//          builds "Cyberware - Voices.esp" (ESL) + voices-formmap.json: one Say-able topic per voiced line (Voices.cs)
//   probe  --esm <plugin> --type AVIF|RACE|NPC_|ACTI|STAT|FURN|PERK [--match text]   lists EditorIDs + FormIDs
//
// Record layout and naming are documented in ../README.md and ../../docs/CATALOG-SCHEMA.md. EditorIDs are the
// contract with Cyberware.dll (it resolves them through formmap.json); never rename one that has shipped.
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Fallout4;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Binary.Parameters;
using Mutagen.Bethesda.Plugins.Records;
using Noggog;

static class Program
{
    static int Main(string[] argv)
    {
        try
        {
            if (argv.Length == 0) { Usage(); return 2; }
            var a = Args.Parse(argv.Skip(1).ToArray());
            Plug.Strings = a.Opt("strings");
            int rc = argv[0] switch
            {
                "gen" => Gen.Run(a),
                "winner" => Winner.Run(a),
                "probe" => Probe.Run(a),
                "armory" => Armory.Run(a),
                "voices" => Voices.Run(a),
                _ => Fail("unknown command " + argv[0]),
            };
            if (rc == 0) a.CheckAllRead(argv[0]);   // a command that succeeded must have read every option it was given
            return rc;
        }
        catch (GenError e) { Console.Error.WriteLine("ERROR: " + e.Message); return 1; }
    }
    static int Fail(string m) { Console.Error.WriteLine(m); Usage(); return 2; }
    static void Usage() => Console.Error.WriteLine("usage: CyberwareEspGen gen|winner|probe|armory|voices ... (see Program.cs header)");
}

class GenError(string m) : Exception(m);

// Every plugin espgen READS goes through here. --strings <dir> holds the vanilla string tables (Fallout4_en.STRINGS,
// .DLSTRINGS, .ILSTRINGS, extracted from "Fallout4 - Interface.ba2" by tools/ba2x.py). Fallout4.esm is localized:
// without them every lstring (a CELL's FULL name) reads as empty and an override copies that empty name into the
// non-localized Cyberware.esp, blanking the cell name in game.
static class Plug
{
    public static string? Strings;
    // Imagespace numbers are in the engine's own units (double vision is a screen fraction, not a 0-2 strength), and a
    // wrong one can blank the whole view (the Sandevistan loop's 1.5, QD blocks 24/25). So every scalar keyframe curve of
    // an imagespace we generate must stay inside the range Fallout4.esm itself uses for that field, across all of its
    // imagespaces. A copy of a vanilla record passes by construction; only a value WE set can fail. It covers every
    // curve the record has (blur, double vision, radial blur, depth of field, motion blur, HDR, cinematic), so a field
    // a later change starts setting is checked without anyone remembering to. Tint and fade colours are not scalar and
    // are not checked. To go past vanilla on purpose, prove the look in game first and widen it here, by field name.
    public static void CheckImadWithinVanilla(IFallout4ModGetter fo4, IImageSpaceAdapterGetter c)
    {
        foreach (var prop in typeof(IImageSpaceAdapterGetter).GetProperties())
        {
            if (prop.GetValue(c) is not IReadOnlyList<IKeyFrameGetter> ours || ours.Count == 0) continue;
            float lo = 0f, hi = 0f;   // a field vanilla never keys is 0 everywhere
            foreach (var v in fo4.ImageSpaceAdapters)
                if (prop.GetValue(v) is IReadOnlyList<IKeyFrameGetter> keys)
                    foreach (var k in keys) { lo = Math.Min(lo, k.Value); hi = Math.Max(hi, k.Value); }
            foreach (var k in ours)
                if (k.Value < lo - 1e-6f || k.Value > hi + 1e-6f)
                    throw new GenError($"{c.EditorID}: imagespace {prop.Name} = {k.Value} is outside vanilla's range [{lo}, {hi}] (Fallout4.esm, all imagespaces). Engine units differ per field; see docs/LESSONS.md");
        }
    }
    public static IFallout4ModDisposableGetter Read(string path)
    {
        var b = Fallout4Mod.Create(Fallout4Release.Fallout4).FromPath(path);
        if (Strings != null) b = b.WithStringsFolder(Strings);
        return b.Construct();
    }
}

class Args
{
    readonly Dictionary<string, string> _v = new();
    readonly HashSet<string> _read = new(StringComparer.Ordinal);   // every option the command actually asked for
    public static Args Parse(string[] a)
    {
        var r = new Args();
        for (int i = 0; i < a.Length; i++)
        {
            if (!a[i].StartsWith("--")) throw new GenError("unexpected argument " + a[i]);
            var k = a[i][2..];
            if (k.Length == 0) throw new GenError("empty option name '--'");
            if (r._v.ContainsKey(k)) throw new GenError($"option --{k} given twice");
            r._v[k] = i + 1 < a.Length && !a[i + 1].StartsWith("--") ? a[++i] : "true";
        }
        return r;
    }
    public string Req(string k) { _read.Add(k); return _v.TryGetValue(k, out var v) ? v : throw new GenError("missing --" + k); }
    public string? Opt(string k) { _read.Add(k); return _v.TryGetValue(k, out var v) ? v : null; }

    // An UNKNOWN OPTION is an option the command never read. Why it is checked this way (HARNESS + the lead, 2026-11-10):
    // the parser used to accept any --flag silently, so `armory --sounds <file>` on a build whose Armory.cs had no --sounds
    // option ran as plain armory and said nothing - the sound simply never got wired, with a clean exit 0. A typo
    // (--sound, --soudns) and a flag meant for a different subcommand both failed the same silent way.
    // This is deliberately derived from what the code READ rather than from a hand-written list of valid flags per
    // command: a hand-written list can go stale and then rejects a legitimate flag, breaking a rig build, whereas a
    // legitimate flag is by definition read somewhere. The cost is that it fires after the command has done its work;
    // that is safe because the non-zero exit fails the caller (build-rig's Run throws on a non-zero exit code), so no
    // half-configured plugin is ever used, and the message names the flag.
    public void CheckAllRead(string cmd)
    {
        var unknown = _v.Keys.Where(k => !_read.Contains(k)).OrderBy(k => k, StringComparer.Ordinal).ToList();
        if (unknown.Count == 0) return;
        throw new GenError($"unknown option(s) for '{cmd}': {string.Join(", ", unknown.Select(k => "--" + k))}. "
                         + $"'{cmd}' never read {(unknown.Count == 1 ? "it" : "them")}, so it would have been ignored silently. "
                         + "Check the spelling, and that the option belongs to this subcommand (see the Program.cs header).");
    }
}

static class Fk
{
    // "Fallout4.esm|00ABCD" -> FormKey
    public static FormKey Parse(string s)
    {
        var bar = s.IndexOf('|');
        if (bar < 0) throw new GenError($"form key '{s}' must look like Plugin.esm|00ABCD");
        return new FormKey(ModKey.FromFileName(s[..bar]), Convert.ToUInt32(s[(bar + 1)..], 16) & 0xFFFFFF);
    }
}

// ------------------------------------------------------------------------------------------------ FormID lock
// EditorID -> local id (0x800..0xFFF, the FO4 ESL range). Kept in git so regenerating never renumbers a record,
// which would orphan it in every save. Removed records keep their number reserved ("retired").
class IdLock
{
    public Dictionary<string, uint> Ids = new();
    public Dictionary<string, uint> Retired = new();
    readonly HashSet<string> _used = new();
    public const uint Min = 0x800, Max = 0xFFF;

    public static IdLock Load(string? path)
    {
        var l = new IdLock();
        if (path == null || !File.Exists(path)) return l;
        var j = JsonNode.Parse(File.ReadAllText(path))!;
        foreach (var (k, v) in j["ids"]!.AsObject()) l.Ids[k] = Convert.ToUInt32(v!.GetValue<string>(), 16);
        if (j["retired"] is JsonObject r) foreach (var (k, v) in r) l.Retired[k] = Convert.ToUInt32(v!.GetValue<string>(), 16);
        return l;
    }
    public bool Frozen;
    public uint Get(string edid) => Get(edid, Min, Max);

    // Ranged allocation: crews own FormID bands (SOUND: 0xFE0-0xFFF). A record already in the
    // lock keeps its id even outside the band (never renumber a shipped record).
    public uint Get(string edid, uint min, uint max)
    {
        _used.Add(edid);
        if (Ids.TryGetValue(edid, out var id)) return id;
        if (Frozen) throw new GenError($"--frozen: {edid} has no FormID in the lock file. Regenerate the lock from the repo (Linux/dev box), commit it, then rebuild.");
        if (Retired.TryGetValue(edid, out id) && id >= min && id <= max) { Retired.Remove(edid); Ids[edid] = id; return id; }
        var taken = Ids.Values.Concat(Retired.Values).ToHashSet();
        for (uint c = min; c <= max; c++)
            if (!taken.Contains(c)) { Ids[edid] = c; return c; }
        throw new GenError(min == Min && max == Max
            ? "ESL FormID range 0x800-0xFFF is full (2048 records): lower stat bits or split the plugin"
            : $"FormID band 0x{min:X3}-0x{max:X3} is full for {edid}: widen the band or merge records");
    }
    public void Save(string path, string comment = "Cyberware.esp FormID lock. Commit it. Never hand-edit an id that has shipped.")
    {
        foreach (var k in Ids.Keys.Where(k => !_used.Contains(k)).ToList()) { Retired[k] = Ids[k]; Ids.Remove(k); }
        var j = new JsonObject
        {
            ["_comment"] = comment,
            ["ids"] = new JsonObject(Ids.OrderBy(k => k.Value).Select(k => KeyValuePair.Create(k.Key, (JsonNode?)JsonValue.Create(k.Value.ToString("X3"))))),
            ["retired"] = new JsonObject(Retired.OrderBy(k => k.Value).Select(k => KeyValuePair.Create(k.Key, (JsonNode?)JsonValue.Create(k.Value.ToString("X3"))))),
        };
        File.WriteAllText(path, j.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");
    }
}

// ------------------------------------------------------------------------------------------------ gen
static class Gen
{
    static readonly ModKey Me = ModKey.FromFileName("Cyberware.esp");

    public static int Run(Args a)
    {
        string catPath = a.Req("catalog"), outDir = a.Req("out"), mastersDir = a.Req("masters");
        string lockPath = a.Opt("lock") ?? Path.Combine(Path.GetDirectoryName(Path.GetFullPath(catPath))!, "formids.lock.json");
        byte[] catBytes = File.ReadAllBytes(catPath);
        string catSha = Convert.ToHexString(SHA256.HashData(catBytes)).ToLowerInvariant();
        var cat = JsonNode.Parse(catBytes)!.AsObject();
        Directory.CreateDirectory(outDir);

        string esmPath = Path.Combine(mastersDir, "Fallout4.esm");
        if (!File.Exists(esmPath)) throw new GenError("no Fallout4.esm in " + mastersDir);
        using var fo4 = Plug.Read(esmPath);
        var avByEdid = fo4.ActorValueInformation.Where(r => r.EditorID != null).ToDictionary(r => r.EditorID!, r => r.FormKey, StringComparer.OrdinalIgnoreCase);
        var kwByEdid = new Lazy<Dictionary<string, FormKey>>(() => fo4.Keywords.Where(r => r.EditorID != null).ToDictionary(r => r.EditorID!, r => r.FormKey, StringComparer.OrdinalIgnoreCase));

        JsonNode? placementFile = a.Opt("placement") is string pp ? JsonNode.Parse(File.ReadAllText(pp)) : null;
        var doorUnlocks = new Dictionary<string, string>();     // "Fallout4.esm|0537C3" -> plugin path holding its winning REFR copy
        foreach (var du in (a.Opt("door-unlock") ?? "").Split(';', StringSplitOptions.RemoveEmptyEntries))
        {
            int deq = du.IndexOf('=');
            if (deq > 0) doorUnlocks[du[..deq]] = du[(deq + 1)..];
        }
        var cellBases = new Dictionary<string, string>();       // "Fallout4.esm|022884" -> plugin path holding its winning copy
        foreach (var cb in (a.Opt("cell-base") ?? "").Split(';', StringSplitOptions.RemoveEmptyEntries))
        {
            var eq = cb.IndexOf('=');
            if (eq > 0) cellBases[cb[..eq]] = cb[(eq + 1)..];
        }
        var lk = IdLock.Load(lockPath);
        lk.Frozen = a.Opt("frozen") == "true";
        bool dialogueGuard = a.Opt("ripperdoc-dialogue-guard") == "true";   // read here: the ripperdoc code below reuses the name 'a'
        var mod = new Fallout4Mod(Me, Fallout4Release.Fallout4);
        mod.ModHeader.Flags |= Fallout4ModHeader.HeaderFlag.Small;   // ESL
        mod.ModHeader.Author = "Cyberware FRAMEWORK generator";
        mod.ModHeader.Description = "Generated from cyberware.json sha256 " + catSha[..16] + ". Do not edit by hand.";
        var forms = new SortedDictionary<string, string>(StringComparer.Ordinal);
        FormKey K(string edid) { var id = lk.Get(edid); forms[edid] = id.ToString("X3"); return new FormKey(Me, id); }
        var report = new List<string>();

        // --- keywords
        var kwChip = mod.Keywords.AddNew(K("CW_KW_Chip")); kwChip.EditorID = "CW_KW_Chip";
        foreach (var s in Arr(cat, "slots"))
        {
            string e = "CW_KW_Slot_" + Str(s, "id");
            var kw = mod.Keywords.AddNew(K(e)); kw.EditorID = e;
        }

        // --- custom actor values (stats with "custom": true): AVIF records the engine and natives read
        foreach (var st in Arr(cat, "stats").Where(x => x["custom"]?.GetValue<bool>() == true))
        {
            string av = Str(st, "av");
            var r = mod.ActorValueInformation.AddNew(K(av));
            r.EditorID = av;
            r.Name = Str(st, "avName", Str(st, "name", av));
            r.Description = "Cyberware stat " + Str(st, "id");
            avByEdid[av] = r.FormKey;
        }

        // --- one value-modifier MGEF pair (+/-) per actor value used anywhere
        var mgef = new Dictionary<(string av, bool neg), FormKey>();
        FormKey Mgef(string av, bool neg)
        {
            if (mgef.TryGetValue((av, neg), out var fk)) return fk;
            if (!avByEdid.TryGetValue(av, out var avKey)) throw new GenError($"actor value '{av}' is not an ActorValueInformation EditorID in Fallout4.esm");
            string e = "CW_AvFX_" + av + (neg ? "_Neg" : "");
            var m = mod.MagicEffects.AddNew(K(e));
            m.EditorID = e;
            m.Name = "Cyberware: " + av + (neg ? " (-)" : "");
            m.Flags = MagicEffect.Flag.Recover | MagicEffect.Flag.HideInUI | MagicEffect.Flag.NoDuration | MagicEffect.Flag.NoArea | MagicEffect.Flag.Painless | MagicEffect.Flag.NoHitEvent
                      | (neg ? MagicEffect.Flag.Detrimental : 0);
            m.Archetype = new MagicEffectArchetype(MagicEffectArchetype.TypeEnum.ValueModifier) { ActorValue = new FormLink<IActorValueInformationGetter>(avKey) };
            m.CastType = CastType.ConstantEffect;
            m.TargetType = TargetType.Self;
            m.MagicSkill.SetToNull();
            m.ResistValue.SetToNull();
            mgef[(av, neg)] = m.FormKey;
            return m.FormKey;
        }
        Spell Ability(string edid, string name, IEnumerable<(string av, double value)> effects)
        {
            var sp = mod.Spells.AddNew(K(edid));
            sp.EditorID = edid;
            sp.Name = name;
            sp.Type = SpellType.Ability;
            sp.CastType = CastType.ConstantEffect;
            sp.TargetType = TargetType.Self;
            sp.Flags = SpellDataFlag.ManualCostCalc | SpellDataFlag.NoAbsorbOrReflect;
            sp.ObjectBounds = new ObjectBounds();
            foreach (var (av, v) in effects)
                sp.Effects.Add(new Effect { BaseEffect = new FormLinkNullable<IMagicEffectGetter>(Mgef(av, v < 0)), Data = new EffectData { Magnitude = (float)Math.Abs(v), Area = 0, Duration = 0 } });
            return sp;
        }
        Perk PerkFor(string edid, string name, FormKey spell)
        {
            var p = mod.Perks.AddNew(K(edid));
            p.EditorID = edid;
            p.Name = name;
            p.Playable = false;
            p.Hidden = true;
            p.NumRanks = 1;
            p.Level = 0;
            p.Effects.Add(new PerkAbilityEffect { Ability = new FormLink<ISpellGetter>(spell), Rank = 0, Priority = 0 });
            return p;
        }

        // --- stat bit perks
        int statPerks = 0;
        foreach (var s in Arr(cat, "stats"))
        {
            string id = Str(s, "id"), av = Str(s, "av"), name = Str(s, "name", id);
            double step = Num(s, "step", 1); int bits = (int)Num(s, "bits", 6), negBits = (int)Num(s, "negBits", 0);
            foreach (var (sign, n) in new[] { ("P", bits), ("N", negBits) })
                for (int b = 0; b < n; b++)
                {
                    double mag = step * (1L << b) * (sign == "N" ? -1 : 1);
                    var sp = Ability($"CW_StatSpell_{id}_{sign}{b}", $"Cyberware {name} {(mag > 0 ? "+" : "")}{mag}", new[] { (av, mag) });
                    PerkFor($"CW_Stat_{id}_{sign}{b}", $"Cyberware {name} {(mag > 0 ? "+" : "")}{mag}", sp.FormKey);
                    statPerks++;
                }
        }

        // --- abilities
        foreach (var ab in Arr(cat, "abilities"))
        {
            string id = Str(ab, "id"), name = Str(ab, "name", id);
            if (Str(ab, "perk") != "") { report.Add($"ability {id}: uses existing perk {Str(ab, "perk")}"); continue; }
            FormKey spell;
            if (Str(ab, "spell") != "") spell = Fk.Parse(Str(ab, "spell"));
            else spell = Ability("CW_AbSpell_" + id, name, Arr(ab, "effects").Select(e => (Str(e, "av"), Num(e, "value", 0)))).FormKey;
            var p = PerkFor("CW_Ab_" + id, name, spell);
            p.Description = Str(ab, "description");
        }

        // --- native perks CW_Nat_<ability>_T<t> (core/cw_effects.cpp decides what each carries; cwtest --native-effects
        //     writes it, this bakes it). Timed ones are added by the DLL while their effect runs, passive ones while the
        //     cyberware is installed, both through the same perk reconcile as the stats.
        var nativeMap = new JsonObject();
        JsonObject? chromeCritBands = null;   // CW_ChromeCrit's band tables, written below so nothing re-derives them
        bool hasNative = Arr(cat, "abilities").Any(x => x["spec"]?["native"] != null);
        JsonNode? natFile = a.Opt("native") is string np ? JsonNode.Parse(File.ReadAllText(np)) : null;
        if (hasNative && natFile == null) throw new GenError("the catalog has native abilities: pass --native <cwtest --native-effects output>");
        var fxCopies = new Dictionary<string, FormKey>();
        FormKey FxCopy(string edid, uint vanillaId, string name)
        {
            if (fxCopies.TryGetValue(edid, out var have)) return have;
            var v = fo4.MagicEffects.First(x => x.FormKey.ID == vanillaId && x.FormKey.ModKey.FileName == "Fallout4.esm");
            var c = v.Duplicate(K(edid));
            c.EditorID = edid;
            c.Name = name;
            c.CastType = CastType.ConstantEffect;       // abilities need constant effects
            c.Flags |= MagicEffect.Flag.NoDuration | MagicEffect.Flag.HideInUI;
            c.Conditions.Clear();
            if (edid == "CW_NatFX_SlowTime")   // the Sandevistan FX (plugin cw_fx.inl) drive the imagespace and sound; the MGEF must not stack Jet's on top
            {
                c.ImageSpaceModifier.Clear();
                c.Sounds?.Clear();
            }
            mod.MagicEffects.Add(c);
            return fxCopies[edid] = c.FormKey;
        }
        // A Fallout4.esm keyword, or one of ours (CW_*: the arm WEAP anim keywords ARMS lists; created here, reused by the arms
        // section below, which looks it up in `forms` the same way).
        FormKey NatKw(string edid)
        {
            if (kwByEdid.Value.TryGetValue(edid, out var k)) return k;
            if (!edid.StartsWith("CW_")) throw new GenError($"keyword '{edid}' is not in Fallout4.esm");
            if (!forms.ContainsKey(edid)) { var kw = mod.Keywords.AddNew(K(edid)); kw.EditorID = edid; kw.Type = Keyword.TypeEnum.None; }
            return new FormKey(Me, Convert.ToUInt32(forms[edid], 16));
        }
        // hit_spell: the vanilla spell for each status (Fallout4.esm). Magnitudes are the vanilla ones, unverified for balance.
        var hitSpells = new Dictionary<string, uint>
        {
            ["bleed"] = 0x1D4AB8,     // PerkDogmeatBleedingDmg (2 dmg/s, 5 s)
            ["burn"] = 0x1478EC,      // CritFire
            ["poison"] = 0x1FE6B7,    // LegendaryPoisonAttack (the Poisoner's legendary DOT)
            ["shock"] = 0x14615A,     // PA_ShockDamage's hit spell (Tesla power-armor arms)
            ["paralyze"] = 0x05240D,  // PerkParalyze (Iron Fist)
        };
        FormKey Av(string edid) => avByEdid.TryGetValue(edid, out var k) ? k : throw new GenError($"actor value '{edid}' is not an AVIF EditorID (Fallout4.esm or a custom stat)");
        ConditionFloat Fn(Condition.Function f, float cmp, CompareOperator op = CompareOperator.EqualTo, FormKey? p1 = null, bool or = false)
        {
            var d = new FunctionConditionData { Function = f, RunOnType = Condition.RunOnType.Subject };
            if (p1 is FormKey fk) d.ParameterOneRecord.SetTo(fk);
            return new ConditionFloat { Data = d, ComparisonValue = cmp, CompareOperator = op, Flags = or ? Condition.Flag.OR : 0 };
        }
        IEnumerable<ConditionFloat> When(JsonNode? w)
        {
            foreach (var c in (w as JsonArray ?? new JsonArray()).Where(x => x != null))
            {
                string fn = Str(c!, "fn"); float v = (float)Num(c!, "value", 0);
                yield return fn switch
                {
                    "sneaking" => Fn(Condition.Function.IsSneaking, 1),
                    "in_power_armor" => Fn(Condition.Function.WornHasKeyword, 1, CompareOperator.EqualTo, NatKw("ArmorTypePower")),
                    "combat" => Fn(Condition.Function.IsInCombat, 1),
                    "hp_below" => Fn(Condition.Function.GetHealthPercentage, v, CompareOperator.LessThan),
                    "hp_above" => Fn(Condition.Function.GetHealthPercentage, v, CompareOperator.GreaterThanOrEqualTo),
                    "ap_above" => Fn(Condition.Function.GetValuePercent, v, CompareOperator.GreaterThanOrEqualTo, Av("ActionPoints")),
                    "power_attack" => Fn(Condition.Function.IsPowerAttacking, 1),
                    "chance" => Fn(Condition.Function.GetRandomPercent, v, CompareOperator.LessThan),   // owner tab, as FortuneFinder04
                    "aiming" => Fn(Condition.Function.GetInIronSights, 1),
                    "endurance_at_least" => Fn(Condition.Function.GetValue, v, CompareOperator.GreaterThanOrEqualTo, Av("Endurance")),
                    _ => throw new GenError($"native condition '{fn}' has no FO4 condition in espgen"),
                };
            }
        }
        // entry -> (FO4 entry point, condition tab count). Tabs: attack = owner/weapon/target, incoming = owner/attacker/
        // attacker weapon (both as vanilla Rifleman01 / HC_DiseaseWeaknessPerk); vats_ap 3, vats_hit 1, fall 1 (vanilla);
        // power_ap and the stagger entries have no vanilla user: 2 tabs assumed (owner + weapon/target), unverified.
        var entryMap = new Dictionary<string, (APerkEntryPointEffect.EntryType ep, byte tabs)>
        {
            ["attack"] = (APerkEntryPointEffect.EntryType.ModWeaponAttackDamage, 3),
            ["incoming"] = (APerkEntryPointEffect.EntryType.ModIncomingWeaponDamage, 3),
            ["power_ap"] = (APerkEntryPointEffect.EntryType.ModPowerAttackActionPoints, 2),
            ["vats_ap"] = (APerkEntryPointEffect.EntryType.ModVATSAttackActionPoints, 3),
            ["vats_hit"] = (APerkEntryPointEffect.EntryType.ModVATSHitChance, 1),
            ["sneak_attack"] = (APerkEntryPointEffect.EntryType.ModSneakAttackMult, 3),          // vanilla Ninja01-03: owner/weapon/target, here Multiply (Lynx Paws)
            ["fall"] = (APerkEntryPointEffect.EntryType.ModFallingDamage, 1),
            ["stagger_out"] = (APerkEntryPointEffect.EntryType.ModTargetStagger, 2),
            ["stagger_in"] = (APerkEntryPointEffect.EntryType.ModIncomingStagger, 2),
            ["hit_spell"] = (APerkEntryPointEffect.EntryType.ApplyCombatHitSpell, 3),           // vanilla PA_ShockDamage: owner/weapon/target
            ["grenade_trajectory"] = (APerkEntryPointEffect.EntryType.ShowGrenadeTrajectory, 1), // vanilla DemolitionExpert02: Set 1
            ["incoming_battery"] = (APerkEntryPointEffect.EntryType.ModIncomingBatteryDamage, 1),
            ["xp"] = (APerkEntryPointEffect.EntryType.ModExp, 1),
            ["terminal_lockout"] = (APerkEntryPointEffect.EntryType.ModTerminalLockoutTime, 1),
            ["explosion"] = (APerkEntryPointEffect.EntryType.ModPlayerExplosionDamage, 2),         // vanilla DemolitionExpert04: owner/weapon
            ["incoming_explosion"] = (APerkEntryPointEffect.EntryType.ModIncomingExplosionDamage, 1), // vanilla mod_armor_Explosion2Perk
            ["vats_crit_charge"] = (APerkEntryPointEffect.EntryType.ModVATSCriticalCharge, 1),        // vanilla CompHancockPerk
            ["cone_of_fire"] = (APerkEntryPointEffect.EntryType.ModConeoffireMult, 2),                // vanilla: 2 tabs, Multiply (Militech Berserk recoil)
            ["detect_light"] = (APerkEntryPointEffect.EntryType.ModDetectionLight, 2),                // vanilla: 2 tabs (owner, the detecting actor)
            ["detect_movement"] = (APerkEntryPointEffect.EntryType.ModDetectionMovement, 2),          // Basic Kiroshi: turrets detect 40% slower
        };
        APerkEntryPointEffect Entry(string entry, double value, IEnumerable<ConditionFloat> owner, IEnumerable<ConditionFloat>? tab1 = null, IEnumerable<ConditionFloat>? tab2 = null, string spell = "")
        {
            if (!entryMap.TryGetValue(entry, out var em)) throw new GenError($"native entry '{entry}' has no FO4 entry point in espgen");
            APerkEntryPointEffect e;
            if (entry == "hit_spell")
            {
                if (!hitSpells.TryGetValue(spell, out var sid)) throw new GenError($"hit_spell '{spell}' has no vanilla spell in espgen");
                e = new PerkEntryPointSelectSpell { EntryPoint = em.ep, PerkConditionTabCount = em.tabs, Rank = 0, Priority = 0,
                                                    Spell = new FormLink<ISpellGetter>(new FormKey(ModKey.FromFileName("Fallout4.esm"), sid)) };
            }
            else e = new PerkEntryPointModifyValue { EntryPoint = em.ep, PerkConditionTabCount = em.tabs, Rank = 0, Priority = 0,
                                                     Modification = entry == "grenade_trajectory" ? PerkEntryPointModifyValue.ModificationType.Set : PerkEntryPointModifyValue.ModificationType.Multiply,
                                                     Value = (float)value };
            void Tab(byte i, IEnumerable<ConditionFloat>? cs)
            {
                var l = cs?.ToList();
                if (l == null || l.Count == 0) return;
                var pc = new PerkCondition { RunOnTabIndex = i };
                foreach (var c in l) pc.Conditions.Add(c);
                e.Conditions.Add(pc);
            }
            Tab(0, owner); Tab(1, tab1); Tab(2, tab2);
            return e;
        }
        int natPerks = 0, natEntries = 0;
        foreach (var (perkId, pn) in (natFile?["perks"] as JsonObject ?? new JsonObject()))
        {
            if (pn == null) continue;
            string ab = Str(pn, "ability"), name = Str(pn, "name", ab);
            int t = (int)Num(pn, "tier", 1);
            if (!perkId.EndsWith("_T" + t)) throw new GenError($"native perk {perkId} does not match tier {t}");
            var sp = mod.Spells.AddNew(K($"CW_NatSpell_{ab}_T{t}"));
            sp.EditorID = $"CW_NatSpell_{ab}_T{t}"; sp.Name = name; sp.Type = SpellType.Ability; sp.CastType = CastType.ConstantEffect; sp.TargetType = TargetType.Self;
            sp.Flags = SpellDataFlag.ManualCostCalc | SpellDataFlag.NoAbsorbOrReflect; sp.ObjectBounds = new ObjectBounds();
            var fx = pn["fx"]!;
            string kind = Str(fx, "kind"); float mag = (float)Num(fx, "magnitude", 0); int area = (int)Num(fx, "areaFeet", 0);
            if (kind != "")
            {
                FormKey baseFx = kind switch
                {
                    "slowtime" => FxCopy("CW_NatFX_SlowTime", 0x248900, "Cyberware: time slow"),
                    "chameleon" => FxCopy("CW_NatFX_Camo", 0x102B70, "Cyberware: Optical Camo"),
                    "detect" => new FormKey(ModKey.FromFileName("Fallout4.esm"), 0x247A40),
                    _ => throw new GenError($"native fx kind '{kind}' unknown"),
                };
                var fxe = new Effect { BaseEffect = new FormLinkNullable<IMagicEffectGetter>(baseFx), Data = new EffectData { Magnitude = mag, Area = area, Duration = 0 } };
                foreach (var c in When(fx["when"])) fxe.Conditions.Add(c);   // e.g. Stalker: only while aiming down sights
                sp.Effects.Add(fxe);
            }
            foreach (var av in Arr(pn, "avs"))
            {
                double v = Num(av, "value", 0);
                var eff = new Effect { BaseEffect = new FormLinkNullable<IMagicEffectGetter>(Mgef(Str(av, "av"), v < 0)), Data = new EffectData { Magnitude = (float)Math.Abs(v), Area = 0, Duration = 0 } };
                foreach (var c in When(av["when"])) eff.Conditions.Add(c);
                sp.Effects.Add(eff);
            }
            var perk = PerkFor(perkId, name + (pn["timed"]?.GetValue<bool>() == true ? " (active)" : ""), sp.FormKey);
            foreach (var en in Arr(pn, "entries"))
            {
                var weap = Arr(en, "weaponAny").Select(k => Fn(Condition.Function.HasKeyword, 1, CompareOperator.EqualTo, NatKw(k.GetValue<string>()), or: true))
                          .Concat(Arr(en, "weaponNone").Select(k => Fn(Condition.Function.HasKeyword, 0, CompareOperator.EqualTo, NatKw(k.GetValue<string>()))));
                var targ = Arr(en, "targetAny").Select(k => Fn(Condition.Function.HasKeyword, 1, CompareOperator.EqualTo, NatKw(k.GetValue<string>()), or: true));
                string entry = Str(en, "entry");
                bool detect = entry == "detect_light" || entry == "detect_movement";   // tab 1 = the detecting actor: targetAny goes there
                if ((weap.Any() || targ.Any()) && entry != "attack" && entry != "hit_spell" && !detect) throw new GenError($"{perkId}: keyword conditions only on 'attack' / 'hit_spell' / detect entries");
                // distance bands: GetDistance(PlayerRef) on the attacker tab (incoming) or the target tab (attack / hit_spell)
                var player = new FormKey(ModKey.FromFileName("Fallout4.esm"), 0x000014);
                double dMin = Num(en, "distMin", -1), dMax = Num(en, "distMax", -1);
                var dist = new List<ConditionFloat>();
                if (dMin >= 0) dist.Add(Fn(Condition.Function.GetDistance, (float)dMin, CompareOperator.GreaterThanOrEqualTo, player));
                if (dMax >= 0) dist.Add(Fn(Condition.Function.GetDistance, (float)dMax, CompareOperator.LessThan, player));
                if (dist.Count > 0 && entry != "incoming" && entry != "attack" && entry != "hit_spell") throw new GenError($"{perkId}: distance bands only on incoming / attack / hit_spell");
                if (entry == "incoming") perk.Effects.Add(Entry(entry, Num(en, "value", 1), When(en["when"]), dist, null, Str(en, "spell")));
                else if (detect) perk.Effects.Add(Entry(entry, Num(en, "value", 1), When(en["when"]), targ, null, ""));
                else perk.Effects.Add(Entry(entry, Num(en, "value", 1), When(en["when"]), weap, targ.Concat(dist), Str(en, "spell")));
                natEntries++;
            }
            natPerks++;
            nativeMap[perkId] = new JsonObject { ["kind"] = kind, ["magnitude"] = Math.Round(mag, 4), ["areaFeet"] = area,
                                                 ["avs"] = Arr(pn, "avs").Count(), ["entries"] = Arr(pn, "entries").Count() };
        }
        // ABILITIES (2026-11-08): the Sandevistan imagespaces and the Kiroshi / Sandevistan effect shaders. Copies of vanilla
        // forms so nothing vanilla changes colour; the plugin tints the EFSH copies at runtime (tint.h "kiroshi" / "hud").
        foreach (var (edid, id, r, g, b) in new (string, uint, byte, byte, byte)[] {
            ("CW_FxKiroshiHostile", 0x22517E, 255, 40, 40), ("CW_FxKiroshiNeutral", 0x22517E, 255, 210, 40), ("CW_FxKiroshiFriendly", 0x22517E, 60, 255, 90),
            ("CW_FxKiroshiLegendary", 0x22517E, 255, 190, 30), ("CW_FxKiroshiLoot", 0x22517E, 60, 220, 255), ("CW_FxSandyGhost", 0x183AFB, 46, 255, 140) })
        {
            var v = fo4.EffectShaders.First(x => x.FormKey.ID == id && x.FormKey.ModKey.FileName == "Fallout4.esm");
            var c = v.Duplicate(K(edid));
            c.EditorID = edid;
            c.EdgeEffectColor = System.Drawing.Color.FromArgb(r, g, b);
            c.FillColorKey1 = c.FillColorKey2 = c.FillColorKey3 = System.Drawing.Color.FromArgb(r, g, b);
            mod.EffectShaders.Add(c);
        }
        // Sandevistan imagespaces: a NiColorInterpolator cannot be retinted safely at runtime (no CommonLib layout), so bake a
        // palette and let the plugin pick the variant nearest the player's tint (tint.h "sandevistan"; "" = default green).
        // Double vision is a SCREEN-FRACTION offset in FO4, not a 0-2 "strength". Vanilla's strongest is 0.02
        // (ImageSpaceConcussion); TeleportInPlayerImod 0.008, CryoWakeImod 0.005; all 247 vanilla IMADs read from Fallout4.esm.
        // The loop shipped 1.5 from the first commit: every sample landed more than a screen away, the sampler clamped to the
        // edge row, and the whole view became that row stretched into columns for the whole effect (QD blocks 24 and 25,
        // P14 + P15). 0.008 is a slight ghost edge on high-contrast shapes; Trigger() scales it by 0.64-1.0 per tier.
        // Plug.CheckImadWithinVanilla below is the general rule: no curve of a generated imagespace may leave the range
        // vanilla uses for that field, whatever field a later change sets.
        foreach (var (pal, pr, pg, pb) in new (string, byte, byte, byte)[] { ("", 46, 255, 140), ("Cyan", 40, 220, 255), ("Blue", 60, 110, 255), ("Violet", 170, 70, 255), ("Red", 255, 60, 70), ("Amber", 255, 170, 40), ("White", 235, 240, 245) })
        // The vanilla fades run 2.0 s; these must land inside 100 ms of the key and be gone within 500 ms of the end (QUALITY-BAR SAN-1, SAN-3). Keyframe times are 0-1, so Duration rescales them.
        foreach (var (edid0, id, dv, dur, wash) in new (string, uint, float, float, float)[] { ("CW_ImodSandyIn", 0x218E64, 0f, 0.25f, 1f), ("CW_ImodSandyLoop", 0x2170E9, 0.008f, 0f, 0.45f), ("CW_ImodSandyOut", 0x218E65, 0f, 0.35f, 1f) })
        {
            var edid = pal == "" ? edid0 : $"{edid0}_{pal}";
            var v = fo4.ImageSpaceAdapters.First(x => x.FormKey.ID == id && x.FormKey.ModKey.FileName == "Fallout4.esm");
            var c = v.Duplicate(K(edid));
            c.EditorID = edid;
            if (dur > 0) c.Duration = dur;
            foreach (var f in c.TintColor) if (f.Color.A > 0) f.Color = System.Drawing.Color.FromArgb(Math.Max(1, (int)(f.Color.A * wash)), pr, pg, pb);   // wash < 1: the loop keeps colour without flooding the screen
            if (dv > 0) { c.DoubleVisionStrength.Clear(); c.DoubleVisionStrength.Add(new KeyFrame { Time = 0, Value = dv }); c.DoubleVisionStrength.Add(new KeyFrame { Time = 1, Value = dv }); }
            Plug.CheckImadWithinVanilla(fo4, c);
            mod.ImageSpaceAdapters.Add(c);
            // Build-log proof of the value a QUALITY row depends on (the lead, 2026-11-10: P15b's smear fix could only be proven by parsing
            // the deployed esp). One line per variant: the double-vision keyframes as written (time:value), and whether we set them.
            var inv = System.Globalization.CultureInfo.InvariantCulture;
            var dvKeys = string.Join(" ", c.DoubleVisionStrength.Select(k => k.Time.ToString("0.###", inv) + ":" + k.Value.ToString("0.####", inv)));
            Console.WriteLine("imad: " + edid + " double vision " + dvKeys + " (" + (dv > 0 ? "set" : "vanilla") + "), duration " + c.Duration.ToString("0.###", inv) + " s");
        }
        // CW_ChromeCrit: out-of-VATS crits and mitigation from the custom AVs (docs/DESIGN.md 3.5). One Mod Attack Damage
        // entry per (crit-chance band, crit-damage band): chance bands of 5% (GetRandomPercent < band floor), damage
        // x(1.5 + band); one Mod Incoming Damage entry per (mitigation-chance band, strength band): x(1 - (0.5 + band)).
        //
        // DEFECT E, fixed 2026-11-10 (BETA QA 2's item audit docs/qa/item-audit/FINDINGS.md; the lead's ruling).
        // The damage and strength bands used to be COARSER THAN THE STATS THEY QUANTISE: damage stepped 0/25/50/75 and
        // strength 0/20/40, while catalog tiers step by 5. So any piece contributing less than a whole band contributed
        // NOTHING. Measured cases: cw_ha_smart_link gives 5/5/5/10/10/10/10 crit damage and never reached a band alone;
        // ab_os_sandy_militech_falcon's crit_dmg_pct 20 landed in the 0 band, giving +0% or +25% but never +20%;
        // cw_ns_neofiber 5..11 and cw_sk_spring_joints 9..19 mitigation strength never reached 20, so a mitigated hit
        // stayed x0.5 with or without them. The bands are now the same 5 points as the stats, so every piece counts.
        // Cost: entries on this ONE perk, not new records - 80 -> 420 attack and 60 -> 180 incoming. Those conditions are
        // evaluated per hit, which is UNMEASURED: the READY carries a rig row timing a sustained fight with and without
        // it. If that measures badly, raise kBandStep to 10 - one line, 600 entries -> 320 (NOT 300: 20 chance bands x (11 damage
        // + 5 strength); the "300" in earlier notes was arithmetic nobody ran), still 2.5x finer than the old 25/20 bands.
        if (natFile?["chromeCrit"]?.GetValue<bool>() == true)
        {
            FormKey cc = Av("CW_CritChance"), cd = Av("CW_CritDamage"), mc = Av("CW_MitigationChance"), ms = Av("CW_MitigationStrength");
            var csp = Ability("CW_ChromeCritSpell", "Cyberware: Chrome Crit", Array.Empty<(string, double)>());
            var cp = PerkFor(kChromeCrit, "Cyberware: Chrome Crit", csp.FormKey);
            // THE FOUR BAND TABLES, and the ONE place each is defined. BETA QA's build check ("a bound number on
            // a banded key equals its band floor") must read these from HERE, not re-derive them: deriving the same
            // numbers twice is what let defect E hide, and what let Stabber state a chance the engine cannot roll. They
            // are written into formmap.json below as "chromeCritBands", which is the artefact the DLL already reads.
            //   CW_CritChance        kChanceStep   (NOT kBandStep: the chance loop is its own table)
            //   CW_MitigationChance  kChanceStep
            //   CW_CritDamage        kBandStep over 0..100
            //   CW_MitigationStrength kBandStep over 0..40
            // Every one is a FLOOR: a value is applied at its band's floor, so a piece worth less than a whole step
            // above its floor contributes nothing beyond it.
            const double kBandStep = 5.0;    // crit DAMAGE and mitigation STRENGTH. Defect E brought this from 25 / 20 to 5; 10.0 is the gated fallback (320 entries)
            const double kChanceStep = 5.0;  // crit CHANCE and mitigation CHANCE. Unchanged by defect E, and NOT tied to kBandStep
            const int kChanceBands = 20;     // chance bands are kChanceStep * 1..kChanceBands, the last one open-ended
            static double[] Bands(double hi, double step)
            {
                var v = new List<double>();
                for (double x = 0; x <= hi + 1e-9; x += step) v.Add(Math.Round(x, 4));
                return v.ToArray();
            }
            double[] dmgBands = Bands(100, kBandStep), strBands = Bands(40, kBandStep);
            for (int b = 1; b <= kChanceBands; b++)
            {
                float bLo = (float)(b * kChanceStep), bHi = b == kChanceBands ? 1e6f : (float)((b + 1) * kChanceStep);
                for (int j = 0; j < dmgBands.Length; j++)
                {
                    float dlo = (float)dmgBands[j], dhi = j + 1 < dmgBands.Length ? (float)dmgBands[j + 1] : 1e6f;
                    cp.Effects.Add(Entry("attack", 1.5 + dmgBands[j] / 100.0, new[] {
                        Fn(Condition.Function.GetValue, bLo, CompareOperator.GreaterThanOrEqualTo, cc), Fn(Condition.Function.GetValue, bHi, CompareOperator.LessThan, cc),
                        Fn(Condition.Function.GetValue, dlo, CompareOperator.GreaterThanOrEqualTo, cd), Fn(Condition.Function.GetValue, dhi, CompareOperator.LessThan, cd),
                        Fn(Condition.Function.GetRandomPercent, bLo, CompareOperator.LessThan) }));
                }
                for (int j = 0; j < strBands.Length; j++)
                {
                    float slo = (float)strBands[j], shi = j + 1 < strBands.Length ? (float)strBands[j + 1] : 1e6f;
                    cp.Effects.Add(Entry("incoming", Math.Max(0.1, 1.0 - (0.5 + strBands[j] / 100.0)), new[] {
                        Fn(Condition.Function.GetValue, bLo, CompareOperator.GreaterThanOrEqualTo, mc), Fn(Condition.Function.GetValue, bHi, CompareOperator.LessThan, mc),
                        Fn(Condition.Function.GetValue, slo, CompareOperator.GreaterThanOrEqualTo, ms), Fn(Condition.Function.GetValue, shi, CompareOperator.LessThan, ms),
                        Fn(Condition.Function.GetRandomPercent, bLo, CompareOperator.LessThan) }));
                }
            }
            // the band tables the generator actually used, for formmap.json (one source for BETA QA's check and the DLL)
            chromeCritBands = new JsonObject {
                ["CW_CritChance"] = new JsonArray(Enumerable.Range(1, kChanceBands).Select(b => (JsonNode?)JsonValue.Create(Math.Round(b * kChanceStep, 4))).ToArray()),
                ["CW_MitigationChance"] = new JsonArray(Enumerable.Range(1, kChanceBands).Select(b => (JsonNode?)JsonValue.Create(Math.Round(b * kChanceStep, 4))).ToArray()),
                ["CW_CritDamage"] = new JsonArray(dmgBands.Select(v => (JsonNode?)JsonValue.Create(v)).ToArray()),
                ["CW_MitigationStrength"] = new JsonArray(strBands.Select(v => (JsonNode?)JsonValue.Create(v)).ToArray()),
                ["areFloors"] = JsonValue.Create(true),
                ["chanceStep"] = JsonValue.Create(kChanceStep),
                ["magnitudeStep"] = JsonValue.Create(kBandStep),
                ["entries"] = JsonValue.Create(cp.Effects.Count - 1),
            };
            report.Add($"chrome crit: {kChromeCrit} with {cp.Effects.Count - 1} banded entries "
                     + $"(chance step {kChanceStep} x {kChanceBands} bands; magnitude step {kBandStep}: {dmgBands.Length} damage x {kChanceBands} chance attack, "
                     + $"{strBands.Length} strength x {kChanceBands} chance incoming)");
        }
        report.Add($"native: {natPerks} CW_Nat perks, {natEntries} perk entries");

        // --- loot chips (MISC): picking one up and opening a ripperdoc menu redeems it into the cyberware stash
        var chips = new JsonArray();
        void Chip(string edid, string name, int value, string kind, string id)
        {
            var m = mod.MiscItems.AddNew(K(edid));
            m.EditorID = edid;
            m.Name = name;
            m.Value = value;
            m.Weight = 0.1f;
            m.ObjectBounds = new ObjectBounds();
            m.Keywords = new ExtendedList<IFormLinkGetter<IKeywordGetter>> { kwChip.ToLink() };
            string? model = cat["chipModel"]?.GetValue<string>();
            if (!string.IsNullOrEmpty(model)) m.Model = new Model { File = model };
            chips.Add(new JsonObject { ["editorId"] = edid, ["kind"] = kind, ["id"] = id });
        }
        foreach (var it in Arr(cat, "items"))
        {
            var tiers = Arr(it, "tiers").ToList();
            Chip("CW_Chip_" + Str(it, "id"), "Cyberware Chip: " + Str(it, "name", Str(it, "id")), (int)Num(tiers.First(), "price", 100) / 4, "item", Str(it, "id"));
        }
        foreach (var ci in Arr(cat, "capacityItems").Where(x => Str(x, "form") == ""))
            Chip("CW_CapItem_" + Str(ci, "id"), Str(ci, "name", Str(ci, "id")), 250, "capacity", Str(ci, "id"));
        foreach (var md in Arr(cat, "mods"))
        {
            var tiers = Arr(md, "tiers").ToList();
            Chip("CW_ModChip_" + Str(md, "id"), "Cyberware Mod Chip: " + Str(md, "name", Str(md, "id")), (int)Num(tiers.First(), "price", 100) / 4, "mod", Str(md, "id"));
        }

        // --- ripperdocs: faction, NPC, chair activator, optional placement
        var chairs = new JsonArray();
        Faction? fac = null;
        var localVoices = new Dictionary<string, VoiceType>();   // EditorID -> our own VTYP, made once and shared
        // Ripperdoc talk (VARGA TALK 2026-11-12, docs/VARGA-TALK.md): made once with the faction, then each doc adds its
        // own greeting INFO to the one starting topic as the loop reaches it. Specific infos go in first and the
        // faction-wide fallback is appended after the loop, because the engine plays the first INFO whose conditions pass.
        Quest? rdQuest = null;
        DialogTopic? rdGreetTopic = null;
        var rdDialogueInfos = new List<DialogResponses>();   // every INFO of ours, for the force-subtitle pass below
        const byte kRdQuestPriority = 90;        // above F34_DialogueShared's 50, which is what wins today
        const uint kRdBranchFlags = 0x01;        // DLBR DNAM: Top-Level. See the long note at the branch itself
        const string kRdShopPrompt = "Let me see your cyberware.";   // The author's words; the DLL opens the view off this choice

        // One topic in the shape every vanilla custom topic uses: 'CUST' subtype, DATA category 0, priority 50.
        DialogTopic RdTopic(string edid, DialogBranch br)
        {
            var t = new DialogTopic(K(edid), Fallout4Release.Fallout4)
            {
                EditorID = edid,
                Priority = 50,
                Category = (DialogTopic.CategoryEnum)0,
                Subtype = DialogTopic.SubtypeEnum.Custom0,
                SubtypeName = new RecordType("CUST"),
            };
            t.Quest.SetTo(rdQuest!.FormKey);
            t.Branch.SetTo(br.FormKey);
            rdQuest!.DialogTopics.Add(t);
            return t;
        }

        // One INFO on that topic. onlyFor = this NPC only (GetIsID); null = any ripperdoc (GetInFaction). prompt = the line the
        // PLAYER picks; null for a greeting, which the engine plays when the conversation starts. Response shape copied from the
        // vanilla CUST shape Voices.cs already matches (emotion -1 via EmotionFixup, Unknown 1, cameras -1, text never empty).
        DialogResponses RdInfo(DialogTopic t, string edid, string text, string? prompt, Npc? onlyFor)
        {
            var info = new DialogResponses(K(edid), Fallout4Release.Fallout4) { EditorID = edid };
            var cd = new FunctionConditionData { Function = onlyFor != null ? Condition.Function.GetIsID : Condition.Function.GetInFaction };
            cd.ParameterOneRecord.SetTo(onlyFor != null ? onlyFor.FormKey : fac!.FormKey);
            info.Conditions.Add(new ConditionFloat { CompareOperator = CompareOperator.EqualTo, ComparisonValue = 1f, Data = cd });
            var resp = new DialogResponse { ResponseNumber = 1, InterruptPercentage = 0, Text = text };
            resp.Emotion.SetTo(FormKey.Null);   // FFFFFFFF, the vanilla "no emotion" (EmotionFixup rewrites the null link)
            resp.Unknown = 1;
            resp.CameraTargetAlias = -1;
            resp.CameraLocationAlias = -1;
            info.Responses.Add(resp);
            info.Flags = new DialogResponseFlags();   // emits ENAM; EmotionFixup.ForceSubtitleOn ORs 0x0200 into it
            info.SubtitlePriority = (DialogResponses.SubtitlePriorityLevel)1;
            if (prompt != null) info.Prompt = prompt;
            t.Responses.Add(info);
            rdDialogueInfos.Add(info);
            return info;
        }
        foreach (var rd in Arr(cat, "ripperdocs"))
        {
            string id = Str(rd, "id"), name = Str(rd, "name", id);
            if (fac == null)
            {
                fac = mod.Factions.AddNew(K("CW_RipperdocFaction"));
                fac.EditorID = "CW_RipperdocFaction";
                fac.Name = "Ripperdocs";
                fac.Flags = Faction.FactionFlag.HiddenFromPC | Faction.FactionFlag.Vendor;
                fac.Ranks.Add(new Rank { Number = 0 });
                // Ripperdoc talk (VARGA TALK 2026-11-12, docs/VARGA-TALK.md; test-only until proven: --ripperdoc-dialogue-guard).
                // A ripperdoc had no dialogue of its own, so activating one let the engine offer another mod's generic dialogue:
                // Fallout 34's TOP-LEVEL branch F34_SayBranch_F34_DialogueShared (FalloutNV34.esm 01F46D52, quest
                // F34_DialogueShared, start-game enabled, priority 50), whose 5,579 FNV_GREETING lines carry no conditions at all
                // ("Ready to try your luck?"). TESActivateEvent only notifies, so the DLL cannot stop it.
                //
                // The 2026-11-09 answer was a SILENT BLOCKING branch meant to end the conversation at once while the DLL opened our
                // view on the activation. TWO reasons that never worked, both measured on the installed beta's own Cyberware.esp
                // (sha256 01d19b89...) against the rig's Fallout4.esm and FalloutNV34.esm on 2026-11-12:
                //   1. DLBR DNAM was 0x2 (Blocking) with Top-Level NOT set. Of 132 vanilla DLBR, 129 are 0x1 (Top-Level), 2 are
                //      0x0 and exactly ONE is 0x2: BoSM01_DanseCompanionIntroForcegreetBranch, a forcegreet branch a package
                //      enters, not a player activation. All 5 of F34's branches are 0x1. A branch that is not Top-Level is not in
                //      the pool the engine picks from when the player starts a conversation, so priority 90 was never consulted.
                //   2. The topic's SNAM was four NUL bytes. All 35,443 vanilla DIAL carry a real 4-character subtype name, and
                //      every F34 FNV_GREETING topic uses 'CUST' with DATA 00000000 - the custom-topic shape.
                // And the author's 2026-11-12 ask reversed the intent: he wants her to TALK, with our menu opening only when he picks
                // "Let me see your cyberware". So this is now a real dialogue of her own: a Top-Level branch on a priority-90
                // start-game-enabled quest, a greeting topic in the 'CUST' shape with one INFO per ripperdoc, and two player
                // choices. The editor ids FA0/FA1/FA2 are KEPT (a shipped record is never renumbered) even though "Block" no
                // longer describes them. The DLL opens the view off the shop INFO, never off the activation.
                if (dialogueGuard)
                {
                    rdQuest = mod.Quests.AddNew(K("CW_RipperdocDialogue"));
                    rdQuest.EditorID = "CW_RipperdocDialogue";
                    rdQuest.Data = new QuestData { Flags = Quest.Flag.StartGameEnabled, Priority = kRdQuestPriority };
                    var br = new DialogBranch(K("CW_RipperdocBlockBranch"), Fallout4Release.Fallout4) { EditorID = "CW_RipperdocBlockBranch" };
                    rdQuest.DialogBranches.Add(br);
                    br.Quest.SetTo(rdQuest.FormKey);
                    // Written as the number so the record carries exactly this whatever the enum's values are (as the VTYP DNAM
                    // is). 0x01 = Top-Level, the shape 129 of 132 vanilla branches use. If a capture shows F34's topics still in
                    // the player's option list, the variant to try is kRdBranchFlags | 0x02 (Blocking) - one line, here.
                    br.Flags = (DialogBranch.Flag)kRdBranchFlags;
                    rdGreetTopic = RdTopic("CW_RipperdocBlockTopic", br);
                    br.StartingTopic.SetTo(rdGreetTopic.FormKey);
                    // The two player choices. Their INFOs are faction-wide, so every ripperdoc offers both.
                    RdInfo(RdTopic("CW_RipperdocTopicShop", br), "CW_RipperdocInfoShop",
                           "Sit. Let's see what you're working with.", kRdShopPrompt, null);
                    RdInfo(RdTopic("CW_RipperdocTopicBye", br), "CW_RipperdocInfoBye",
                           "Suit yourself. Door's where you left it.", "Not right now.", null);
                    report.Add($"ripperdoc talk: CW_RipperdocDialogue (priority {kRdQuestPriority}), branch DNAM 0x{kRdBranchFlags:X2} (top-level), 'CUST' topics, prompts \"{kRdShopPrompt}\" / \"Not right now.\"");
                }
            }
            var npc = mod.Npcs.AddNew(K("CW_Ripperdoc_" + id));
            npc.EditorID = "CW_Ripperdoc_" + id;
            npc.Name = name;
            npc.Flags = Npc.Flag.Unique | Npc.Flag.Protected | Npc.Flag.AutoCalcStats;
            npc.Race = new FormLink<IRaceGetter>(Fk.Parse(Str(rd, "race", "Fallout4.esm|013746")));  // HumanRace
            npc.ObjectBounds = new ObjectBounds();
            npc.Level = new NpcLevel { Level = 10 };
            npc.Factions.Add(new RankPlacement { Faction = fac.ToLink(), Rank = 0 });
            string tpl = Str(rd, "npcTemplate");
            if (tpl != "")
            {
                // Traits (face, body, voice) + inventory come from a vanilla NPC that has FaceGen: no dark-face bug.
                npc.DefaultTemplate = new FormLinkNullable<INpcSpawnGetter>(Fk.Parse(tpl));
                npc.UseTemplateActors = Npc.TemplateActorType.Traits | Npc.TemplateActorType.Inventory | Npc.TemplateActorType.ModelOrAnimation;
            }

            if (Str(rd, "outfit") != "") npc.DefaultOutfit = new FormLinkNullable<IOutfitGetter>(Fk.Parse(Str(rd, "outfit")));
            if (Str(rd, "voice") != "") npc.Voice = new FormLinkNullable<IVoiceTypeGetter>(Fk.Parse(Str(rd, "voice")));

            var chair = mod.Activators.AddNew(K("CW_RipperdocChair_" + id));
            chair.EditorID = "CW_RipperdocChair_" + id;
            chair.Name = Str(rd, "chairName", "Ripperdoc Chair");
            chair.ActivateTextOverride = Str(rd, "chairVerb", "Get Chromed");
            chair.ObjectBounds = new ObjectBounds();
            chair.Model = new Model { File = Str(rd, "chairModel", Str(cat, "chairModel", "Furniture\\Chairs\\ChairDentist01.nif")) };
            chairs.Add(new JsonObject { ["editorId"] = chair.EditorID, ["ripperdoc"] = id });

            // Placement (SPEC docs/RIPPERDOCS.md): NPC + a FaceGenSurgeryChair furniture ref into the WINNING copy of
            // the cell; or an existing vanilla chair ref (spec.placementExtra.chairExisting). Activating the chair ref
            // opens this doc's menu (formmap ripperdocChairs[].ref).
            var plx = placementFile?["ripperdocs"]?.AsArray().FirstOrDefault(x => Str(x!, "id") == id);
            if (plx != null)
            {
                npc.Race = new FormLink<IRaceGetter>(RecordByEdid(fo4.Races, Str(plx, "race", "HumanRace")));
                if (Str(plx, "sex") == "female") npc.Flags |= Npc.Flag.Female;
                // Voice. A name starting with CW_ is a voice type of OUR OWN, created below; anything else is an
                // EditorID in Fallout4.esm. Why a custom one matters: FO4 gates every generic dialogue set at the
                // QUEST level with one GetIsVoiceType against a voice type or a FormList of them, so a vanilla voice
                // type puts vanilla barks in a unique NPC's mouth - FemaleEvenToned alone let 21 quests / 436 recorded
                // INFOs select Varga, which is how a vanilla "Huh?" ended up subtitled under her name. A type that is
                // in no FormList fails every one of those conditions before an INFO is picked. docs/VARGA-VOICETYPE.md.
                string pvoice = Str(plx, "voice");
                if (pvoice.StartsWith("CW_"))
                {
                    if (!localVoices.TryGetValue(pvoice, out var vt))
                    {
                        vt = mod.VoiceTypes.AddNew(K(pvoice));
                        vt.EditorID = pvoice;
                        // DNAM 0x02 = female, "allow default dialogue" CLEAR: exactly what every unique vanilla
                        // character carries (Magnolia 0x02), while the generic pool is the 30 types with bit 0 set,
                        // FemaleEvenToned among them at 0x03. Read from Fallout4.esm: bit 0 is default dialogue and
                        // bit 1 is Female, which every pair proves (MaleRough 0x01 / FemaleRough 0x03,
                        // PlayerVoiceMale01 0x01 / PlayerVoiceFemale01 0x03, the same for Child and Old) - and
                        // Mutagen's own Fallout4 VoiceType.Flag is {AllowDefaultDialog = 1, Female = 2}, which agrees.
                        // Written as the byte so the record carries 0x02 whatever the enum's values are; row VV1
                        // reads the DNAM back out of the built plugin.
                        vt.Flags = (VoiceType.Flag)0x02;
                        localVoices[pvoice] = vt;
                        report.Add($"voice type {pvoice}: own VTYP, DNAM 0x02 (female, no default dialogue) - in no vanilla voice FormList, so no generic bark can select her");
                    }
                    npc.Voice = new FormLinkNullable<IVoiceTypeGetter>(vt.FormKey);
                }
                else if (pvoice != "") npc.Voice = new FormLinkNullable<IVoiceTypeGetter>(RecordByEdid(fo4.VoiceTypes, pvoice));
                if (HexIn(Str(plx, "outfit")) is uint oid) npc.DefaultOutfit = new FormLinkNullable<IOutfitGetter>(new FormKey(ModKey.FromFileName("Fallout4.esm"), oid));
                foreach (var f in Arr(plx, "factions"))
                    if (HexIn(f.GetValue<string>()) is uint fid) npc.Factions.Add(new RankPlacement { Faction = new FormLink<IFactionGetter>(new FormKey(ModKey.FromFileName("Fallout4.esm"), fid)), Rank = 0 });
                // Pinned head parts (VISUAL POLISH 2026-11-09). A ripperdoc has no FaceGen, so the engine builds its head at
                // runtime: every part it would otherwise take from the race defaults is named here, each one verified by
                // tools/headparts_scan.py to have no cloth data in this load order (a cloth hair crashed Varga's cell).
                foreach (var h in Arr(plx, "headParts"))
                    npc.HeadParts.Add(new FormLink<IHeadPartGetter>(RecordByEdid(fo4.HeadParts, h.GetValue<string>())));
                // Her own skin (RIPPERDOC LORE 2026-11-09, assets-pipeline/varga/FACE-NECK-DESIGN.md). A copy of a pinned vanilla face
                // part whose model is the same head NIF with only its material path changed, plus her own copy of the race's default
                // face texture set (HumanRace DFTF = SkinHeadFemaleHero) as her Head Texture, both pointing at her maps: whichever of
                // the two the runtime head takes its textures from, it gets hers. The copied part keeps the original's .tri morphs and
                // extra parts, and the same mesh, so the no-cloth check on it (polish/headparts.txt) still holds.
                if (plx["head"] is JsonObject hdj)
                {
                    string hFrom = Str(hdj, "from"), tFrom = Str(hdj, "textureFrom"), hEd = Str(hdj, "edid"), tEd = Str(hdj, "textureEdid");
                    var tSrc = fo4.TextureSets.FirstOrDefault(x => string.Equals(x.EditorID, tFrom, StringComparison.OrdinalIgnoreCase))
                               ?? throw new GenError($"ripperdoc {id}: head.textureFrom '{tFrom}' is not a TXST EditorID in Fallout4.esm");
                    var ts = tSrc.Duplicate(K(tEd));
                    ts.EditorID = tEd;
                    ts.Diffuse = Str(hdj, "diffuse");
                    ts.NormalOrGloss = Str(hdj, "normal");
                    ts.SmoothSpec = Str(hdj, "smoothSpec");
                    mod.TextureSets.Add(ts);
                    var hSrc = fo4.HeadParts.FirstOrDefault(x => string.Equals(x.EditorID, hFrom, StringComparison.OrdinalIgnoreCase))
                               ?? throw new GenError($"ripperdoc {id}: head.from '{hFrom}' is not an HDPT EditorID in Fallout4.esm");
                    var hp = hSrc.Duplicate(K(hEd));
                    hp.EditorID = hEd;
                    hp.Model = new Model { File = Str(hdj, "model") };
                    hp.TextureSet.SetTo(ts.FormKey);
                    mod.HeadParts.Add(hp);
                    int swapped = 0;
                    for (int i = 0; i < npc.HeadParts.Count; i++)
                        if (npc.HeadParts[i].FormKey == hSrc.FormKey) { npc.HeadParts[i] = new FormLink<IHeadPartGetter>(hp.FormKey); swapped++; }
                    if (swapped != 1) throw new GenError($"ripperdoc {id}: head.from '{hFrom}' must appear exactly once in headParts (found {swapped})");
                    npc.HeadTexture.SetTo(ts.FormKey);
                    report.Add($"ripperdoc {id}: own head {hEd} (copy of {hFrom}, model {Str(hdj, "model")}) and head texture {tEd} (copy of {tFrom})");
                }
                if (Str(plx, "hairColor") != "") npc.HairColor.SetTo(RecordByEdid(fo4.Colors, Str(plx, "hairColor")));
                // QNAM / Texture Lighting (FACE 2026-11-12). Measured: of the 424 female NPC_ records in
                // Fallout4.esm that carry their own head parts, 424 of 424 have a QNAM; our ripperdocs had
                // none. Conformance only - what a missing QNAM renders as was never measured, and the look
                // fix is NOT credited to it. The catalog gives four floats 0..1 (r, g, b, a).
                if (plx["textureLighting"] is JsonArray tl && tl.Count >= 4)
                {
                    static int C(JsonNode? n) => Math.Clamp((int)Math.Round((n?.GetValue<double>() ?? 0.0) * 255.0), 0, 255);
                    npc.TextureLighting = System.Drawing.Color.FromArgb(C(tl[3]), C(tl[0]), C(tl[1]), C(tl[2]));
                    report.Add($"ripperdoc {id}: QNAM texture lighting {C(tl[0])},{C(tl[1])},{C(tl[2])} a{C(tl[3])} (424 of 424 vanilla female NPCs with their own head parts carry one; ours had none)");
                }
                // Outfit made of named vanilla armor pieces (Kessler's goggles, Achterberg's glasses): a new OTFT in our
                // ESL. Its FormID is pinned in data/formids.lock.json inside VISUAL POLISH's range 0xF88-0xF97.
                var outfitItems = Arr(plx, "outfitItems").Select(x => x.GetValue<string>()).ToList();
                // A doctor's own outfit (Varga): the NIF carries the visible body too, because an ARMA on slot 33 hides the skin, arms included.
                Armor? customArmo = null;
                if (plx["customOutfit"] is JsonObject co)
                {
                    var human = new FormKey(ModKey.FromFileName("Fallout4.esm"), 0x013746);
                    string ed = Str(co, "edid");
                    static uint Bit(int slot) => 1u << (slot - 30);
                    uint Hands = Bit(34) | Bit(35);   // FO4's own NakedHands ARMA is on 34+35; on any other slot the bare hands keep drawing under these
                    ArmorAddon Addon(string suffix, uint slots, string file)
                    {
                        var aa = mod.ArmorAddons.AddNew(K(ed + suffix));
                        aa.EditorID = ed + suffix;
                        aa.BodyTemplate = new BodyTemplate { FirstPersonFlags = (BipedObjectFlag)slots };
                        aa.Race = new FormLinkNullable<IRaceGetter>(human);
                        aa.WorldModel = new GenderedItem<Model?>(null, new Model { File = file });
                        return aa;
                    }
                    customArmo = mod.Armors.AddNew(K(ed));
                    customArmo.EditorID = ed;
                    customArmo.Name = Str(co, "name", ed);
                    customArmo.MajorFlags |= Armor.MajorFlag.NonPlayable;
                    customArmo.BipedBodyTemplate = new BipedBodyTemplate { FirstPersonFlags = (BipedObjectFlag)(Bit(30) | Bit(31) | Bit(33) | Hands | Bit(47)) };   // hair top and long (slot 30 alone leaves the lower part of her vanilla hair drawing under this one), body, both hands, eyes (the loupe)
                    customArmo.Race = new FormLinkNullable<IRaceGetter>(human);
                    foreach (var dressAddon in new[] { Addon("_AA", Bit(33), Str(co, "body")), Addon("_HandsAA", Hands, Str(co, "hands")), Addon("_LoupeAA", Bit(47), Str(co, "loupe")), Addon("_HairAA", Bit(30) | Bit(31), Str(co, "hair")) })
                        customArmo.Armatures.Add(new ArmorAddonModel { AddonIndex = 0, ArmorAddon = new FormLinkNullable<IArmorAddonGetter>(dressAddon.FormKey) });
                }
                if (outfitItems.Count > 0 || customArmo != null)
                {
                    var o = mod.Outfits.AddNew(K("CW_RipperdocOutfit_" + id));
                    o.EditorID = "CW_RipperdocOutfit_" + id;
                    o.Items = new ExtendedList<IFormLinkGetter<IOutfitTargetGetter>>();
                    foreach (var it in outfitItems)
                        o.Items.Add(new FormLink<IOutfitTargetGetter>(RecordByEdid(fo4.Armors, it)));
                    if (customArmo != null) o.Items.Add(new FormLink<IOutfitTargetGetter>(customArmo.FormKey));
                    npc.DefaultOutfit = new FormLinkNullable<IOutfitGetter>(o.FormKey);
                }
            }
            npc.DefaultTemplate.SetToNull();   // no template: FO4 builds the head at runtime when there is no FaceGen
            // Stats from a vanilla doctor. First in-game test 2026-11-08: no CLAS + AutoCalcStats left Doc Kessler at 0 HP,
            // dead where he stood (lifeState 2). Class, level and actor-value properties come from DocCrocker (male) /
            // DoctorAmari (female) unless the catalog ripperdoc names another NPC in "statsFrom".
            {
                string statsEdid = Str(rd, "statsFrom", npc.Flags.HasFlag(Npc.Flag.Female) ? "DoctorAmari" : "DocCrocker");
                var src = fo4.Npcs.FirstOrDefault(n => string.Equals(n.EditorID, statsEdid, StringComparison.OrdinalIgnoreCase))
                          ?? throw new GenError($"ripperdoc {id}: statsFrom '{statsEdid}' is not an NPC_ EditorID in Fallout4.esm");
                npc.Class = new FormLinkNullable<IClassGetter>(src.Class.FormKey);
                npc.Level = src.Level.DeepCopy();
                npc.Properties = src.Properties == null ? null : new ExtendedList<ObjectProperty>(src.Properties.Select(x => x.DeepCopy()));
                if (src.Flags.HasFlag(Npc.Flag.AutoCalcStats)) npc.Flags |= Npc.Flag.AutoCalcStats; else npc.Flags &= ~Npc.Flag.AutoCalcStats;
                report.Add($"ripperdoc {id}: stats from {statsEdid} (class {src.Class.FormKey}, {(src.Properties?.Count ?? 0)} AV properties, autocalc {src.Flags.HasFlag(Npc.Flag.AutoCalcStats)})");
            }
            // This doc's own greeting line, as an INFO on the ONE starting topic, conditioned to this NPC (GetIsID). It is added
            // here - before every "not placed" continue below - so a doc that is generated but unplaced still owns its line and
            // its FormID never drifts. Varga's words are line c8e7741c of data/ripperdoc-voice.json, the greeting
            // READY-lore-greeting-c8e7741c already chose and already has a 4,270 ms clip for: writing the SAME words as text
            // means the voiced update only has to drop <INFO id>_1.fuz and its .lip beside it, with no record edit.
            if (dialogueGuard && rdGreetTopic != null)
            {
                string greet = id switch
                {
                    "diamondcity" => "Crocker's upstairs. He has never asked what's behind this door.",
                    "goodneighbor" => "Anton Kessler. Sit if you're buying. Talk if you're not.",
                    "institute" => "Dr. Achterberg. State your requirement.",
                    _ => "",
                };
                if (greet == "") throw new GenError($"ripperdoc {id}: no greeting line. Add one beside the others in Program.cs (docs/VARGA-TALK.md)");
                RdInfo(rdGreetTopic, "CW_RipperdocGreetInfo_" + id, greet, null, npc);
                report.Add($"ripperdoc {id}: greeting INFO CW_RipperdocGreetInfo_{id} (GetIsID), text \"{greet}\"");
            }

            var pl = rd["placement"] as JsonObject;
            if (pl == null || pl["cell"] == null) { report.Add($"ripperdoc {id}: records generated, not placed (no 'placement')"); continue; }
            // disabled (the author, 2026-11-09, Achterberg): the doc is not added to the game yet. Every record
            // (NPC_, OTFT, formmap rows) is still generated with its locked FormID so he can be enabled later
            // and the lock never drifts, but placement is skipped entirely: no ACHR, no chair ref, no CELL
            // override for his cell. Flag shape agreed with RIPPERDOC LORE.
            if (rd["disabled"]?.GetValue<bool>() == true)
            {
                report.Add($"ripperdoc {id}: DISABLED - records generated with locked ids, nothing placed (no NPC, chair or cell override)");
                continue;
            }
            // Crash guard (VISUAL POLISH 2026-11-09, crash-2026-11-09-00-07-47.log): a ripperdoc has no FaceGen, so the
            // engine builds its head at runtime from the race's default head parts. HumanRace's default female hair
            // (HairFemale01) carries cloth data from a hair mod in this load order, and building it crashed the game
            // the moment Varga's cell loaded (hclClothInstance::setTransformSet <- BSFaceGenUtils::AttachHeadHelper).
            // So a female ripperdoc is placed only once every runtime head part is pinned to a part verified to have
            // no cloth data ("headParts" in the placement data). Until then she stays in the catalog, unplaced.
            if (npc.Flags.HasFlag(Npc.Flag.Female) && npc.HeadParts.Count == 0)
            {
                report.Add($"ripperdoc {id}: NOT placed - female with no pinned head parts (runtime head = race default hair with cloth data = crash; see docs/LESSONS.md)");
                continue;
            }
            var cellKey = Fk.Parse(Str(pl, "cell"));
            string? basePath = cellBases.TryGetValue(Str(pl, "cell"), out var bp) ? bp : null;
            if (basePath == null) { report.Add($"ripperdoc {id}: NOT placed - no --cell-base for {cellKey} (build-rig passes the winning plugin)"); continue; }
            var cellCtx = WinningCellContext(basePath, cellKey);
            var cell = (Cell)cellCtx.GetOrAddAsOverride(mod);
            if (cellCtx.Record.Name != null && string.IsNullOrEmpty(cell.Name?.String))
                throw new GenError($"CELL {cellKey} from {Path.GetFileName(basePath)} has a localized name that reads empty: pass --strings <dir with Fallout4_en.STRINGS> (build-rig extracts them)");
            report.Add($"ripperdoc {id}: cell {cellKey} name '{cell.Name?.String}'");
            float[] Pos(string k) => Arr(pl, k).Select(v => (float)v.GetValue<double>()).ToArray();
            var npcRef = new PlacedNpc(K("CW_RipperdocRef_" + id), Fallout4Release.Fallout4)
            {
                EditorID = "CW_RipperdocRef_" + id,
                Base = new FormLinkNullable<INpcGetter>(npc.FormKey),
                Position = new P3Float(Pos("npcPos")[0], Pos("npcPos")[1], Pos("npcPos")[2]),
                Rotation = new P3Float(0, 0, (float)Num(pl, "npcRot", 0)),
            };
            npcRef.MajorFlags |= PlacedNpc.MajorFlag.Persistent;
            cell.Persistent.Add(npcRef);
            string existing = Str(rd["spec"]?["placementExtra"] ?? new JsonObject(), "chairExisting");
            var chairEntry = new JsonObject { ["editorId"] = chair.EditorID, ["ripperdoc"] = id, ["npcRef"] = npcRef.EditorID };
            if (existing != "")
            {
                chairEntry["refForm"] = existing;
                report.Add($"ripperdoc {id}: placed in {Path.GetFileName(basePath)} copy of {cellKey}; uses the existing chair ref {existing}");
            }
            else
            {
                string chairBase = Str(rd["spec"]?["placementExtra"] ?? new JsonObject(), "chairBase", "Fallout4.esm|052812");
                var chairRef = new PlacedObject(K("CW_RipperdocChairRef_" + id), Fallout4Release.Fallout4)
                {
                    EditorID = "CW_RipperdocChairRef_" + id,
                    Base = new FormLinkNullable<IPlaceableObjectGetter>(Fk.Parse(chairBase)),
                    Position = new P3Float(Pos("chairPos")[0], Pos("chairPos")[1], Pos("chairPos")[2]),
                    Rotation = new P3Float(0, 0, (float)Num(pl, "chairRot", 0)),
                };
                chairRef.MajorRecordFlagsRaw |= 0x400;   // persistent, so its FormID is stable for the activation map
                cell.Persistent.Add(chairRef);
                chairEntry["refEditorId"] = chairRef.EditorID;
                report.Add($"ripperdoc {id}: placed NPC + {chairBase} chair in {Path.GetFileName(basePath)} copy of {cellKey}");
                // The doc's own seat (QUALITY DIRECTOR DOC-2 + LORE + the lead, 2026-11-09). Sitting IN the
                // Memory Lounger closes the hood over his face (the close is baked into the model's
                // Enter/Occupied controller sequences) and kills the talk prompt; vanilla's own pattern is
                // Kent Connolly in a plain chair, never a pod. placementExtra.docSeat places that chair and
                // the sit link targets IT; the lounger above stays as the patient chair / install-scene anchor.
                PlacedObject? seatRef = null;
                if (rd["spec"]?["placementExtra"]?["docSeat"] is JsonObject ds)
                {
                    float[] spos = Arr(ds, "pos").Select(v => (float)v.GetValue<double>()).ToArray();
                    seatRef = new PlacedObject(K("CW_RipperdocSeatRef_" + id), Fallout4Release.Fallout4)
                    {
                        EditorID = "CW_RipperdocSeatRef_" + id,
                        Base = new FormLinkNullable<IPlaceableObjectGetter>(Fk.Parse(Str(ds, "base"))),
                        Position = new P3Float(spos[0], spos[1], spos[2]),
                        Rotation = new P3Float(0, 0, (float)Num(ds, "rot", 0)),
                    };
                    seatRef.MajorRecordFlagsRaw |= 0x400;
                    cell.Persistent.Add(seatRef);
                    report.Add($"ripperdoc {id}: doc seat {Str(ds, "baseEditorId", Str(ds, "base"))} at ({spos[0]}, {spos[1]}, {spos[2]})");
                }
                // The doc walks to his seat and sits (VISUAL POLISH 2026-11-09, the lead's T5): the vanilla generic package
                // DefaultSitLinkCustom01NoEating (Fallout4.esm 1C4E73, Sit template, no conditions) sits the actor in the
                // ref his LinkCustom01 (05D5E6) points at: the docSeat when one is placed, else the chair ref.
                // Displaced (moved, pushed, after a talk) he paths back over the navmesh and sits again.
                if (rd["spec"]?["placementExtra"]?["sitInChair"]?.GetValue<bool>() == true)
                {
                    var sitTarget = seatRef ?? chairRef;
                    npc.Packages.Add(new FormLink<IPackageGetter>(Fk.Parse("Fallout4.esm|1C4E73")));
                    npcRef.LinkedReferences.Add(new LinkedReferences
                    {
                        KeywordOrReference = new FormLink<IKeywordLinkedReferenceGetter>(Fk.Parse("Fallout4.esm|05D5E6")),
                        Reference = new FormLink<IPlacedGetter>(sitTarget.FormKey),
                    });
                    report.Add($"ripperdoc {id}: sits in {sitTarget.EditorID} (package DefaultSitLinkCustom01NoEating, LinkCustom01 -> {(seatRef != null ? "doc seat" : "chair")})");
                }
            }
            // Unlock named doors on the approach (VISUAL POLISH 2026-11-09, T5 cycle 3 root cause). Kessler's back
            // room's ONLY door (0537C3 DoctorAmariLabDoorREF) is key-locked level 255 with no key anywhere in the
            // load order, so neither his sit package nor the player could ever reach the room: the Sit package fell
            // back to the nearest reachable chair (the kitchen chair 348 u away). The override copies the WINNING
            // REFR (espgen winner pass, same contract as --cell-base) and removes only the lock. COMCurieQuest holds
            // this door only as a script property (pDoctorAmariLabDoorREF) whose runtime Lock/Unlock calls still
            // work on a base-unlocked door.
            foreach (var dNode in Arr(rd["spec"]?["placementExtra"] ?? new JsonObject(), "unlockDoors"))
            {
                string doorKey = dNode.GetValue<string>();
                if (!doorUnlocks.TryGetValue(doorKey, out var duPath))
                    throw new GenError($"ripperdoc {id}: placementExtra.unlockDoors names {doorKey} but gen got no --door-unlock \"{doorKey}=<winning plugin>\" (build-rig passes it from the winner pass)");
                var door = (PlacedObject)WinningPlacedContext(duPath, Fk.Parse(doorKey)).GetOrAddAsOverride(mod);
                if (door.Lock == null)
                    report.Add($"ripperdoc {id}: door {doorKey} already unlocked in {Path.GetFileName(duPath)} (override kept)");
                else
                {
                    door.Lock = null;
                    report.Add($"ripperdoc {id}: door {doorKey} unlocked (REFR override from {Path.GetFileName(duPath)}, XLOC removed)");
                }
            }
            chairs[chairs.Count - 1] = chairEntry;
        }

        // The faction-wide fallback greeting, added LAST so it is the last INFO on the topic: the engine plays the first one
        // whose conditions pass, so a ripperdoc with its own GetIsID line above always wins, and a doc added later without one
        // still greets instead of falling through to another mod's dialogue. It keeps the shipped id FA2.
        if (dialogueGuard && rdGreetTopic != null)
        {
            RdInfo(rdGreetTopic, "CW_RipperdocBlockInfo", "Door's shut. Talk.", null, null);
            report.Add($"ripperdoc talk: {rdDialogueInfos.Count} INFOs on {rdQuest!.DialogTopics.Count} topics (greeting + 2 player choices), fallback CW_RipperdocBlockInfo last");
        }

        // --- race subgraphs (ARMS): override the WINNING HumanRaceSubGraphData and append our entries
        string? sgPath = a.Opt("subgraphs");
        var sgOurs = new List<Subgraph>();   // every entry we add, appended or inserted (Verify skips them by position)
        if (sgPath != null)
        {
            string basePath = a.Req("subgraph-base");
            var sg = JsonNode.Parse(File.ReadAllText(sgPath))!;
            var entries = sg["subgraphs"]!.AsArray();
            var raceKey = Fk.Parse(entries.Select(e => Str(e!, "race", "")).FirstOrDefault(r => r != "") ?? "Fallout4.esm|166729");
            using var baseMod = Plug.Read(basePath);
            var winner = baseMod.Races.FirstOrDefault(r => r.FormKey == raceKey)
                         ?? throw new GenError($"{Path.GetFileName(basePath)} has no RACE {raceKey}");
            var race = mod.Races.GetOrAddAsOverride(winner);
            int before = race.Subgraphs.Count;
            var vanillaRace = fo4.Races.First(r => r.FormKey == raceKey);
            bool Same(ISubgraphGetter x, ISubgraphGetter y) =>
                x.BehaviorGraph == y.BehaviorGraph && x.Perspective == y.Perspective && x.Role == y.Role &&
                x.ActorKeywords.Select(k => k.FormKey).SequenceEqual(y.ActorKeywords.Select(k => k.FormKey)) &&
                x.TargetKeywords.Select(k => k.FormKey).SequenceEqual(y.TargetKeywords.Select(k => k.FormKey)) &&
                x.AnimationPaths.SequenceEqual(y.AnimationPaths, StringComparer.OrdinalIgnoreCase);
            // the winner's index of Fallout4.esm entry #ti (found by content, never by index), or -1
            int TemplateAt(int ti)
            {
                if (ti < 0 || ti >= vanillaRace.Subgraphs.Count) return -1;
                var tmpl = vanillaRace.Subgraphs[ti];
                for (int i = 0; i < race.Subgraphs.Count; i++)
                    if (!sgOurs.Contains(race.Subgraphs[i]) && Same(race.Subgraphs[i], tmpl)) return i;
                return -1;
            }
            int armsInserted = 0, armsAppended = 0;
            // ARMS entries go BEFORE their vanilla template (the gauntlet / PowerFist entry), so they sit ahead of the
            // vanilla catch-all entries (1P "MeleeBehavior, no keywords" = H2H) instead of after the whole table, the same
            // placement the PLS clones use. JSON order is kept (the leap / slam entries stay ahead of their plain entry).
            foreach (var e in entries)
            {
                var sub = new Subgraph
                {
                    BehaviorGraph = Str(e!, "behavior"),
                    Perspective = Str(e!, "perspective") == "first" ? Perspective.First : Perspective.Third,
                    Role = ParseRole(e!),
                };
                foreach (var k in Arr(e!, "actorKeywords")) sub.ActorKeywords.Add(new FormLink<IKeywordGetter>(KwRef(k!.GetValue<string>())));
                foreach (var k in Arr(e!, "weaponKeywords")) sub.TargetKeywords.Add(new FormLink<IKeywordGetter>(KwRef(k!.GetValue<string>())));
                foreach (var p in Arr(e!, "animPaths")) sub.AnimationPaths.Add(p!.GetValue<string>());
                int at = TemplateAt((int)Num(e!, "templateIndex", -1));
                if (at >= 0) { race.Subgraphs.Insert(at, sub); armsInserted++; }
                else { race.Subgraphs.Add(sub); armsAppended++; }
                sgOurs.Add(sub);
            }
            report.Add($"subgraphs: {raceKey} copied from {Path.GetFileName(basePath)} ({before} entries) + {entries.Count} Cyberware entries = {race.Subgraphs.Count} " +
                       $"({armsInserted} inserted before their vanilla template, {armsAppended} appended: template not found)");
            // PLS clones (ARMS, subgraphs_pls.json next to subgraphs.json): each is a vanilla entry + actor keyword
            // CW_PLSInstalled + a CyberPLS_<type> folder, inserted BEFORE its template so it wins while the keyword is on.
            // ARMS's templateIndex is into Fallout4.esm's list; the winner (another plugin's copy) may differ, so the
            // template is found in the winner by content (the Fallout4.esm entry at templateIndex), never by index.
            // every subgraphs_pls*.json (1P: subgraphs_pls.json; 3P player entries: subgraphs_pls_3p.json, ARMS)
            foreach (var plsPath in Directory.GetFiles(Path.GetDirectoryName(Path.GetFullPath(sgPath))!, "subgraphs_pls*.json").OrderBy(x => x, StringComparer.Ordinal))
            {
                int added = 0, moved = 0;
                foreach (var e in JsonNode.Parse(File.ReadAllText(plsPath))!["subgraphs"]!.AsArray())
                {
                    int ti = (int)Num(e!, "templateIndex", -1);
                    if (ti < 0 || ti >= vanillaRace.Subgraphs.Count) throw new GenError($"PLS clone {Str(e!, "id")}: templateIndex {ti} is outside Fallout4.esm's {vanillaRace.Subgraphs.Count} entries");
                    var tmpl = vanillaRace.Subgraphs[ti];
                    int at = -1;
                    for (int i = 0; i < race.Subgraphs.Count; i++)
                        if (!sgOurs.Contains(race.Subgraphs[i]) && Same(race.Subgraphs[i], tmpl)) { at = i; break; }
                    if (at < 0) throw new GenError($"PLS clone {Str(e!, "id")}: its Fallout4.esm template #{ti} ({tmpl.BehaviorGraph}) is not in the winning copy from {Path.GetFileName(basePath)}");
                    var sub = new Subgraph
                    {
                        BehaviorGraph = Str(e!, "behavior"),
                        Perspective = Str(e!, "perspective") == "first" ? Perspective.First : Perspective.Third,
                        Role = ParseRole(e!),
                    };
                    foreach (var k in Arr(e!, "actorKeywords")) sub.ActorKeywords.Add(new FormLink<IKeywordGetter>(KwRef(k!.GetValue<string>())));
                    foreach (var k in Arr(e!, "targetKeywords")) sub.TargetKeywords.Add(new FormLink<IKeywordGetter>(KwRef(k!.GetValue<string>())));
                    foreach (var p in Arr(e!, "animPaths")) sub.AnimationPaths.Add(p!.GetValue<string>());
                    if (sub.BehaviorGraph != tmpl.BehaviorGraph || sub.Role != tmpl.Role) throw new GenError($"PLS clone {Str(e!, "id")}: behavior/role differ from its template #{ti}");
                    int srcIdx = race.Subgraphs.Take(at).Count(x => !sgOurs.Contains(x));   // the template's index among the source's own entries
                    race.Subgraphs.Insert(at, sub);
                    sgOurs.Add(sub);
                    added++;
                    if (srcIdx != ti) moved++;
                }
                report.Add($"subgraphs: {Path.GetFileName(plsPath)}: {added} PLS clones (CW_PLSInstalled) inserted before their templates; {moved} template(s) sit at a different index in the winner than in Fallout4.esm");
            }
            FormKey KwRef(string s)
            {
                if (s.Contains('|')) return Fk.Parse(s);
                string e = s.StartsWith("CW_") ? s : "CW_" + s;   // bare names are keywords we create
                if (!forms.ContainsKey(e)) { var kw = mod.Keywords.AddNew(K(e)); kw.EditorID = e; kw.Type = Keyword.TypeEnum.None; }   // vanilla Anims* keywords are type None
                return new FormKey(Me, Convert.ToUInt32(forms[e], 16));
            }
        }

        // --- ARMS weapons + visual armor + leap keyword (arms/data/weapons_arms.json)
        var armsMap = new JsonObject();
        if (a.Opt("arms") is string armsPath)
        {
            var aj = JsonNode.Parse(File.ReadAllText(armsPath))!;
            var fo4Mk = ModKey.FromFileName("Fallout4.esm");
            var kwEdidById = fo4.Keywords.Where(k => k.EditorID != null).ToDictionary(k => k.FormKey, k => k.EditorID!);
            FormKey Kw(string s)
            {
                var tok = s.Trim().Split(' ')[0];
                if (tok.Contains('|')) return Fk.Parse(tok);
                string e = tok.StartsWith("CW_") ? tok : "CW_" + tok;
                if (!forms.ContainsKey(e)) { var kw = mod.Keywords.AddNew(K(e)); kw.EditorID = e; kw.Type = Keyword.TypeEnum.None; }
                return new FormKey(Me, Convert.ToUInt32(forms[e], 16));
            }
            var handlerOf = new Dictionary<string, string> { ["CW_MantisBlades"] = "ar_mantis", ["CW_GorillaArms"] = "ar_gorilla", ["CW_Monowire"] = "ar_monowire" };
            var weaponsOut = new JsonObject();
            var weaponKw = new Dictionary<string, FormKey>();   // WEAP EditorID -> its first CW_ anim keyword
            foreach (var w in Arr(aj, "weapons"))
            {
                string edid = Str(w, "edid"), tmpl = Str(w, "template");
                if (!tmpl.Contains(" WEAP ")) { report.Add($"arms: {edid} skipped ({Str(w, "status", "no WEAP template")})"); continue; }
                string tName = tmpl.Split(' ').Last();
                var t = fo4.Weapons.FirstOrDefault(x => string.Equals(x.EditorID, tName, StringComparison.OrdinalIgnoreCase))
                        ?? throw new GenError($"arms: template WEAP '{tName}' is not in Fallout4.esm");
                var nw = (Weapon)t.Duplicate(K(edid));
                nw.EditorID = edid;
                nw.Name = Str(w, "name", edid);
                nw.Keywords ??= new();
                foreach (var r in Arr(w, "keywords_remove"))
                {
                    var txt = r.GetValue<string>();
                    if (txt.Split(' ')[0].Contains('|')) { var fk = Fk.Parse(txt.Split(' ')[0]); nw.Keywords.RemoveAll(k => k.FormKey == fk); }
                    else   // "the template's AnimsPowerFist keyword": remove the template's Anims* keyword(s) except AnimsUnarmed
                        nw.Keywords.RemoveAll(k => kwEdidById.TryGetValue(k.FormKey, out var ke) && ke.StartsWith("anims", StringComparison.OrdinalIgnoreCase) && !ke.Equals("AnimsUnarmed", StringComparison.OrdinalIgnoreCase));
                }
                foreach (var ad in Arr(w, "keywords_add"))
                {
                    var fk = Kw(ad.GetValue<string>());
                    if (!nw.Keywords.Any(k => k.FormKey == fk)) nw.Keywords.Add(new FormLink<IKeywordGetter>(fk));
                    if (fk.ModKey == Me && !weaponKw.ContainsKey(edid)) weaponKw[edid] = fk;
                }
                string model = Str(w, "model");
                int note = model.IndexOf(" (");
                if (note > 0) model = model[..note];   // "Weapons\PowerFist\PowerFist.nif (placeholder until ...)"
                if (model == "") { nw.Model = null; nw.FirstPersonModel = null; nw.Flags |= Weapon.Flag.CannotDrop; }   // the blades are the visual ARMO
                else if (!string.Equals(model, t.Model?.File, StringComparison.OrdinalIgnoreCase)) { nw.Model = new Model { File = model }; nw.FirstPersonModel = null; }
                // else: the template's own world + first-person models stay
                if (w["stats_proposal"] is JsonObject st)
                {
                    if (st["damage"] != null) nw.BaseDamage = (ushort)Num(st, "damage", nw.BaseDamage);
                    if (st["speed"] != null) nw.Speed = (float)Num(st, "speed", nw.Speed);
                    if (st["reach"] != null) nw.Reach = (float)Num(st, "reach", nw.Reach);
                    if (st["critMult"] != null) nw.CritDamageMult = (float)Num(st, "critMult", nw.CritDamageMult);
                }
                nw.Value = 0; nw.Weight = 0; nw.Flags |= Weapon.Flag.CannotDrop;   // an implant, not loot: no selling, stashing or weight (variants copy it)
                mod.Weapons.Add(nw);
                string handler = Str(w, "handler", handlerOf.TryGetValue(edid, out var hh) ? hh : "");
                // One source of truth (BETA QA 2, 2026-11-10): the record carries the catalog's T5 base_dmg and speed, and the
                // plugin writes the installed tier's base_dmg at run time (arms::SetArmDamage). A weapons_arms.json that
                // disagrees with the catalog fails the build here; cwtest --validate reports the same lines offline.
                if (handler != "")
                {
                    JsonNode? armCatT5 = (cat["abilities"] as JsonArray ?? new JsonArray())
                        .FirstOrDefault(x => x?["spec"]?["native"] is JsonNode armNatNode && Str(armNatNode, "handler") == handler)?["spec"]?["native"]?["paramsT5"];
                    if (armCatT5 == null) throw new GenError($"arms: {edid}: the catalog has no weapon ability with handler {handler}");
                    double armCatDmg = Num(armCatT5, "base_dmg", -1), armCatSpd = Num(armCatT5, "speed", -1);
                    if (armCatDmg < 0 || Math.Abs(nw.BaseDamage - armCatDmg) > 1e-9)
                        throw new GenError($"arms: {edid} damage {nw.BaseDamage} (weapons_arms.json stats_proposal) but the catalog's T5 base_dmg for {handler} is {armCatDmg}: make them agree");
                    if (armCatSpd < 0 || Math.Abs(nw.Speed - armCatSpd) > 1e-4)
                        throw new GenError($"arms: {edid} speed {nw.Speed} (weapons_arms.json stats_proposal) but the catalog says {armCatSpd} for {handler}: make them agree");
                    double armCatReach = Num(armCatT5, "reach", -1);
                    if (armCatReach < 0 || Math.Abs(nw.Reach - armCatReach) > 1e-4)
                        throw new GenError($"arms: {edid} reach {nw.Reach} (weapons_arms.json stats_proposal) but the catalog says {armCatReach} for {handler}: make them agree");
                    report.Add($"arms: {edid} damage {nw.BaseDamage} speed {nw.Speed} reach {nw.Reach} = catalog T5 ({handler}); the installed tier's base_dmg is written at run time");
                }
                weaponsOut[edid] = new JsonObject { ["handler"] = handler, ["template"] = $"{t.FormKey}", ["animKeyword"] = weaponKw.TryGetValue(edid, out var wk) ? $"CW_{kwName(wk)}" : null };
                // Element variants (CP: an edge / knuckles / cable mod CONVERTS the arm's damage type): the same WEAP with base
                // damage 1 + the rest as typed damage (dtFire / dtPoison / dtElectrical) and a CW_Element<El> keyword. The plugin
                // holds the variant that matches the installed element mod.
                if (handler != "")
                {
                    var elOut = new JsonObject();
                    foreach (var (el, dt) in new[] { ("thermal", 0x060A82u), ("chemical", 0x060A84u), ("electrical", 0x060A80u) })
                    {
                        string El = char.ToUpperInvariant(el[0]) + el[1..];
                        var v = (Weapon)nw.Duplicate(K($"{edid}_{El}"));
                        v.EditorID = $"{edid}_{El}";
                        v.Name = $"{nw.Name} ({El})";
                        int total = Math.Max(2, (int)nw.BaseDamage);
                        v.BaseDamage = 1;
                        v.DamageTypes = new ExtendedList<WeaponDamageType> { new WeaponDamageType { DamageType = new FormLink<IDamageTypeGetter>(new FormKey(fo4Mk, dt)), Value = (uint)(total - 1) } };
                        var ek = Kw("CW_Element" + El);
                        if (!v.Keywords!.Any(k => k.FormKey == ek)) v.Keywords.Add(new FormLink<IKeywordGetter>(ek));
                        mod.Weapons.Add(v);
                        elOut[el] = v.EditorID;
                    }
                    weaponsOut[edid]!["elements"] = elOut;
                    report.Add($"arms: {edid} element WEAPs thermal/chemical/electrical: base 1 + {Math.Max(2, (int)nw.BaseDamage) - 1} typed damage");
                }
                report.Add($"arms: WEAP {edid} from {tName} ({t.FormKey}), handler {handler}, keywords {nw.Keywords.Count}, model {(model == "" ? "none (visual armor)" : model)}");
            }
            string kwName(FormKey fk) => forms.First(f => f.Value == fk.ID.ToString("X3")).Key[3..];
            var visualsOut = new JsonObject();
            // Element looks (the lead's queue item b): per element, one MSWP + a copy of each ARMA whose models carry it + an ARMO
            // variant `<armo>_<Element>`. The plugin wears the variant that matches the installed edge / knuckles / round mod.
            var elementOut = new JsonObject();
            string NormEl(string k)
            {
                string e = k.ToLowerInvariant();
                return e.StartsWith("electr") ? "electrical" : e.StartsWith("therm") ? "thermal" : e.StartsWith("toxic") || e.StartsWith("chem") ? "chemical"
                     : e.StartsWith("explos") ? "explosive" : e.StartsWith("tranq") ? "tranq" : e;
            }
            string Mat(string p) { p = p.Trim(); return p.StartsWith("Materials\\", StringComparison.OrdinalIgnoreCase) ? p[10..] : p; }   // MSWP paths are relative to Materials
            void ElementVariants(Armor armo, List<ArmorAddon> aas, JsonObject? swaps, string defaultFrom)
            {
                if (swaps == null) return;
                var subs = new SortedDictionary<string, List<(string from, string to)>>(StringComparer.Ordinal);
                string fromDir = defaultFrom.Contains('\\') ? defaultFrom[..(defaultFrom.LastIndexOf('\\') + 1)] : "";
                foreach (var (k, v) in swaps)
                {
                    if (k == "from" || v is not JsonValue) continue;
                    string txt = v.GetValue<string>();
                    string el = NormEl(k.Split('_')[0]);
                    string from = defaultFrom, to = txt;
                    if (txt.Contains("->")) { var parts = txt.Split("->"); from = parts[0]; to = parts[1]; }
                    else if (!to.Contains('\\')) to = fromDir + to;
                    (subs.TryGetValue(el, out var l) ? l : subs[el] = new()).Add((Mat(from), Mat(to)));
                }
                var map = new JsonObject();
                foreach (var (el, list) in subs)
                {
                    string El = char.ToUpperInvariant(el[0]) + el[1..];
                    var ms = mod.MaterialSwaps.AddNew(K($"CW_MSWP_{armo.EditorID![3..]}_{El}"));
                    ms.EditorID = $"CW_MSWP_{armo.EditorID![3..]}_{El}";
                    foreach (var (f, t) in list) ms.Substitutions.Add(new MaterialSubstitution { OriginalMaterial = f, ReplacementMaterial = t });
                    Model? Sw(IModelGetter? m) => m == null ? null : new Model { File = m.File, MaterialSwap = new FormLinkNullable<IMaterialSwapGetter>(ms.FormKey) };
                    var va = mod.Armors.AddNew(K($"{armo.EditorID}_{El}"));
                    va.EditorID = $"{armo.EditorID}_{El}"; va.Name = armo.Name; va.MajorFlags = armo.MajorFlags;
                    va.BipedBodyTemplate = new BipedBodyTemplate { FirstPersonFlags = armo.BipedBodyTemplate!.FirstPersonFlags };
                    va.Race = new FormLinkNullable<IRaceGetter>(armo.Race.FormKey);
                    foreach (var a0 in aas)
                    {
                        var ca = mod.ArmorAddons.AddNew(K($"{a0.EditorID}_{El}"));
                        ca.EditorID = $"{a0.EditorID}_{El}";
                        ca.BodyTemplate = new BodyTemplate { FirstPersonFlags = a0.BodyTemplate!.FirstPersonFlags };
                        ca.Race = new FormLinkNullable<IRaceGetter>(a0.Race.FormKey);
                        ca.WorldModel = new GenderedItem<Model?>(Sw(a0.WorldModel?.Male), Sw(a0.WorldModel?.Female));
                        ca.FirstPersonModel = new GenderedItem<Model?>(Sw(a0.FirstPersonModel?.Male), Sw(a0.FirstPersonModel?.Female));
                        va.Armatures.Add(new ArmorAddonModel { AddonIndex = 0, ArmorAddon = new FormLinkNullable<IArmorAddonGetter>(ca.FormKey) });
                    }
                    map[el] = va.EditorID;
                }
                elementOut[armo.EditorID!] = map;
                report.Add($"arms: {armo.EditorID} element looks: {string.Join(", ", subs.Select(s => $"{s.Key} ({s.Value.Count} swap{(s.Value.Count == 1 ? "" : "s")})"))}");
            }
            foreach (var ar in Arr(aj, "armor"))
            {
                string edid = Str(ar, "edid");
                var slotTxt = Str(ar, "bipedSlot", "58");
                var sm = System.Text.RegularExpressions.Regex.Match(slotTxt, @"\b(3\d|4\d|5\d|6[01])\b");
                int slot = sm.Success ? int.Parse(sm.Groups[1].Value) : 58;
                var flag = (BipedObjectFlag)(1u << (slot - 30));
                var models = ar["arma_models"] as JsonObject ?? new JsonObject();
                Model? M(string k) => Str(models, k) is var f && f != "" ? new Model { File = f } : null;
                var aa = mod.ArmorAddons.AddNew(K(edid + "_AA"));
                aa.EditorID = edid + "_AA";
                aa.BodyTemplate = new BodyTemplate { FirstPersonFlags = flag };
                aa.Race = new FormLinkNullable<IRaceGetter>(new FormKey(fo4Mk, 0x013746));   // HumanRace
                aa.WorldModel = new GenderedItem<Model?>(M("male3rd"), M("female3rd"));
                aa.FirstPersonModel = new GenderedItem<Model?>(M("male1st"), M("female1st"));
                var armo = mod.Armors.AddNew(K(edid));
                armo.EditorID = edid;
                armo.Name = Str(ar, "name", edid);
                armo.MajorFlags |= Armor.MajorFlag.NonPlayable;
                armo.BipedBodyTemplate = new BipedBodyTemplate { FirstPersonFlags = flag };
                armo.Race = new FormLinkNullable<IRaceGetter>(new FormKey(fo4Mk, 0x013746));
                armo.Armatures.Add(new ArmorAddonModel { AddonIndex = 0, ArmorAddon = new FormLinkNullable<IArmorAddonGetter>(aa.FormKey) });
                string housing = "";
                if (ar["arma_housing"] is JsonObject hj && Str(hj, "edid") != "")   // ARMS: a second ARMA on the same ARMO (Mantis forearm housing)
                {
                    housing = Str(hj, "edid");
                    var hm = hj["models"] as JsonObject ?? new JsonObject();
                    Model? HM(string k) => Str(hm, k) is var f && f != "" ? new Model { File = f } : null;
                    // its own biped slot: a slot holds ONE ARMA, so a second ARMA on the blades' slot is never attached (in game 2026-11-08)
                    var hsm = System.Text.RegularExpressions.Regex.Match(Str(hj, "bipedSlot", slot.ToString()), @"\b(3\d|4\d|5\d|6[01])\b");
                    var hflag = (BipedObjectFlag)(1u << ((hsm.Success ? int.Parse(hsm.Groups[1].Value) : slot) - 30));
                    var ha = mod.ArmorAddons.AddNew(K(housing));
                    ha.EditorID = housing;
                    ha.BodyTemplate = new BodyTemplate { FirstPersonFlags = hflag };
                    armo.BipedBodyTemplate!.FirstPersonFlags |= hflag;
                    ha.Race = new FormLinkNullable<IRaceGetter>(new FormKey(fo4Mk, 0x013746));
                    ha.WorldModel = new GenderedItem<Model?>(HM("male3rd"), HM("female3rd"));
                    ha.FirstPersonModel = new GenderedItem<Model?>(HM("male1st"), HM("female1st"));
                    armo.Armatures.Add(new ArmorAddonModel { AddonIndex = 0, ArmorAddon = new FormLinkNullable<IArmorAddonGetter>(ha.FormKey) });
                }
                var aaList = new List<ArmorAddon> { aa };
                if (housing != "") aaList.Add(mod.ArmorAddons.First(x => x.EditorID == housing));
                var mats = ar["materials"] as JsonObject;
                ElementVariants(armo, aaList, mats?["element_swaps_MSWP"] as JsonObject, mats != null ? Str(mats, "default") : "");
                // which drawn weapon shows it: the WEAP whose EditorID is this one without "Visual" (CW_MantisBlades)
                string weap = edid.EndsWith("Visual") ? edid[..^6] : "";
                if (weap != "" && weaponKw.TryGetValue(weap, out var vk)) visualsOut["CW_" + kwName(vk)] = edid;
                report.Add($"arms: ARMO {edid} + ARMA {edid}_AA{(housing != "" ? " + " + housing : "")} on biped slots 0x{(uint)armo.BipedBodyTemplate!.FirstPersonFlags:X8} (non-playable), shown while {(weap != "" ? weap : "?")} is drawn");
            }
            // Installed visuals (worn the whole time the piece is installed, not only while drawn): ARMS weapons[].visual for
            // the Gorilla knuckles (5dcc2473) and the PLS launcher (b1418275). One ARMO + one ARMA, HumanRace, non-playable.
            var installedOut = new JsonObject();
            // drawnWeapon != "": a drawn visual like the blades (on at weaponDraw while that WEAP is out), else worn while installed
            string ArmoFrom(JsonObject vis, string handler, string drawnWeapon = "")
            {
                var ids = System.Text.RegularExpressions.Regex.Matches(Str(vis, "edid"), @"CW_\w+").Select(m => m.Value).ToList();
                if (ids.Count < 2) throw new GenError($"arms: visual for {handler}: need 'CW_<armo> (ARMO) + CW_<arma> (ARMA)', got '{Str(vis, "edid")}'");
                string armoId = ids[0], aaId = ids[1];
                string mdl = Str(vis, "models").Split(" for ")[0].Trim();
                var sm = System.Text.RegularExpressions.Regex.Match(Str(vis, "bipedSlot", "58"), @"\b(3\d|4\d|5\d|6[01])\b");
                int slot = sm.Success ? int.Parse(sm.Groups[1].Value) : 58;
                var flag = (BipedObjectFlag)(1u << (slot - 30));
                var aa = mod.ArmorAddons.AddNew(K(aaId));
                aa.EditorID = aaId;
                aa.BodyTemplate = new BodyTemplate { FirstPersonFlags = flag };
                aa.Race = new FormLinkNullable<IRaceGetter>(new FormKey(fo4Mk, 0x013746));   // HumanRace
                // "model3p": a separate third-person model (the PLS launcher's flared Launcher3P.nif); 1P keeps "models"
                string mdl3p = Str(vis, "model3p", mdl);
                aa.WorldModel = new GenderedItem<Model?>(new Model { File = mdl3p }, new Model { File = mdl3p });
                aa.FirstPersonModel = new GenderedItem<Model?>(new Model { File = mdl }, new Model { File = mdl });
                var armo = mod.Armors.AddNew(K(armoId));
                armo.EditorID = armoId;
                armo.Name = Str(vis, "name", armoId);   // shown by any "item added/equipped" notice, so a player-facing name
                armo.MajorFlags |= Armor.MajorFlag.NonPlayable;
                armo.BipedBodyTemplate = new BipedBodyTemplate { FirstPersonFlags = flag };
                armo.Race = new FormLinkNullable<IRaceGetter>(new FormKey(fo4Mk, 0x013746));
                armo.Armatures.Add(new ArmorAddonModel { AddonIndex = 0, ArmorAddon = new FormLinkNullable<IArmorAddonGetter>(aa.FormKey) });
                if (drawnWeapon == "") installedOut[handler] = armoId;
                else if (weaponKw.TryGetValue(drawnWeapon, out var dk)) visualsOut["CW_" + kwName(dk)] = armoId;
                else throw new GenError($"arms: drawn visual {armoId}: {drawnWeapon} has no CW_ anim keyword");
                var mv = vis["materials_MSWP"] as JsonObject;
                ElementVariants(armo, new List<ArmorAddon> { aa }, mv, mv != null ? Str(mv, "from") : "");
                report.Add($"arms: ARMO {armoId} + ARMA {aaId} ({mdl}{(mdl3p != mdl ? ", 3P " + mdl3p : "")}) on biped slot {slot} (non-playable), " +
                           (drawnWeapon == "" ? $"worn while {handler} is installed" : $"shown while {drawnWeapon} is drawn"));
                return armoId;
            }
            foreach (var w in Arr(aj, "weapons"))
            {
                if (Str(w, "edid") == "CW_GorillaArms" && w["visual"] is JsonObject gv) ArmoFrom(gv, "ar_gorilla");
                if (Str(w, "edid") == "CW_Monowire" && w["visual"] is JsonObject mw) ArmoFrom(mw, "ar_monowire", "CW_Monowire");   // ARMS
            }

            // PLS (the lead's option c: exclusive Arms slot). A grenade-slot WEAP (copy of fragGrenade: its throw keywords play
            // ARMS's WPNGrenadeThrow), a straight missile projectile, one explosion per CP round (vanilla stand-ins), and the
            // actor keyword CW_PLSInstalled. Charges = how many of the WEAP the player holds: the plugin tops it up.
            var plsOut = new JsonObject();
            var plsW = Arr(aj, "weapons").FirstOrDefault(x => Str(x, "edid") == "CW_ProjectileLaunchSystem");
            if (plsW?["record"] is JsonObject)
            {
                JsonNode? plsNat = (cat["abilities"] as JsonArray ?? new JsonArray()).FirstOrDefault(x => x != null && Str(x, "id") == "ab_ar_pls")?["spec"]?["native"]?["paramsT5"];
                double dmg = plsNat != null ? Num(plsNat, "base_dmg", 120) : 120, radM = plsNat != null ? Num(plsNat, "explosion_radius_m", 3) : 3;
                float outer = (float)(radM * 70.0), inner = outer * 0.4f;   // ~70 units per metre
                // HOW each round's explosion is built (vanilla stand-in, lethal). WHICH rounds exist is the catalog's (ARMORY, the lead
                // 2026-11-10): one explosion per cwm_pls_round_* mod, named by its ability's spec.native.paramsT5.round, the same field
                // cw::PlsRounds reads for the plugin's handlers. A dropped round leaves no orphan CW_PLSExplosion. explosive is always
                // built: it is CW_PLSProjectile's default explosion, not only a mod. A catalog round this table cannot build fails here.
                var roundBuild = new (string round, string vanilla, bool lethal)[] {
                    ("explosive", "fragGrenadeExplosion", true), ("electrical", "pulseGrenadeExplosion", true), ("thermal", "ExplosionMolotovCocktail", true),
                    ("incendiary", "ExplosionMolotovCocktail", true), ("chemical", "ExplosionTrapGas", true), ("tranq", "cryoGrenadeExplosion", false) };
                var catAbilities = new Dictionary<string, JsonNode>(StringComparer.Ordinal);
                foreach (var ab in Arr(cat, "abilities")) catAbilities[Str(ab, "id")] = ab;
                var catRounds = new HashSet<string>(StringComparer.Ordinal) { "explosive" };
                foreach (var m in Arr(cat, "mods"))
                {
                    string mid = Str(m, "id");
                    if (!mid.StartsWith("cwm_pls_round_", StringComparison.Ordinal)) continue;
                    string r = "";
                    foreach (var t in Arr(m, "tiers"))
                        foreach (var an in Arr(t, "abilities"))
                            if (an is JsonValue av && av.TryGetValue<string>(out var aid) && catAbilities.TryGetValue(aid, out var ab) &&
                                ab["spec"]?["native"] is JsonObject nat && Str(nat, "handler").StartsWith("mod_pls_round_", StringComparison.Ordinal) &&
                                nat["paramsT5"] is JsonObject p5 && Str(p5, "round") != "")
                                r = Str(p5, "round");
                    if (r == "") throw new GenError($"arms: PLS round mod {mid} has no ability with a mod_pls_round_* handler and a paramsT5.round");
                    if (!roundBuild.Any(b => b.round == r)) throw new GenError($"arms: PLS round '{r}' ({mid}) has no explosion template in espgen's roundBuild table");
                    catRounds.Add(r);
                }
                var rounds = roundBuild.Where(b => catRounds.Contains(b.round)).ToArray();
                report.Add($"arms: PLS rounds from the catalog: {string.Join(", ", rounds.Select(b => b.round))}");
                var explOut = new JsonObject();
                FormKey explDefault = default;
                foreach (var (round, vtmpl, lethal) in rounds)
                {
                    var v = fo4.Explosions.FirstOrDefault(x => string.Equals(x.EditorID, vtmpl, StringComparison.OrdinalIgnoreCase))
                            ?? throw new GenError($"arms: PLS explosion template '{vtmpl}' is not in Fallout4.esm");
                    string id = "CW_PLSExplosion_" + char.ToUpperInvariant(round[0]) + round[1..];
                    var x = (Explosion)v.Duplicate(K(id));
                    x.EditorID = id;
                    x.Damage = lethal ? (float)dmg : 0f;   // tranq: non-lethal (the cryo freeze stands in for "unconscious"). T5 value: the
                                                           // plugin writes the installed tier's base_dmg at run time (arms::SetPlsDamage)
                    x.OuterRadius = outer; x.InnerRadius = inner;
                    // the round's status (CP: burn / poison / shock-stun): a vanilla enchantment on the explosion. Tranq keeps the
                    // cryo template's own freeze ENCH. FO4 explosion enchantments always apply (CP's 20% chance is not kept).
                    uint ench = round switch { "chemical" => 0x085700u, "thermal" or "incendiary" => 0x1B88D2u, "electrical" => 0x0C4670u, _ => 0u };
                    if (ench != 0) x.ObjectEffect = new FormLinkNullable<IObjectEffectGetter>(new FormKey(fo4Mk, ench));
                    mod.Explosions.Add(x);
                    explOut[round] = id;
                    if (round == "explosive") explDefault = x.FormKey;
                    report.Add($"arms: EXPL {id} from {vtmpl} ({v.FormKey}), damage {x.Damage}, radius {inner:0}/{outer:0}");
                }
                var mp = fo4.Projectiles.First(x => x.EditorID == "MissileProjectile");
                var proj = (Projectile)mp.Duplicate(K("CW_PLSProjectile"));
                proj.EditorID = "CW_PLSProjectile";
                proj.Speed = 3000; proj.Gravity = 0; proj.Range = 4000;
                proj.Explosion = new FormLinkNullable<IExplosionGetter>(explDefault);
                mod.Projectiles.Add(proj);
                var fg = fo4.Weapons.First(x => x.EditorID == "fragGrenade");
                var pw = (Weapon)fg.Duplicate(K("CW_ProjectileLaunchSystem"));
                pw.EditorID = "CW_ProjectileLaunchSystem";
                pw.Name = Str(plsW, "name", "Projectile Launch System");
                pw.Value = 0; pw.Weight = 0;
                pw.Flags |= Weapon.Flag.CannotDrop;
                if (pw.ExtraData != null) pw.ExtraData.ProjectileOverride = new FormLinkNullable<IProjectileGetter>(proj.FormKey);
                else throw new GenError("arms: fragGrenade has no extra data to point at CW_PLSProjectile");
                mod.Weapons.Add(pw);
                Kw("CW_PLSInstalled");
                string vis = plsW["visual"] is JsonObject pv ? ArmoFrom(pv, "ar_pls") : "";
                plsOut["weapon"] = "CW_ProjectileLaunchSystem"; plsOut["projectile"] = "CW_PLSProjectile"; plsOut["keyword"] = "CW_PLSInstalled";
                plsOut["explosions"] = explOut; plsOut["visual"] = vis;
                report.Add($"arms: WEAP CW_ProjectileLaunchSystem from fragGrenade ({fg.FormKey}) -> CW_PLSProjectile (MissileProjectile copy, speed 3000, gravity 0, range 4000), keyword CW_PLSInstalled");
            }

            // Gorilla ground slam (ARMS): the plugin adds CW_GorillaSlamReady while the Gorilla Arms are drawn in the
            // air (it selects cybergorillaslam_1p), and at HitFrame (or on landing) damages + knocks back hostiles around the
            // player. The shockwave VISUAL (the lead, 2026-11-08): CW_GorillaSlamShockwave, a copy of the Behemoth's ground slam
            // explosion with damage and force 0 (the plugin deals the damage and Limiter Removal the knockdown), placed at the
            // player's feet through TESDataHandler::CreateReferenceAtLocation (NEW_REFR_DATA is complete in a08c731 after all).
            var slamOut = new JsonObject();
            if (aj["groundSlam"] is JsonObject gs)
            {
                string sk = Str(gs, "keyword").Split(' ')[0];
                Kw(sk);
                slamOut["keyword"] = sk.StartsWith("CW_") ? sk : "CW_" + sk;
                slamOut["weaponKeyword"] = weaponKw.TryGetValue("CW_GorillaArms", out var gk) ? "CW_" + kwName(gk) : null;
                slamOut["radiusUnits"] = 350; slamOut["damage"] = 60; slamOut["knock"] = 1.0;
                var bs = fo4.Explosions.FirstOrDefault(x => string.Equals(x.EditorID, "BehemothSlamExplosion", StringComparison.OrdinalIgnoreCase))
                         ?? throw new GenError("arms: slam explosion template 'BehemothSlamExplosion' is not in Fallout4.esm");
                var sx = (Explosion)bs.Duplicate(K("CW_GorillaSlamShockwave"));
                sx.EditorID = "CW_GorillaSlamShockwave";
                sx.Damage = 0f; sx.Force = 0f; sx.OuterRadius = 350f; sx.InnerRadius = 140f;
                sx.Sound1.SetToNull(); sx.Sound2.SetToNull();   // the code plays CW_SND_GorillaSlam / CW_SND_MantisLand: no vanilla boom on top (sound/WIRING-SPEC.md)
                mod.Explosions.Add(sx);
                slamOut["explosion"] = "CW_GorillaSlamShockwave";
                report.Add($"arms: EXPL CW_GorillaSlamShockwave from BehemothSlamExplosion ({bs.FormKey}), damage 0, force 0, radius 140/350");
            }
            // Finishers (ARMS, arms/data/finishers.json next to --arms; the lead: approved, behind bFinishers). Three
            // dynamic idles per finisher: dyn_Activation plays the IDLE's own AnimationFile, so lead and victim keep their own
            // lengths. Templates: IdleLookAtOwnOutfit (1P RootBehavior) and IdleOutroPlayerWalk (3P RaiderRootBehavior):
            // no conditions, no parent; the plugin starts them with PlayIdle, so they sit outside the idle tree.
            var finOut = new JsonObject();
            string finPath = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(armsPath))!, "finishers.json");
            if (File.Exists(finPath))
            {
                var fj = JsonNode.Parse(File.ReadAllText(finPath))!;
                var t1 = fo4.IdleAnimations.First(x => x.EditorID == "IdleLookAtOwnOutfit");
                var t3 = fo4.IdleAnimations.First(x => x.EditorID == "IdleOutroPlayerWalk");
                var setKw = new Dictionary<string, string> { ["mantisblade"] = "CW_MantisBlades", ["monowire"] = "CW_Monowire", ["strongarms"] = "CW_GorillaArms" };
                IdleAnimation Idle(IIdleAnimationGetter tmpl, string id, string file)
                {
                    var i = (IdleAnimation)tmpl.Duplicate(K(id));
                    i.EditorID = id;
                    i.AnimationFile = "Actors\\Character\\" + file;
                    i.Conditions.Clear();   // the templates have none; parent/previous stay the template's two nulls (vanilla ANAM shape)
                    mod.IdleAnimations.Add(i);
                    return i;
                }
                foreach (var (fid, f) in fj["finishers"] as JsonObject ?? new JsonObject())
                {
                    if (f == null) continue;
                    var files = f["files"]!;
                    string set = Str(f, "set");
                    if (!setKw.TryGetValue(set, out var weap) || !weaponKw.TryGetValue(weap, out var wk)) throw new GenError($"finisher {fid}: set '{set}' has no arm WEAP keyword");
                    Idle(t1, $"CW_Finisher_{fid}_Lead1P", Str(files, "lead1p"));
                    Idle(t3, $"CW_Finisher_{fid}_Lead3P", Str(files, "lead3p"));
                    Idle(t3, $"CW_Finisher_{fid}_Victim", Str(files, "victim3p"));
                    var use = f["victimPlacement"]?["use"];
                    finOut[fid] = new JsonObject
                    {
                        ["weaponKeyword"] = "CW_" + kwName(wk), ["lead1p"] = $"CW_Finisher_{fid}_Lead1P", ["lead3p"] = $"CW_Finisher_{fid}_Lead3P",
                        ["victim"] = $"CW_Finisher_{fid}_Victim", ["leadSeconds"] = Num(f, "leadSeconds", 3), ["victimSeconds"] = Num(f, "victimSeconds", 3),
                        ["killSeconds"] = Num(f, "killSeconds", 2), ["x"] = use != null ? Num(use, "x", 0) : 0, ["y"] = use != null ? Num(use, "y", 30) : 30,
                        ["yawDeg"] = use != null ? Num(use, "yawDeg", 180) : 180,
                    };
                    report.Add($"arms: finisher {fid} ({set}): IDLEs CW_Finisher_{fid}_Lead1P/_Lead3P/_Victim (dyn_Activation), victim at ({finOut[fid]!["x"]}, {finOut[fid]!["y"]})");
                }
            }
            armsMap["finishers"] = finOut;
            armsMap["installedVisuals"] = installedOut;
            armsMap["elementVisuals"] = elementOut;
            armsMap["pls"] = plsOut;
            armsMap["slam"] = slamOut;

            // Combat feel (the author, 2026-11-08): the plugin reads arms.combat.gorillaKnockback.* (arms::CombatCfg). The
            // keywords are already in CW_ form (they match the WEAP keywords the HitSink collects), so pass it through.
            if (aj["combat"] is JsonObject combatJson) armsMap["combat"] = combatJson.DeepClone();

            var leap = aj["leap"] as JsonObject;
            string leapKw = leap != null ? Str(leap, "keyword").Split(' ')[0] : "";
            if (leapKw != "") Kw(leapKw);
            armsMap["weapons"] = weaponsOut;
            armsMap["visuals"] = visualsOut;
            armsMap["leapKeyword"] = leapKw == "" ? null : (leapKw.StartsWith("CW_") ? leapKw : "CW_" + leapKw);
            armsMap["leapWeaponKeyword"] = weaponKw.TryGetValue("CW_MantisBlades", out var mk) ? "CW_" + kwName(mk) : null;
        }

        // --- SOUND: SNDR records from data/sound/sounds.json (band 0xFE0-0xFFF; see sound/README.md).
        // Category + output model are vanilla records linked by EditorID, so a typo fails the build here,
        // not silently in game. Sound files are staged by the build door; espgen only writes the paths.
        // --- SOUND: SNDR records from data/sound/sounds.json. The lead, 2026-11-09: Cyberware.esp
        // carries at most the 32 ids in SOUND's band 0xFE0-0xFFF ("plugin": "core" slots: the ones
        // Cyberware.esp records reference - WEAP/EXPL/MGEF wiring). Everything else goes in the
        // separate ESL "Cyberware - Sound.esp" (own lock, full 0x800-0xFFF), which Cyberware.esp
        // must never gain a master on; the DLL resolves those by plugin+id from formmap.
        var soundsMap = new JsonObject();
        Fallout4Mod? modSnd = null;
        IdLock? lkSnd = null;
        string? sndLockPath = null;
        if (a.Opt("sounds") is string sndPath)
        {
            const uint SndMin = 0xFE0, SndMax = 0xFFF;   // core band inside Cyberware.esp
            var SndKey = ModKey.FromFileName("Cyberware - Sound.esp");
            sndLockPath = a.Opt("sound-lock") ?? Path.Combine(Path.GetDirectoryName(Path.GetFullPath(sndPath))!, "formids.sound.lock.json");
            var snd = JsonNode.Parse(File.ReadAllText(sndPath))!.AsObject();
            var scatByEdid = new Lazy<Dictionary<string, FormKey>>(() => fo4.SoundCategories.Where(r => r.EditorID != null).ToDictionary(r => r.EditorID!, r => r.FormKey, StringComparer.OrdinalIgnoreCase));
            var sopmByEdid = new Lazy<Dictionary<string, FormKey>>(() => fo4.SoundOutputModels.Where(r => r.EditorID != null).ToDictionary(r => r.EditorID!, r => r.FormKey, StringComparer.OrdinalIgnoreCase));
            int nCore = 0, nExt = 0;
            foreach (var slot in Arr(snd, "slots"))
            {
                if (Str(slot, "status") == "dropped") continue;
                if (Str(slot, "plugin", "sound") == "armory") continue;   // hosted by "Cyberware - Armory.esp" (Armory.cs), where its WEAP lives
                var files = Arr(slot, "files").Select(f => f!.GetValue<string>()).ToList();
                if (files.Count == 0) continue;   // candidate slots without cut files make no record yet
                string edid = Str(slot, "id");
                if (!edid.StartsWith("CW_SND_")) throw new GenError($"sounds: slot id '{edid}' must start with CW_SND_");
                bool core = Str(slot, "plugin", "sound") == "core";
                SoundDescriptor r;
                if (core)
                {
                    var fkey = new FormKey(Me, lk.Get(edid, SndMin, SndMax));
                    forms[edid] = fkey.ID.ToString("X3");
                    r = mod.SoundDescriptors.AddNew(fkey);
                    nCore++;
                }
                else
                {
                    if (modSnd == null)
                    {
                        modSnd = new Fallout4Mod(SndKey, Fallout4Release.Fallout4);
                        modSnd.ModHeader.Flags |= Fallout4ModHeader.HeaderFlag.Small;
                        modSnd.ModHeader.Author = "Cyberware SOUND generator";
                        modSnd.ModHeader.Description = "Cyberware sound descriptors (overflow plugin; no other Cyberware plugin masters this). Generated, do not edit.";
                        lkSnd = IdLock.Load(sndLockPath);
                        lkSnd.Frozen = lk.Frozen;
                    }
                    r = modSnd.SoundDescriptors.AddNew(new FormKey(SndKey, lkSnd!.Get(edid)));
                    nExt++;
                }
                r.EditorID = edid;
                Sndr.Fill(r, slot, scatByEdid.Value, sopmByEdid.Value);
                soundsMap[Str(slot, "slot")] = new JsonObject
                {
                    ["edid"] = edid,
                    ["plugin"] = core ? "Cyberware.esp" : SndKey.FileName.String,
                    ["id"] = r.FormKey.ID.ToString("X3"),
                };
            }
            report.Add($"sounds: {nCore} core SNDR in band 0x{SndMin:X3}-0x{SndMax:X3}, {nExt} in Cyberware - Sound.esp, from {Path.GetFileName(sndPath)}");
        }

        // --- write + verify by reading back
        string espPath = Path.Combine(outDir, "Cyberware.esp");
        // Masters in game load order: vanilla first, then the profile's plugins.txt order (--load-order), then by name.
        var vanilla = new[] { "Fallout4.esm", "DLCRobot.esm", "DLCworkshop01.esm", "DLCCoast.esm", "DLCworkshop02.esm", "DLCworkshop03.esm", "DLCNukaWorld.esm", "DLCUltraHighResolution.esm" }.ToList();
        var lo = a.Opt("load-order") is string lop ? File.ReadAllLines(lop).Where(l => l.StartsWith('*')).Select(l => l[1..]).ToList() : new List<string>();
        var needed = mod.EnumerateFormLinks().Select(l => l.FormKey.ModKey)
            .Concat(mod.EnumerateMajorRecords().Select(r => r.FormKey.ModKey))
            .Where(k => k != Me && !k.IsNull).Distinct().ToList();
        int Rank(ModKey k) { int i = vanilla.FindIndex(x => x.Equals(k.FileName, StringComparison.OrdinalIgnoreCase)); if (i >= 0) return i; i = lo.FindIndex(x => x.Equals(k.FileName, StringComparison.OrdinalIgnoreCase)); return i >= 0 ? 100 + i : 100000; }
        var masters = needed.OrderBy(Rank).ThenBy(k => k.FileName.String).ToList();
        mod.WriteToBinary(espPath, new BinaryWriteParameters
        {
            MastersListOrdering = new MastersListOrderingByLoadOrder(masters),
        });
        if (sgPath != null)
        {
            int n = SubgraphFixup.Fix(espPath);
            report.Add($"subgraphs: reordered SGNM after SAKD/STKD in {n} RACE record(s) (Mutagen write order != vanilla)");
            var sgKey = Fk.Parse(JsonNode.Parse(File.ReadAllText(sgPath))!["subgraphs"]!.AsArray().Select(e => Str(e!, "race", "")).FirstOrDefault(r => r != "") ?? "Fallout4.esm|166729");
            var oursAt = new HashSet<int>();
            var finalRace = mod.Races.First(r => r.FormKey == sgKey);
            for (int i = 0; i < finalRace.Subgraphs.Count; i++) if (sgOurs.Contains(finalRace.Subgraphs[i])) oursAt.Add(i);
            report.Add(SubgraphFixup.Verify(espPath, a.Req("subgraph-base"), sgKey.ID, oursAt));
        }
        {
            // A response's "no emotion": Mutagen writes 00000000, vanilla FFFFFFFF (EmotionFixup.cs). The dialogue guard's
            // info is the one response this plugin holds; the assert makes a null emotion a build failure from now on.
            int ne = EmotionFixup.Fix(espPath);
            if (ne > 0) report.Add($"dialogue: TRDA emotion 00000000 -> FFFFFFFF in {ne} response(s) (a null FormLink; vanilla's \"no emotion\" is FFFFFFFF)");
            EmotionFixup.AssertNone(espPath);
            // Force Subtitle on every ripperdoc dialogue INFO: the lines are text only in this update, so a player with
            // dialogue subtitles off would otherwise read nothing, and the DLL recognises the shop choice by that INFO
            // appearing in the SubtitleManager. Both calls throw rather than ship a line nobody can read.
            if (rdDialogueInfos.Count > 0)
            {
                var rdIds = rdDialogueInfos.Select(x => x.FormKey.ID).ToList();
                int nf = EmotionFixup.ForceSubtitleOn(espPath, rdIds);
                EmotionFixup.AssertForcedSubtitle(espPath, rdIds);
                report.Add($"ripperdoc talk: Force Subtitle (ENAM 0x0200) on {nf} INFO(s) [{string.Join(" ", rdIds.Select(i => i.ToString("X3")))}]");
            }
        }
        using (var back = Fallout4Mod.CreateFromBinaryOverlay(espPath, Fallout4Release.Fallout4))
        {
            int n = back.EnumerateMajorRecords().Count();
            bool esl = back.ModHeader.Flags.HasFlag(Fallout4ModHeader.HeaderFlag.Small);
            var ids = back.EnumerateMajorRecords().Where(r => r.FormKey.ModKey == Me).Select(r => r.FormKey.ID).ToList();
            if (!esl) throw new GenError("written plugin is not ESL-flagged");
            if (ids.Any(i => i < IdLock.Min || i > IdLock.Max)) throw new GenError("a new record is outside 0x800-0xFFF");
            report.Insert(0, $"wrote {espPath}: ESL, {n} records ({ids.Count} new, {n - ids.Count} overrides), masters [{string.Join(", ", back.ModHeader.MasterReferences.Select(m => m.Master.FileName))}], {statPerks} stat-bit perks");
        }
        if (!lk.Frozen) lk.Save(lockPath);

        // --- the overflow sound plugin (never a master of, or mastered by, Cyberware.esp)
        if (modSnd != null)
        {
            string sndEspPath = Path.Combine(outDir, modSnd.ModKey.FileName);
            var sndMasters = modSnd.EnumerateFormLinks().Select(l => l.FormKey.ModKey)
                .Where(k => k != modSnd.ModKey && !k.IsNull).Distinct().OrderBy(Rank).ThenBy(k => k.FileName.String).ToList();
            if (sndMasters.Any(k => k == Me)) throw new GenError("Cyberware - Sound.esp must not master Cyberware.esp");
            modSnd.WriteToBinary(sndEspPath, new BinaryWriteParameters { MastersListOrdering = new MastersListOrderingByLoadOrder(sndMasters) });
            using (var back = Fallout4Mod.CreateFromBinaryOverlay(sndEspPath, Fallout4Release.Fallout4))
            {
                if (!back.ModHeader.Flags.HasFlag(Fallout4ModHeader.HeaderFlag.Small)) throw new GenError("Cyberware - Sound.esp is not ESL-flagged");
                if (back.ModHeader.MasterReferences.Any(m => m.Master.FileName.String.Equals("Cyberware.esp", StringComparison.OrdinalIgnoreCase)))
                    throw new GenError("Cyberware - Sound.esp gained a master on Cyberware.esp");
                report.Add($"wrote {sndEspPath}: ESL, {back.EnumerateMajorRecords().Count()} records, masters [{string.Join(", ", back.ModHeader.MasterReferences.Select(m => m.Master.FileName))}]");
            }
            if (lkSnd != null && !lkSnd.Frozen && sndLockPath != null) lkSnd.Save(sndLockPath);
        }
        if (mod.EnumerateFormLinks().Any(l => l.FormKey.ModKey.FileName.String.Equals("Cyberware - Sound.esp", StringComparison.OrdinalIgnoreCase)))
            throw new GenError("Cyberware.esp must not gain a master on Cyberware - Sound.esp (rule of 2026-11-09)");

        var map = new JsonObject
        {
            ["plugin"] = "Cyberware.esp",
            ["catalogSha256"] = catSha,
            ["generator"] = "CyberwareEspGen 0.1.0",
            ["forms"] = new JsonObject(forms.Select(k => KeyValuePair.Create(k.Key, (JsonNode?)JsonValue.Create(k.Value)))),
            ["ripperdocChairs"] = chairs,
            // Present only when --ripperdoc-dialogue-guard built the talk. The DLL keys its behaviour off this: with it, the
            // activation opens NO menu and the view opens on the shop INFO; without it (the play build, which has no dialogue
            // records at all) activating a ripperdoc opens the view directly, as before. docs/VARGA-TALK.md
            ["ripperdocTalk"] = rdQuest == null ? null : new JsonObject
            {
                ["quest"] = forms["CW_RipperdocDialogue"],
                ["branch"] = forms["CW_RipperdocBlockBranch"],
                ["greetTopic"] = forms["CW_RipperdocBlockTopic"],
                ["shopInfo"] = forms["CW_RipperdocInfoShop"],
                ["byeInfo"] = forms["CW_RipperdocInfoBye"],
                ["shopPrompt"] = kRdShopPrompt,
            },
            ["chips"] = chips,
            ["native"] = nativeMap,
            ["chromeCritBands"] = chromeCritBands,
            ["arms"] = armsMap,
            ["sounds"] = soundsMap,
        };
        File.WriteAllText(Path.Combine(outDir, "formmap.json"), map.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");
        foreach (var r in report) Console.WriteLine(r);
        Console.WriteLine($"formmap: {forms.Count} EditorIDs -> {Path.Combine(outDir, "formmap.json")}; lock: {lockPath}");
        return 0;
    }

    const string kChromeCrit = "CW_ChromeCrit";   // cw::kChromeCritPerk

    static FormKey RecordByEdid<T>(IEnumerable<T> recs, string edid) where T : IMajorRecordGetter
        => recs.FirstOrDefault(r => string.Equals(r.EditorID, edid, StringComparison.OrdinalIgnoreCase))?.FormKey
           ?? throw new GenError($"'{edid}' is not an EditorID in Fallout4.esm");

    static uint? HexIn(string s)   // "WastelandDoctorOutfit (227ED3)" -> 0x227ED3
    {
        var m = System.Text.RegularExpressions.Regex.Match(s, @"\(([0-9A-Fa-f]{6})\)");
        return m.Success ? Convert.ToUInt32(m.Groups[1].Value, 16) : null;
    }

    static readonly List<IDisposable> KeepOpen = new();
    static Mutagen.Bethesda.Plugins.Cache.IModContext<IFallout4Mod, IFallout4ModGetter, ICell, ICellGetter> WinningCellContext(string path, FormKey cell)
    {
        var m = Plug.Read(path);
        KeepOpen.Add(m);
        var cache = m.ToImmutableLinkCache();
        foreach (var ctx in m.EnumerateMajorRecordContexts<ICell, ICellGetter>(cache))
            if (ctx.Record.FormKey == cell) return ctx;
        throw new GenError($"{Path.GetFileName(path)} has no CELL {cell}");
    }

    static Mutagen.Bethesda.Plugins.Cache.IModContext<IFallout4Mod, IFallout4ModGetter, IPlacedObject, IPlacedObjectGetter> WinningPlacedContext(string path, FormKey fk)
    {
        var m = Plug.Read(path);
        KeepOpen.Add(m);
        var cache = m.ToImmutableLinkCache();
        foreach (var ctx in m.EnumerateMajorRecordContexts<IPlacedObject, IPlacedObjectGetter>(cache))
            if (ctx.Record.FormKey == fk) return ctx;
        throw new GenError($"{Path.GetFileName(path)} has no placed object {fk}");
    }

    static Subgraph.SubgraphRole ParseRole(JsonNode e)
    {
        // ARMS gives the raw SRAF bytes from the vanilla template: u16 role, u16 perspective (01000100 = Weapon, 1P).
        string hex = Str(e, "SRAF_hex");
        if (hex.Length == 8) return (Subgraph.SubgraphRole)Convert.ToUInt16(hex[2..4] + hex[0..2], 16);
        return Enum.Parse<Subgraph.SubgraphRole>(Str(e, "role", "Weapon"));
    }

    internal static IEnumerable<JsonNode> Arr(JsonNode o, string k) => o[k] is JsonArray a ? a.Where(x => x != null)!.Cast<JsonNode>() : Enumerable.Empty<JsonNode>();
    internal static string Str(JsonNode o, string k, string def = "") => o[k] is JsonValue v && v.TryGetValue<string>(out var s) ? s : def;
    internal static double Num(JsonNode o, string k, double def) => o[k] is JsonValue v && v.TryGetValue<double>(out var d) ? d : def;
}

// ------------------------------------------------------------------------------------------------ Sndr
// Fill one SNDR from a sounds.json slot node. Shared: `gen --sounds` (Cyberware.esp core band +
// Cyberware - Sound.esp) and other plugin commands (Armory) call it with their own mod and lock;
// the caller allocates the FormKey and sets EditorID. Slot fields: files[] (Data-relative,
// backslashes), category + outputModel (EditorIDs in Fallout4.esm), loop, priority, variance,
// freqShift, freqVariance, attenuation (dB). Lookups come from the caller's Fallout4.esm read:
//   fo4.SoundCategories / fo4.SoundOutputModels -> EditorID -> FormKey dictionaries.
static class Sndr
{
    public static void Fill(SoundDescriptor r, JsonNode slot, Dictionary<string, FormKey> scatByEdid, Dictionary<string, FormKey> sopmByEdid)
    {
        string edid = r.EditorID ?? "?";
        r.Notes = "SOUND slot " + Gen.Str(slot, "slot");
        foreach (var fn in Gen.Arr(slot, "files").Select(f => f!.GetValue<string>()))
        {
            if (fn.Contains('/') || fn.Contains("..")) throw new GenError($"sounds: {edid}: file path '{fn}' must be Data-relative with backslashes");
            r.SoundFiles.Add(fn);
        }
        if (r.SoundFiles.Count == 0) throw new GenError($"sounds: {edid}: no files");
        string catName = Gen.Str(slot, "category", "AudioCategorySFX");
        if (!scatByEdid.TryGetValue(catName, out var catKey)) throw new GenError($"sounds: {edid}: '{catName}' is not a SoundCategory EditorID in Fallout4.esm");
        r.Category.SetTo(catKey);
        string om = Gen.Str(slot, "outputModel", "SOMMono01500");
        if (!sopmByEdid.TryGetValue(om, out var omKey)) throw new GenError($"sounds: {edid}: '{om}' is not a SoundOutputModel EditorID in Fallout4.esm");
        r.OutputModel.SetTo(omKey);
        r.Data = new SoundDescriptorStandardData
        {
            Priority = (byte)Gen.Num(slot, "priority", 128),
            Variance = (byte)Gen.Num(slot, "variance", 0),
            PercentFrequencyShift = (sbyte)Gen.Num(slot, "freqShift", 0),
            PercentFrequencyVariance = (sbyte)Gen.Num(slot, "freqVariance", 0),
            StaticAttenuation = (float)Gen.Num(slot, "attenuation", 0),
        };
        if (slot["loop"]?.GetValue<bool>() == true)
            r.LoopAndRumble = new SoundLoopAndRumble { Loop = SoundDescriptor.LoopType.Loop };
    }
}

// ------------------------------------------------------------------------------------------------ winner
static class Winner
{
    // --form "Fallout4.esm|166729;Fallout4.esm|022884" (RACE or CELL records). One pass over the load order.
    // --write file: one "formkey=path" line per form (the first form alone on line 1 for older callers is not kept).
    public static int Run(Args a)
    {
        string profile = a.Req("profile"), modsDir = a.Req("mods"), dataDir = a.Req("data");
        var targets = a.Req("form").Split(';', StringSplitOptions.RemoveEmptyEntries).Select(f => (key: f, fk: Fk.Parse(f))).ToList();
        var mods = File.ReadAllLines(Path.Combine(profile, "modlist.txt")).Where(l => l.StartsWith('+')).Select(l => l[1..]).ToList();
        string? Locate(string plugin)
        {
            foreach (var m in mods) { var p = Path.Combine(modsDir, m, plugin); if (File.Exists(p)) return p; }
            var d = Path.Combine(dataDir, plugin);
            return File.Exists(d) ? d : null;
        }
        var order = new List<string> { "Fallout4.esm", "DLCRobot.esm", "DLCworkshop01.esm", "DLCCoast.esm", "DLCworkshop02.esm", "DLCworkshop03.esm", "DLCNukaWorld.esm", "DLCUltraHighResolution.esm" };
        order.AddRange(File.ReadAllLines(Path.Combine(profile, "plugins.txt")).Where(l => l.StartsWith('*')).Select(l => l[1..]));
        var win = new Dictionary<string, string>();
        var hits = targets.ToDictionary(t => t.key, t => new List<string>());
        int scanned = 0;
        foreach (var pl in order.Distinct(StringComparer.OrdinalIgnoreCase))
        {
            if (pl.Equals("Cyberware.esp", StringComparison.OrdinalIgnoreCase)) continue;
            var path = Locate(pl);
            if (path == null) continue;
            scanned++;
            try
            {
                using var m = Plug.Read(path);
                HashSet<FormKey>? cells = null;
                HashSet<FormKey>? placed = null;
                byte[]? raw = null;
                foreach (var t in targets)
                {
                    bool has = m.Races.Any(r => r.FormKey == t.fk);
                    if (!has) { cells ??= m.EnumerateMajorRecords<ICellGetter>().Select(c => c.FormKey).ToHashSet(); has = cells.Contains(t.fk); }
                    if (!has)
                    {
                        // a placed record (REFR/ACHR, e.g. a door for --door-unlock). Enumerating placed records is the
                        // slow path, so triage first: an override must carry the local id's low 3 bytes somewhere in the file.
                        raw ??= File.ReadAllBytes(path);
                        var needle = new byte[] { (byte)t.fk.ID, (byte)(t.fk.ID >> 8), (byte)(t.fk.ID >> 16) };
                        if (raw.AsSpan().IndexOf(needle) >= 0)
                        {
                            placed ??= m.EnumerateMajorRecords<IPlacedGetter>().Select(p => p.FormKey).ToHashSet();
                            has = placed.Contains(t.fk);
                        }
                    }
                    if (has) { win[t.key] = path; hits[t.key].Add(pl); }
                }
            }
            catch (Exception e) { Console.Error.WriteLine($"skip {pl}: {e.Message}"); }
        }
        Console.Error.WriteLine($"scanned {scanned} plugins");
        foreach (var t in targets)
        {
            Console.Error.WriteLine($"{t.key} defined/overridden by: {string.Join(", ", hits[t.key])}");
            if (!win.ContainsKey(t.key)) throw new GenError("no plugin in the load order has " + t.key);
            Console.WriteLine($"{t.key}={win[t.key]}");
        }
        if (a.Opt("write") is string wf) File.WriteAllLines(wf, targets.Select(t => $"{t.key}={win[t.key]}"));
        return 0;
    }
}

// ------------------------------------------------------------------------------------------------ probe
static class Probe
{
    public static int Run(Args a)
    {
        using var m = Plug.Read(a.Req("esm"));
        string type = a.Req("type").ToUpperInvariant(), match = a.Opt("match") ?? "";
        IEnumerable<IMajorRecordGetter> recs = type switch
        {
            "AVIF" => m.ActorValueInformation,
            "RACE" => m.Races,
            "NPC_" => m.Npcs,
            "ACTI" => m.Activators,
            "STAT" => m.Statics,
            "FURN" => m.Furniture,
            "PERK" => m.Perks,
            "KYWD" => m.Keywords,
            "CELL" => m.EnumerateMajorRecords<ICellGetter>(),
            "MGEF" => m.MagicEffects,
            "ALCH" => m.Ingestibles,
            "SPEL" => m.Spells,
            "IMAD" => m.ImageSpaceAdapters,
            "SNDR" => m.SoundDescriptors,
            "SCAT" => m.SoundCategories,
            "SOPM" => m.SoundOutputModels,
            _ => throw new GenError("probe type not supported: " + type),
        };
        foreach (var r in recs.Where(r => (r.EditorID ?? "").Contains(match, StringComparison.OrdinalIgnoreCase)))
        {
            string extra = r switch
            {
                IFurnitureGetter f => " model=" + f.Model?.File,
                IActivatorGetter ac => " model=" + ac.Model?.File,
                IStaticGetter s => " model=" + s.Model?.File,
                IRaceGetter rc => $" subgraphs={rc.Subgraphs.Count} template={rc.SubgraphTemplateRace.FormKeyNullable}",
                ISoundDescriptorGetter sd => $" cat={sd.Category.FormKeyNullable} om={sd.OutputModel.FormKeyNullable} files=[{string.Join(";", sd.SoundFiles)}] loop={(sd.LoopAndRumble?.Loop.ToString() ?? "-")}",
                INpcGetter n => $" name={n.Name?.String} template={n.DefaultTemplate.FormKeyNullable} female={n.Flags.HasFlag(Npc.Flag.Female)}",
                IMagicEffectGetter e => $" arch={(e.Archetype as IMagicEffectArchetypeGetter)?.Type.ToString() ?? e.Archetype.GetType().Name} av={e.Archetype.ActorValue.FormKeyNullable} cast={e.CastType} flags={e.Flags} shader={e.HitShader.FormKeyNullable} isfx={e.ImageSpaceModifier.FormKeyNullable}",
                IIngestibleGetter g => " effects=" + string.Join(";", g.Effects.Select(x => $"{x.BaseEffect.FormKeyNullable}/m{x.Data?.Magnitude}/d{x.Data?.Duration}/a{x.Data?.Area}")),
                ISpellGetter sp => $" type={sp.Type} cast={sp.CastType} effects=" + string.Join(";", sp.Effects.Select(x => $"{x.BaseEffect.FormKeyNullable}/m{x.Data?.Magnitude}/d{x.Data?.Duration}/a{x.Data?.Area}")),
                _ => "",
            };
            Console.WriteLine($"{r.FormKey.ModKey.FileName}|{r.FormKey.ID:X6} {r.EditorID}{extra}");
        }
        return 0;
    }
}
