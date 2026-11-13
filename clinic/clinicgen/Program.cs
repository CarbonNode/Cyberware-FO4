// ClinicEspGen - builds "Cyberware - Clinic.esp" (ESL-flagged): Varga's clinic interior for the CLINIC
// stream. Reproducible: the same spec + lock give the same FormIDs (espgen's IdLock pattern).
//
// What it does (docs in ../README.md):
//   1. Clones zUnusedFalloutShelter02's architecture, navmesh and kept clutter into a NEW interior
//      cell (CW_VargaClinic01) at identical local coordinates, so the vanilla navmesh stays valid.
//      Refs on the spec's strip list are not cloned. The NAVM is re-parented to the new cell;
//      vertices/triangles/grid are untouched.
//   2. Adds the dressing pass from the spec (vanilla Fallout4.esm bases only).
//   3. Places a load-door pair: one in the new cell, one in an override of DmndSurgeryBasement01
//      copied from the WINNING plugin (--dc-base, normally Cyberware.esp's copy), XTEL both ways.
//      The override adds ONE ref and changes nothing else (precombine/previs fields preserved).
//
//   gen --esm <Fallout4.esm path> --spec clinic.json --out <dir> [--lock clinic.lock.json]
//       [--dc-base <plugin with the winning DmndSurgeryBasement01>] [--frozen]
//
// Without --dc-base the DC-side door is skipped (dev build) and the run says so loudly.
using System.Security.Cryptography;
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
            if (argv.Length == 0 || argv[0] != "gen") { Console.Error.WriteLine("usage: ClinicEspGen gen --esm <Fallout4.esm> --spec clinic.json --out <dir> [--lock f] [--dc-base plugin] [--frozen]"); return 2; }
            var a = Args.Parse(argv.Skip(1).ToArray());
            int rc = Gen.Run(a);
            if (rc == 0) a.CheckAllRead("clinicgen");   // see Args.CheckAllRead: a silently ignored flag is a wiring bug
            return rc;
        }
        catch (GenError e) { Console.Error.WriteLine("ERROR: " + e.Message); return 1; }
    }
}

class GenError(string m) : Exception(m);

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
    public static FormKey Parse(string s)
    {
        var bar = s.IndexOf('|');
        if (bar < 0) throw new GenError($"form key '{s}' must look like Plugin.esm|00ABCD");
        return new FormKey(ModKey.FromFileName(s[..bar]), Convert.ToUInt32(s[(bar + 1)..], 16) & 0xFFFFFF);
    }
}

// EditorID -> local id (0x800..0xFFF). Same contract as espgen's lock: committed, never renumbered.
class IdLock
{
    public Dictionary<string, uint> Ids = new();
    public Dictionary<string, uint> Retired = new();
    readonly HashSet<string> _used = new();
    public const uint Min = 0x800, Max = 0xFFF;
    public bool Frozen;

    public static IdLock Load(string? path)
    {
        var l = new IdLock();
        if (path == null || !File.Exists(path)) return l;
        var j = JsonNode.Parse(File.ReadAllText(path))!;
        foreach (var (k, v) in j["ids"]!.AsObject()) l.Ids[k] = Convert.ToUInt32(v!.GetValue<string>(), 16);
        if (j["retired"] is JsonObject r) foreach (var (k, v) in r) l.Retired[k] = Convert.ToUInt32(v!.GetValue<string>(), 16);
        return l;
    }
    public uint Get(string edid)
    {
        _used.Add(edid);
        if (Ids.TryGetValue(edid, out var id)) return id;
        if (Retired.TryGetValue(edid, out var old)) { Ids[edid] = old; Retired.Remove(edid); return old; }   // a phase-gated record revives with its old id
        if (Frozen) throw new GenError($"--frozen: lock has no id for '{edid}'");
        var taken = new HashSet<uint>(Ids.Values.Concat(Retired.Values));
        for (uint i = Min; i <= Max; i++)
            if (!taken.Contains(i)) { Ids[edid] = i; return i; }
        throw new GenError("ESL id range exhausted");
    }
    public void Save(string path)
    {
        foreach (var k in Ids.Keys.Where(k => !_used.Contains(k)).ToList())
        { Retired[k] = Ids[k]; Ids.Remove(k); }
        var j = new JsonObject
        {
            ["ids"] = new JsonObject(Ids.OrderBy(kv => kv.Value).Select(kv => KeyValuePair.Create(kv.Key, (JsonNode?)kv.Value.ToString("X3")))),
            ["retired"] = new JsonObject(Retired.OrderBy(kv => kv.Value).Select(kv => KeyValuePair.Create(kv.Key, (JsonNode?)kv.Value.ToString("X3")))),
        };
        File.WriteAllText(path, j.ToJsonString(new System.Text.Json.JsonSerializerOptions { WriteIndented = true }) + "\n");
    }
}

static class Gen
{
    static readonly ModKey Me = ModKey.FromFileName("Cyberware - Clinic.esp");

    public static int Run(Args a)
    {
        string specPath = a.Req("spec");
        var spec = JsonNode.Parse(File.ReadAllText(specPath))!;
        // The spec's hash goes into the esp header (SNAM), so it is taken over LF line endings: a CRLF checkout (git autocrlf on
        // Windows) must give the same esp byte for byte as an LF one (2026-11-10: 15 header bytes differed, every record equal).
        var specBytes = File.ReadAllBytes(specPath);
        var specLf = new List<byte>(specBytes.Length);
        for (int i = 0; i < specBytes.Length; i++)
            if (!(specBytes[i] == (byte)'\r' && i + 1 < specBytes.Length && specBytes[i + 1] == (byte)'\n')) specLf.Add(specBytes[i]);
        string specSha = Convert.ToHexString(SHA256.HashData(specLf.ToArray())).ToLowerInvariant();
        string outDir = a.Req("out");
        Directory.CreateDirectory(outDir);
        string? lockPath = a.Opt("lock");
        var lk = IdLock.Load(lockPath);
        lk.Frozen = a.Opt("frozen") == "true";
        var report = new List<string>();

        using var esm = Fallout4Mod.CreateFromBinaryOverlay(a.Req("esm"), Fallout4Release.Fallout4);

        var mod = new Fallout4Mod(Me, Fallout4Release.Fallout4);
        mod.ModHeader.Flags |= Fallout4ModHeader.HeaderFlag.Small;   // ESL
        mod.ModHeader.Author = "Cyberware CLINIC generator";
        mod.ModHeader.Description = "Generated from clinic.json sha256 " + specSha[..16] + ". Do not edit by hand. Load AFTER Cyberware.esp.";
        FormKey K(string edid) => new(Me, lk.Get(edid));

        // ---- base resolution: EditorID -> FormKey over the record types the spec may name
        var bases = new Dictionary<string, FormKey>(StringComparer.OrdinalIgnoreCase);
        // CLNCL4: the record TYPE of each base, because only havok classes need the no-settle flag.
        // STAT and FURN are static collision and the engine never settles them, so flagging them would
        // be noise in the plugin. Kept beside `bases` so both come from the same single pass.
        var baseKind = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        void Index(IEnumerable<IFallout4MajorRecordGetter> recs, string kind)
        {
            foreach (var r in recs)
                if (r.EditorID is string e && !bases.ContainsKey(e)) { bases[e] = r.FormKey; baseKind[e] = kind; }
        }
        Index(esm.Statics, "STAT"); Index(esm.MovableStatics, "MSTT"); Index(esm.Containers, "CONT");
        Index(esm.Furniture, "FURN"); Index(esm.Doors, "DOOR"); Index(esm.Lights, "LIGH");
        Index(esm.MiscItems, "MISC"); Index(esm.Ingestibles, "ALCH"); Index(esm.Activators, "ACTI");
        Index(esm.Armors, "ARMO");

        // The classes whose refs havok moves when the cell loads - the same set as prop_support.py's
        // HAVOK, restricted to the types this generator can place. A ref of one of these, placed as
        // dressing or clutter, gets NoSettle when the spec asks for it.
        var havokKinds = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
            { "MISC", "ALCH", "MSTT", "CONT", "ACTI", "ARMO" };
        // int, not uint: MajorRecordFlagsRaw is an int (see the `|= 0x400` persistent flags below), and
        // 0x20000000 fits, so this stays a plain `|=` with no cast.
        const int NoSettleFlag = 0x20000000;   // REFR "Don't Havok Settle"
        // phase2-only, like stripDonor: propRest belongs to the phase 2 clutter pass, and a phase 1
        // build must stay byte-comparable to what it was before this change.
        bool noSettle = a.Opt("phase2") == "true"
                        && (spec["phase2"]?["propRest"]?["noSettle"]?.GetValue<bool>() ?? false);
        int pinned = 0;
        void Pin(PlacedObject r, string baseName)
        {
            if (!noSettle) return;
            if (!baseKind.TryGetValue(baseName, out var k) || !havokKinds.Contains(k)) return;
            r.MajorRecordFlagsRaw |= NoSettleFlag;
            pinned++;
        }
        FormKey Base(string name) => name.Contains('|') ? Fk.Parse(name)
            : bases.TryGetValue(name, out var fk) ? fk
            : throw new GenError($"base '{name}' is no STAT/MSTT/CONT/FURN/DOOR/LIGH/MISC/ALCH/ACTI/ARMO EditorID in Fallout4.esm");

        // ---- donor cell
        var donorKey = Fk.Parse(spec["donor"]!.GetValue<string>());
        ICellGetter? donor = null;
        foreach (var block in esm.Cells.Records)
            foreach (var sub in block.SubBlocks)
                foreach (var c in sub.Cells)
                    if (c.FormKey == donorKey) donor = c;
        if (donor == null) throw new GenError($"donor cell {donorKey} not found in the esm");

        // ---- the new cell, header copied from the donor MINUS previs/precombine/location fields
        string cellEdid = spec["cell"]!["edid"]!.GetValue<string>();
        var cell = new Cell(K(cellEdid), Fallout4Release.Fallout4)
        {
            EditorID = cellEdid,
            Name = spec["cell"]!["name"]!.GetValue<string>(),
            Flags = donor.Flags,
            Lighting = donor.Lighting?.DeepCopy(),
            WaterHeight = donor.WaterHeight,
        };
        cell.LightingTemplate.SetTo(donor.LightingTemplate.FormKey);
        cell.Water.SetTo(donor.Water.FormKey);
        cell.AcousticSpace.SetTo(donor.AcousticSpace.FormKey);
        cell.Music.SetTo(donor.Music.FormKey);
        cell.ImageSpace.SetTo(donor.ImageSpace.FormKey);
        report.Add($"cell {cellEdid}: header from donor {donor.EditorID} (flags {donor.Flags}, LGTM {donor.LightingTemplate.FormKey})");

        // ---- clone donor refs (minus strip list) at identical coordinates
        var strip = new HashSet<uint>(((JsonArray)spec["strip"]!).Select(s => Convert.ToUInt32(s!.GetValue<string>(), 16)));
        // Phase 2 only: donor fills and haze that flatten the station (phase2.stripDonor); Phase 1 keeps them
        if (a.Opt("phase2") == "true" && spec["phase2"]?["stripDonor"] is JsonArray p2strip)
            foreach (var s in p2strip) strip.Add(Convert.ToUInt32(s!.GetValue<string>(), 16));
        int kept = 0, skipped = 0, stripMatched = 0;
        void Clone(IReadOnlyList<IPlacedGetter> src, ExtendedList<IPlaced> dst)
        {
            foreach (var p in src)
            {
                if (strip.Contains(p.FormKey.ID)) { stripMatched++; continue; }
                if (p is not IPlacedObjectGetter po) { skipped++; continue; }   // no ACHR/trap expected in the donor
                var dup = po.Duplicate(K($"Donor_{p.FormKey.ID:X8}"));
                dup.EditorID = null;                                            // vanilla refs carry no EDID
                dst.Add(dup);
                kept++;
            }
        }
        Clone(donor.Persistent, cell.Persistent);
        Clone(donor.Temporary, cell.Temporary);
        if (stripMatched != strip.Count)
            throw new GenError($"strip list: only {stripMatched} of {strip.Count} ids matched donor refs - stale list?");
        report.Add($"donor refs: {kept} cloned, {strip.Count} stripped, {skipped} non-REFR skipped");

        // ---- navmesh: verbatim copy, re-parented to the new cell
        int navs = 0;
        foreach (var nmg in donor.NavigationMeshes)
        {
            var nm = nmg.Duplicate(K($"Navmesh_{nmg.FormKey.ID:X8}"));
            if (nm.NavmeshGeometry == null) throw new GenError($"donor NAVM {nmg.FormKey} has no NVNM");
            var parent = new CellNavmeshParent();
            parent.Parent.SetTo(cell.FormKey);
            nm.NavmeshGeometry.Parent = parent;
            cell.NavigationMeshes.Add(nm);
            navs++;
            report.Add($"navmesh {nmg.FormKey.ID:X8}: {nm.NavmeshGeometry.Vertices?.Count} verts, {nm.NavmeshGeometry.Triangles.Count} tris, re-parented");
        }
        if (navs == 0) throw new GenError("donor has no navmesh");
        // the walkable mesh as plain JSON for tools/lane_check.py (a build product, never committed)
        {
            var g0 = cell.NavigationMeshes[0].NavmeshGeometry!;
            var vs = new JsonArray(g0.Vertices!.Select(p => (JsonNode?)new JsonArray(p.X, p.Y, p.Z)).ToArray());
            var ts = new JsonArray(g0.Triangles.Select(t => (JsonNode?)new JsonArray((int)t.Vertices.X, (int)t.Vertices.Y, (int)t.Vertices.Z)).ToArray());
            File.WriteAllText(Path.Combine(outDir, "clinic-nav.json"), new JsonObject { ["v"] = vs, ["t"] = ts }.ToJsonString() + "\n");
        }

        // ---- Phase 2 layout moves (spec phase2.reposition): applied to the spec in memory so the Phase 1 esp keeps
        // its layout. Each entry matches ONE dressing ref by base + position, then sets a new pos/rotZ. "persistentEdid"
        // makes that ref persistent with an EditorID, for a ref a package links to (the office chair).
        var persistentDress = new Dictionary<string, PlacedObject>(StringComparer.OrdinalIgnoreCase);
        // Phase 2 crate swaps (spec phase2.swapOut): matched by base + original position, before any reposition.
        // A swapped ref keeps its index, so the Dress_NNN ids of every other ref never shift.
        if (a.Opt("phase2") == "true" && spec["phase2"]?["swapOut"] is JsonArray swaps)
        {
            foreach (var m in swaps)
            {
                string sBase = m!["base"]!.GetValue<string>();
                var from = (JsonArray)m["from"]!;
                JsonNode? hit = null;
                foreach (var d in (JsonArray)spec["dress"]!)
                {
                    var dp = (JsonArray)d!["pos"]!;
                    if (d["base"]!.GetValue<string>() == sBase
                        && Math.Abs(dp[0]!.GetValue<double>() - from[0]!.GetValue<double>()) < 0.51
                        && Math.Abs(dp[1]!.GetValue<double>() - from[1]!.GetValue<double>()) < 0.51)
                    { if (hit != null) throw new GenError($"phase2.swapOut: two '{sBase}' refs match {from.ToJsonString()}"); hit = d; }
                }
                if (hit == null) throw new GenError($"phase2.swapOut: no '{sBase}' at {from.ToJsonString()} - stale entry?");
                hit["swappedOut"] = true;
            }
            report.Add($"phase2: {swaps.Count} dressing ref(s) swapped out");
        }
        if (a.Opt("phase2") == "true" && spec["phase2"]?["reposition"] is JsonArray moves)
        {
            foreach (var m in moves)
            {
                string mBase = m!["base"]!.GetValue<string>();
                var from = (JsonArray)m["from"]!;
                JsonNode? hit = null;
                foreach (var d in (JsonArray)spec["dress"]!)
                {
                    var dp = (JsonArray)d!["pos"]!;
                    if (d["base"]!.GetValue<string>() == mBase
                        && Math.Abs(dp[0]!.GetValue<double>() - from[0]!.GetValue<double>()) < 0.51
                        && Math.Abs(dp[1]!.GetValue<double>() - from[1]!.GetValue<double>()) < 0.51)
                    { if (hit != null) throw new GenError($"phase2.reposition: two '{mBase}' refs match {from.ToJsonString()}"); hit = d; }
                }
                if (hit == null) throw new GenError($"phase2.reposition: no '{mBase}' at {from.ToJsonString()} - stale entry?");
                if (m["pos"] is JsonArray np) hit["pos"] = np.DeepClone();
                if (m["rotZ"] is JsonNode nr) hit["rotZ"] = nr.DeepClone();
                if (m["persistentEdid"] is JsonNode pe) hit["persistentEdid"] = pe.DeepClone();
            }
            report.Add($"phase2: {moves.Count} dressing ref(s) repositioned");
        }

        // ---- dressing
        int dressed = 0, dressIndex = -1;
        foreach (var d in (JsonArray)spec["dress"]!)
        {
            dressIndex++;
            if (d!["swappedOut"] is not null) continue;
            string baseName = d!["base"]!.GetValue<string>();
            var fk = Base(baseName);
            var r = new PlacedObject(K($"Dress_{dressIndex:D3}_{baseName}"), Fallout4Release.Fallout4)
            {
                Position = P3(d["pos"]!),
                Rotation = new P3Float(0, 0, (float)d["rotZ"]!.GetValue<double>()),
            };
            r.Base.SetTo(fk);
            Pin(r, baseName);                      // CLNCL4: hold loose clutter where the spec puts it
            if (d["scale"] is JsonNode sc) r.Scale = (float)sc.GetValue<double>();
            if (d["lockLevel"] is JsonNode ll)
                r.Lock = new LockData { Level = (LockLevel)ll.GetValue<int>() };
            if (d["persistentEdid"] is JsonNode pedid)
            {
                r.EditorID = pedid.GetValue<string>();
                r.MajorRecordFlagsRaw |= 0x400;
                cell.Persistent.Add(r);
                persistentDress[r.EditorID] = r;
            }
            else cell.Temporary.Add(r);
            dressed++;
        }
        report.Add($"dressing: {dressed} refs placed");

        // ---- CP props (the lead, 2026-11-09: chair + monitor banks are THE focal point): own STAT records
        // whose models are the props2nif BSTriShape conversions (shader names = vanilla BGSM paths, so no
        // textures or materials ship). Model path root: Meshes\Cyberware\Clinic\.
        if (spec["cpProps"] is JsonNode cps)
        {
            var statByModel = new Dictionary<string, FormKey>(StringComparer.OrdinalIgnoreCase);
            int placedCp = 0;
            foreach (var d in (JsonArray)cps)
            {
                string model = d!["model"]!.GetValue<string>();
                if (!statByModel.TryGetValue(model, out var sk))
                {
                    string sedid = "CWC_Stat_" + Path.GetFileNameWithoutExtension(model);
                    var st = new Static(K(sedid), Fallout4Release.Fallout4)
                    {
                        EditorID = sedid,
                        Model = new Model { File = @"Cyberware\Clinic\" + model },
                    };
                    mod.Statics.Add(st);
                    sk = st.FormKey;
                    statByModel[model] = sk;
                }
                var r = new PlacedObject(K($"CpRef_{placedCp:D3}_{Path.GetFileNameWithoutExtension(model)}"), Fallout4Release.Fallout4)
                {
                    Position = P3(d["pos"]!),
                    Rotation = new P3Float(0, 0, (float)d["rotZ"]!.GetValue<double>()),
                };
                r.Base.SetTo(sk);
                if (d["scale"] is JsonNode sc2) r.Scale = (float)sc2.GetValue<double>();
                cell.Temporary.Add(r);
                placedCp++;
            }
            report.Add($"cp props: {statByModel.Count} STATs, {placedCp} refs (models under Meshes\\Cyberware\\Clinic\\)");
        }

        // ---- SOUND stream integration: our OWN SNDRs for their staged wavs (never master their esp).
        // Loops with a "pos" also get an invisible ACTI (LoopingSound) + a placed ref - the vanilla
        // ambience pattern. One-shots are SNDR-only; CHAIRS plays them from the install scene.
        if (spec["sounds"] is JsonNode snds)
        {
            var ambCat = esm.SoundCategories.First(c => c.EditorID == "AudioCategoryAMB").FormKey;
            var mono1500 = esm.SoundOutputModels.First(m => m.EditorID == "SOMMono01500").FormKey;
            int placedSnd = 0, sndrs = 0;
            foreach (var d in (JsonArray)snds)
            {
                string sedid = d!["edid"]!.GetValue<string>();
                bool loop = d["loop"]!.GetValue<bool>();
                var sd = new SoundDescriptor(K(sedid), Fallout4Release.Fallout4)
                {
                    EditorID = sedid,
                    LoopAndRumble = new SoundLoopAndRumble { Loop = loop ? SoundDescriptor.LoopType.Loop : SoundDescriptor.LoopType.None },
                    Data = new SoundDescriptorStandardData
                    {
                        StaticAttenuation = (float)d["atten"]!.GetValue<double>(),
                        Priority = 128,
                    },
                };
                sd.Category.SetTo(ambCat);
                sd.OutputModel.SetTo(mono1500);
                sd.SoundFiles.Add(d["file"]!.GetValue<string>());
                mod.SoundDescriptors.Add(sd);
                sndrs++;
                if (d["pos"] is JsonNode sp)
                {
                    var act = new Mutagen.Bethesda.Fallout4.Activator(K(sedid + "_Marker"), Fallout4Release.Fallout4) { EditorID = sedid + "_Marker" };
                    act.LoopingSound.SetTo(sd.FormKey);
                    mod.Activators.Add(act);
                    var r = new PlacedObject(K(sedid + "_Ref"), Fallout4Release.Fallout4) { Position = P3(sp) };
                    r.Base.SetTo(act.FormKey);
                    cell.Temporary.Add(r);
                    placedSnd++;
                }
            }
            report.Add($"sounds: {sndrs} SNDRs ({placedSnd} placed ambience loops)");
        }

        // ---- interior cell block bookkeeping (CK groups interiors by the last decimal digits of the id)
        uint cid = cell.FormKey.ID;
        var subBlock = new CellSubBlock { BlockNumber = (int)(cid / 10 % 10), GroupType = GroupTypeEnum.InteriorCellSubBlock };
        subBlock.Cells.Add(cell);
        var blockRec = new CellBlock { BlockNumber = (int)(cid % 10), GroupType = GroupTypeEnum.InteriorCellBlock };
        blockRec.SubBlocks.Add(subBlock);
        mod.Cells.Records.Add(blockRec);

        // ---- the door pair
        var doors = spec["doors"]!;
        var doorBase = Base(doors["base"]!.GetValue<string>());
        var clinicDoor = new PlacedObject(K("CW_ClinicDoorInner"), Fallout4Release.Fallout4)
        {
            EditorID = "CW_ClinicDoorInner",
            Position = P3(doors["clinic"]!["pos"]!),
            Rotation = new P3Float(0, 0, (float)doors["clinic"]!["rotZ"]!.GetValue<double>()),
        };
        clinicDoor.Base.SetTo(doorBase);
        clinicDoor.MajorRecordFlagsRaw |= 0x400;   // persistent
        cell.Persistent.Add(clinicDoor);

        string? dcBase = a.Opt("dc-base");
        IFallout4ModDisposableGetter? dcBaseMod = dcBase == null ? null : Fallout4Mod.Create(Fallout4Release.Fallout4).FromPath(dcBase).Construct();
        if (dcBaseMod == null)
        {
            report.Add("WARNING: no --dc-base; DC-side door NOT placed; this esp is a dev build, do not ship it");
        }
        else
        {
            var dcCellKey = Fk.Parse(doors["dc"]!["cell"]!.GetValue<string>());
            var ctx = WinningCellContext(dcBaseMod, dcCellKey);
            var dcCell = (Cell)ctx.GetOrAddAsOverride(mod);
            if (ctx.Record.Name != null && string.IsNullOrEmpty(dcCell.Name?.String))
                throw new GenError($"CELL {dcCellKey} from {dcBaseMod.ModKey} has a localized name that reads empty - pass a non-localized winner (Cyberware.esp's copy)");
            var dcDoor = new PlacedObject(K("CW_ClinicDoorOuter"), Fallout4Release.Fallout4)
            {
                EditorID = "CW_ClinicDoorOuter",
                Position = P3(doors["dc"]!["pos"]!),
                Rotation = new P3Float(0, 0, (float)doors["dc"]!["rotZ"]!.GetValue<double>()),
            };
            dcDoor.Base.SetTo(doorBase);
            dcDoor.MajorRecordFlagsRaw |= 0x400;
            dcCell.Persistent.Add(dcDoor);

            // XTEL both ways: each side names the OTHER door and where the traveller lands there.
            var dcLand = (JsonArray)doors["dc"]!["landing"]!;
            var clLand = (JsonArray)doors["clinic"]!["landing"]!;
            clinicDoor.TeleportDestination = Tele(dcDoor.FormKey, dcLand);
            dcDoor.TeleportDestination = Tele(clinicDoor.FormKey, clLand);
            report.Add($"door pair: clinic {clinicDoor.FormKey} <-> DC {dcDoor.FormKey} in {Path.GetFileName(dcBase)}'s copy of {dcCellKey}");
        }

        // ---- PHASE 2 (--phase2, needs --dc-base = the final Cyberware.esp): Varga moves in, bed, schedule, menu chair.
        // Her NPC_ override is REGENERATED FROM THE FINAL Cyberware.esp every build (LORE's collision rule): we copy
        // the master's record as-is and only APPEND our packages, so look work (WNAM/DOFT/PNAM/head parts) passes through.
        if (a.Opt("phase2") == "true")
        {
            if (dcBaseMod == null) throw new GenError("--phase2 needs --dc-base (the final Cyberware.esp)");
            var p2 = spec["phase2"] ?? throw new GenError("--phase2: spec has no phase2 block");
            var cwKey = dcBaseMod.ModKey;

            // globals the DLL writes (INSTALL SCENE: CW_ClinicSceneActive, 1 during the chair scene). Short, like vanilla quest
            // flags; the DLL sets TESGlobal::value and runs evp, it never needs Papyrus. Built before the packages that read them.
            if (p2["globals"] is JsonArray globList)
                foreach (var gj in globList)
                {
                    string gedid = gj!["edid"]!.GetValue<string>();
                    var glob = new GlobalShort(K(gedid), Fallout4Release.Fallout4) { EditorID = gedid, Data = (short)(gj["value"]?.GetValue<int>() ?? 0) };
                    mod.Globals.Add(glob);
                    report.Add($"phase2 global {gedid} {glob.FormKey} (short, start {glob.Data})");
                }

            // packages: duplicates of vanilla defaults, schedule adjusted
            var pkgLinks = new List<FormKey>();
            foreach (var pj in (JsonArray)p2["packages"]!)
            {
                var srcKey = Fk.Parse(pj!["copyOf"]!.GetValue<string>());
                var srcPkg = esm.Packages.FirstOrDefault(p => p.FormKey == srcKey)
                             ?? throw new GenError($"phase2: package {srcKey} not in the esm");
                var pkg = srcPkg.Duplicate(K(pj["edid"]!.GetValue<string>()));
                pkg.EditorID = pj["edid"]!.GetValue<string>();
                if (pj["scheduleHour"] is JsonNode sh) pkg.ScheduleHour = (sbyte)sh.GetValue<int>();
                if (pj["scheduleDurationMinutes"] is JsonNode sd) pkg.ScheduleDurationInMinutes = sd.GetValue<int>();
                pkg.ScheduleMonth = -1;   // some vanilla donors carry month 0; ours run every month
                // hours [from, to): the time window as CONDITIONS on GameHour (vanilla precedent: MS02DonnyInside), not a
                // PSDT schedule. P15 (2026-11-09): with PSDT schedules the clinic station/desk/bed packages never won a pick
                // in the clinic, even a fresh one (15:00, the hold won over the shop although the two differ only in their
                // schedule), and evp never moved her off a still-valid hold. With windows as conditions, at any hour exactly
                // one clinic package passes, so every pick (engine, evp or fresh) has one answer.
                if (pj["hours"] is JsonArray hw)
                {
                    if (pj["scheduleHour"] != null || pj["scheduleDurationMinutes"] != null)
                        throw new GenError($"phase2 package {pkg.EditorID}: 'hours' and a PSDT schedule are exclusive");
                    pkg.ScheduleHour = -1; pkg.ScheduleMinute = -1; pkg.ScheduleDurationInMinutes = 0;   // PSDT "any", like the hold
                    var gameHour = Fk.Parse("Fallout4.esm|000038");
                    void HourCond(CompareOperator op, float v)
                    {
                        var g = new FunctionConditionData { Function = Condition.Function.GetGlobalValue };
                        g.ParameterOneRecord.SetTo(gameHour);
                        pkg.Conditions.Add(new ConditionFloat { CompareOperator = op, ComparisonValue = v, Data = g });
                    }
                    if (hw.Count != 2) throw new GenError($"phase2 package {pkg.EditorID}: hours must be [from, to], null for an open end");
                    if (hw[0] is JsonNode from) HourCond(CompareOperator.GreaterThanOrEqualTo, from.GetValue<float>());
                    if (hw[1] is JsonNode to) HourCond(CompareOperator.LessThan, to.GetValue<float>());
                }
                if (pj["radius"] is JsonNode rad)
                {
                    int changed = 0;
                    foreach (var kv in pkg.Data)
                        if (kv.Value is PackageDataLocation pl && pl.Location is not null) { pl.Location.Radius = rad.GetValue<uint>(); changed++; }
                    if (changed != 1) throw new GenError($"phase2 package {pkg.EditorID}: radius set on {changed} location inputs, expected 1");
                }
                // nearRef: leash the package to a persistent clinic ref ("near reference") instead of her editor location.
                // P12 (2026-11-09) saw her at the bed and the desk at hour 15 under the radius-150 station sandbox. A near-
                // editor-location target depends on what the engine and the save hold as her start; the chair ref does not.
                if (pj["nearRef"] is JsonNode nr)
                {
                    var target = K(nr.GetValue<string>());
                    int changed = 0;
                    foreach (var kv in pkg.Data)
                        if (kv.Value is PackageDataLocation pl && pl.Location is not null)
                        { pl.Location.Target = new LocationTarget { Link = new FormLink<IPlacedGetter>(target) }; changed++; }
                    if (changed != 1) throw new GenError($"phase2 package {pkg.EditorID}: nearRef set on {changed} location inputs, expected 1");
                }
                if (pj["condition"] is JsonNode cj)
                {
                    // inClinic: GetInCell == 1, so she only runs a station package while she is in the clinic; in the market
                    // the current-location fallback runs (a station package there would try to path through the door).
                    // notInClinic: GetInCell == 0, the outdoor fallback (P15: its radius-1100 sandbox must never roam the room).
                    var cond = cj.GetValue<string>();
                    if (cond != "inClinic" && cond != "notInClinic") throw new GenError($"phase2 package {pkg.EditorID}: unknown condition '{cond}'");
                    var inCell = new FunctionConditionData { Function = Condition.Function.GetInCell };
                    inCell.ParameterOneRecord.SetTo(cell.FormKey);
                    pkg.Conditions.Add(new ConditionFloat { CompareOperator = CompareOperator.EqualTo, ComparisonValue = cond == "inClinic" ? 1f : 0f, Data = inCell });
                }
                // globalIs [edid, value]: GetGlobalValue <our GLOB> == value, ANDed with the rest (INSTALL SCENE step-in package).
                if (pj["globalIs"] is JsonArray gi)
                {
                    if (gi.Count != 2) throw new GenError($"phase2 package {pkg.EditorID}: globalIs must be [edid, value]");
                    var gk = K(gi[0]!.GetValue<string>());
                    if (!mod.Globals.ContainsKey(gk)) throw new GenError($"phase2 package {pkg.EditorID}: globalIs names {gi[0]}, which phase2.globals does not build");
                    var gv = new FunctionConditionData { Function = Condition.Function.GetGlobalValue };
                    gv.ParameterOneRecord.SetTo(gk);
                    pkg.Conditions.Add(new ConditionFloat { CompareOperator = CompareOperator.EqualTo, ComparisonValue = gi[1]!.GetValue<float>(), Data = gv });
                }
                mod.Packages.Add(pkg);
                pkgLinks.Add(pkg.FormKey);
                report.Add($"phase2 package {pkg.EditorID}: copy of {srcPkg.EditorID}, hour {pkg.ScheduleHour} duration {pkg.ScheduleDurationInMinutes} min, {pkg.Conditions.Count} condition(s)");
            }

            // NPC_ override from the FINAL master, our packages appended
            string npcEdid = p2["vargaNpcEdid"]!.GetValue<string>();
            var npcSrc = dcBaseMod.Npcs.FirstOrDefault(n => string.Equals(n.EditorID, npcEdid, StringComparison.OrdinalIgnoreCase))
                         ?? throw new GenError($"phase2: NPC_ '{npcEdid}' not in {cwKey}");
            var npc = npcSrc.DeepCopy();
            foreach (var pk in pkgLinks)
                if (!npc.Packages.Any(x => x.FormKey == pk)) npc.Packages.Add(new FormLink<IPackageGetter>(pk));
            mod.Npcs.Add(npc);
            report.Add($"phase2 NPC override {npcEdid}: master record copied verbatim + {pkgLinks.Count} package(s) appended ({npc.Packages.Count} total)");

            // move her ACHR into the clinic (override keeps the formid, so the DLL's npc map is untouched)
            string refEdid = p2["vargaRefEdid"]!.GetValue<string>();
            IPlacedNpcGetter? achrSrc = null;
            foreach (var cg in dcBaseMod.EnumerateMajorRecords<IPlacedNpcGetter>())
                if (string.Equals(cg.EditorID, refEdid, StringComparison.OrdinalIgnoreCase)) achrSrc = cg;
            if (achrSrc == null) throw new GenError($"phase2: ACHR '{refEdid}' not in {cwKey}");
            var achr = achrSrc.DeepCopy();
            achr.Position = P3(p2["varga"]!["pos"]!);
            achr.Rotation = new P3Float(0, 0, (float)p2["varga"]!["rotZ"]!.GetValue<double>());
            // her office seat: DefaultSitLinkCustom01NoEating sits the actor in the ref LinkCustom01 points at (Kessler's pattern)
            string seatEdid = p2["officeSeatEdid"]!.GetValue<string>();
            if (!persistentDress.TryGetValue(seatEdid, out var seatRef)) throw new GenError($"phase2: office seat '{seatEdid}' is not a persistent dressing ref");
            achr.LinkedReferences.Clear();
            achr.LinkedReferences.Add(new LinkedReferences
            {
                KeywordOrReference = new FormLink<IKeywordLinkedReferenceGetter>(Fk.Parse("Fallout4.esm|05D5E6")),
                Reference = new FormLink<IPlacedGetter>(seatRef.FormKey),
            });
            cell.Persistent.Add(achr);
            report.Add($"phase2: {refEdid} ({achr.FormKey}) moved into {cellEdid} at {achr.Position}; LinkCustom01 -> {seatEdid} {seatRef.FormKey}");

            // the menu chair: a persistent ref of Cyberware.esp's ACTI; SPEC points formmap ripperdocChairs at it
            string chairEdid = p2["chair"]!["baseEdid"]!.GetValue<string>();
            var chairBase = dcBaseMod.Activators.FirstOrDefault(x => string.Equals(x.EditorID, chairEdid, StringComparison.OrdinalIgnoreCase))
                            ?? throw new GenError($"phase2: ACTI '{chairEdid}' not in {cwKey}");
            var chairRef = new PlacedObject(K("CW_ClinicChair"), Fallout4Release.Fallout4)
            {
                EditorID = "CW_ClinicChair",
                Position = P3(p2["chair"]!["pos"]!),
                Rotation = new P3Float(0, 0, (float)p2["chair"]!["rotZ"]!.GetValue<double>()),
            };
            chairRef.Base.SetTo(chairBase.FormKey);
            chairRef.MajorRecordFlagsRaw |= 0x400;
            cell.Persistent.Add(chairRef);
            report.Add($"phase2: menu chair ref {chairRef.FormKey} (formmap refForm \"{Me.FileName}|{chairRef.FormKey.ID:X3}\")");

            // INSTALL SCENE (docs/INSTALL-SCENE.md): the patient seat the player is put into, co-located with the
            // CP surgical chair (a STAT, so nobody can sit in it), and her mark at the patient's shoulder. Both persistent.
            // "patientSeatB" is the SECOND CANDIDATE for one boot to decide between (SEAT-XMRK-CAUSE.md): same shape, its own
            // spec entry, its own ids. Delete it from the spec once the boot has decided; its lock ids then retire by themselves.
            foreach (var seatKey in new[] { "patientSeat", "patientSeatB" })
            if (p2[seatKey] is JsonNode ps)
            {
                var srcKey = Fk.Parse(ps["copyOf"]!.GetValue<string>());
                var src = esm.Furniture.FirstOrDefault(f => f.FormKey == srcKey) ?? throw new GenError($"phase2 {seatKey}: {srcKey} is not a FURN in the esm");
                string bedid = ps["baseEdid"]!.GetValue<string>();
                var furn = src.Duplicate(K(bedid));
                furn.EditorID = bedid;
                furn.VirtualMachineAdapter = null;      // no vanilla lounger script rides along; the DLL drives the scene
                // The name is the spec's, never the donor's: the donor's FULL is a localized string, and Mutagen resolves it only when
                // Strings\Fallout4_en.* sit next to the esm (the build machine's pinned masters: "Memory Lounger"; the rig and Linux: "").
                // So the esp depended on the master folder's layout (2026-11-10). "" or absent = no FULL: no crosshair prompt.
                string seatName = ps["name"]?.GetValue<string>() ?? "";
                furn.Name = seatName.Length > 0 ? seatName : null;
                if (ps["model"] is JsonNode pm) furn.Model = new Model { File = pm.GetValue<string>() };
                // XMRK, the marker model. An invisible MODL carries no markers and no behaviour graph, so the record that
                // swaps one in MUST name a marker model: that is where vanilla keeps the markers of every invisible
                // furniture (201 of the 207 invisible-model FURN in Fallout4.esm carry XMRK), including the very precedent
                // this seat was copied from (NPCMutantBodyExamine 1B1F5B -> Markers\MarkerNPCMutantBodyExamine.nif) and
                // vanilla's own invisible PLAYER-scene furniture (CryoPodSceneFurniture 0446F6 ->
                // Markers\MarkerCryoPodScene.nif, which carries Furniture\CryoPod\CryoPod01.hkx). Without it nothing tells
                // the engine where or how to seat an actor: ActivateRef is accepted and no sit ever settles (SCN-T0).
                // See docs/integration/SEAT-XMRK-CAUSE.md.
                if (ps["markerModel"] is JsonNode pmm) furn.MarkerModel = pmm.GetValue<string>();
                mod.Furniture.Add(furn);
                string redid = ps["refEdid"]!.GetValue<string>();
                var seat = new PlacedObject(K(redid), Fallout4Release.Fallout4)
                {
                    EditorID = redid,
                    Position = P3(ps["pos"]!),
                    Rotation = new P3Float(0, 0, (float)ps["rotZ"]!.GetValue<double>()),
                };
                seat.Base.SetTo(furn.FormKey);
                seat.MajorRecordFlagsRaw |= 0x400;
                cell.Persistent.Add(seat);
                report.Add($"phase2: patient seat {redid} {seat.FormKey} (FURN {bedid}, copy of {src.EditorID}, model {furn.Model?.File}, marker model {furn.MarkerModel ?? "(none)"})");
            }
            // CRASH FIX A: the on-mesh sandbox anchor her station packages leash to. Same shape as the surgeon mark - a
            // persistent XMarkerHeading - because the fault was never the package, it was that CW_ClinicChair sits 15.1 u
            // off the generated navmesh and a "near reference" sandbox there makes the engine search for the nearest
            // navmesh point on every evaluation. See phase2.sandboxAnchor's note.
            if (p2["sandboxAnchor"] is JsonNode sa)
            {
                string aedid = sa["refEdid"]!.GetValue<string>();
                var anchor = new PlacedObject(K(aedid), Fallout4Release.Fallout4)
                {
                    EditorID = aedid,
                    Position = P3(sa["pos"]!),
                    Rotation = new P3Float(0, 0, (float)sa["rotZ"]!.GetValue<double>()),
                };
                anchor.Base.SetTo(Base(sa["base"]!.GetValue<string>()));
                anchor.MajorRecordFlagsRaw |= 0x400;        // persistent: the packages link to it by form id
                cell.Persistent.Add(anchor);
                report.Add($"phase2: sandbox anchor {aedid} {anchor.FormKey} at {anchor.Position}");
            }

            if (p2["surgeonMark"] is JsonNode sm)
            {
                string medid = sm["refEdid"]!.GetValue<string>();
                var mark = new PlacedObject(K(medid), Fallout4Release.Fallout4)
                {
                    EditorID = medid,
                    Position = P3(sm["pos"]!),
                    Rotation = new P3Float(0, 0, (float)sm["rotZ"]!.GetValue<double>()),
                };
                mark.Base.SetTo(Base(sm["base"]!.GetValue<string>()));
                mark.MajorRecordFlagsRaw |= 0x400;
                cell.Persistent.Add(mark);
                report.Add($"phase2: surgeon mark {medid} {mark.FormKey} at {mark.Position}");
            }

            // her bed: vanilla hospital frame (STAT) + mattress (FURN) at the same pos/rot, exactly as vanilla pairs them
            var bedPos = P3(p2["bed"]!["pos"]!);
            var bedRot = new P3Float(0, 0, (float)p2["bed"]!["rotZ"]!.GetValue<double>());
            foreach (var (edid, baseName) in new[] { ("CW_ClinicBedFrame", "HospitalBed01Frame"), ("CW_ClinicBed", "NpcBedHospitalMattressLay01") })
            {
                var r = new PlacedObject(K(edid), Fallout4Release.Fallout4) { EditorID = edid, Position = bedPos, Rotation = bedRot };
                r.Base.SetTo(Base(baseName));
                cell.Temporary.Add(r);
            }
            report.Add("phase2: hospital bed (frame + lay-furniture) placed behind the divider");
            // Station lighting (phase2.lights): our own LIGH records copied from vanilla, then tuned. Aim comes from "rot".
            int lights = 0;
            if (p2["lights"] is JsonArray lightList)
                foreach (var lj in lightList)
                {
                    string ledid = lj!["edid"]!.GetValue<string>();
                    var srcKey = Fk.Parse(lj["copyOf"]!.GetValue<string>());
                    var src = esm.Lights.FirstOrDefault(x => x.FormKey == srcKey) ?? throw new GenError($"phase2 light {ledid}: {srcKey} is not a LIGH in the esm");
                    var li = src.Duplicate(K(ledid));
                    li.EditorID = ledid;
                    if (lj["radius"] is JsonNode lr) li.Radius = lr.GetValue<uint>();
                    if (lj["fov"] is JsonNode lf) li.FOV = (float)lf.GetValue<double>();
                    if (lj["nearClip"] is JsonNode ln) li.NearClip = (float)ln.GetValue<double>();
                    if (lj["fade"] is JsonNode lfa) li.FadeValue = (float)lfa.GetValue<double>();
                    if (lj["color"] is JsonArray lc) li.Color = System.Drawing.Color.FromArgb(lc[0]!.GetValue<int>(), lc[1]!.GetValue<int>(), lc[2]!.GetValue<int>());
                    mod.Lights.Add(li);
                    var rot = (JsonArray)lj["rot"]!;
                    var r = new PlacedObject(K(ledid + "_Ref"), Fallout4Release.Fallout4)
                    {
                        EditorID = ledid + "_Ref",
                        Position = P3(lj["pos"]!),
                        Rotation = P3(rot),
                    };
                    r.Base.SetTo(li.FormKey);
                    cell.Temporary.Add(r);
                    lights++;
                }
            if (p2["cellLighting"] is JsonNode cl && cell.Lighting is { } lit)
            {
                static System.Drawing.Color C(JsonNode n) { var c = (JsonArray)n; return System.Drawing.Color.FromArgb(c[0]!.GetValue<int>(), c[1]!.GetValue<int>(), c[2]!.GetValue<int>()); }
                lit.FogNear = (float)cl["fogNear"]!.GetValue<double>();
                lit.FogFar = (float)cl["fogFar"]!.GetValue<double>();
                lit.FogMax = (float)cl["fogMax"]!.GetValue<double>();
                lit.FogNearColor = C(cl["fogNearColor"]!);
                lit.FogFarColor = C(cl["fogFarColor"]!);
                lit.DirectionalColor = C(cl["directionalColor"]!);
            }
            report.Add($"phase2: {lights} station light(s), cell fog retuned");
            // Original genre clutter that replaces the swapped crates: vanilla Fallout4.esm bases only (phase2.clutter).
            int clutter = 0;
            if (p2["clutter"] is JsonArray clutterList)
                foreach (var d in clutterList)
                {
                    string baseName = d!["base"]!.GetValue<string>();
                    var r = new PlacedObject(K($"Clutter_{clutter:D3}_{baseName}"), Fallout4Release.Fallout4)
                    {
                        Position = P3(d["pos"]!),
                        Rotation = new P3Float(0, 0, (float)d["rotZ"]!.GetValue<double>()),
                    };
                    r.Base.SetTo(Base(baseName));
                    Pin(r, baseName);              // CLNCL4: hold loose clutter where the spec puts it
                    if (d["scale"] is JsonNode cs) r.Scale = (float)cs.GetValue<double>();
                    cell.Temporary.Add(r);
                    clutter++;
                }
            // Statics from a mesh path under Meshes\: our own painted signs, and vanilla meshes that have no plain STAT.
            int ownStatics = 0;
            var ownStat = new Dictionary<string, FormKey>(StringComparer.OrdinalIgnoreCase);
            if (p2["statics"] is JsonArray staticList)
                foreach (var d in staticList)
                {
                    string model = d!["model"]!.GetValue<string>();
                    string stem = Path.GetFileNameWithoutExtension(model.Replace('\\', '/'));
                    if (!ownStat.TryGetValue(model, out var sk))
                    {
                        var st = new Static(K("CWC_Own_" + stem), Fallout4Release.Fallout4) { EditorID = "CWC_Own_" + stem, Model = new Model { File = model } };
                        mod.Statics.Add(st);
                        sk = st.FormKey;
                        ownStat[model] = sk;
                    }
                    var r = new PlacedObject(K($"OwnRef_{ownStatics:D3}_{stem}"), Fallout4Release.Fallout4)
                    {
                        Position = P3(d["pos"]!),
                        Rotation = new P3Float(0, 0, (float)d["rotZ"]!.GetValue<double>()),
                    };
                    r.Base.SetTo(sk);
                    if (d["scale"] is JsonNode cs) r.Scale = (float)cs.GetValue<double>();
                    cell.Temporary.Add(r);
                    ownStatics++;
                }
            report.Add($"phase2: {clutter} vanilla clutter refs, {ownStatics} own-model refs");
            report.Add(noSettle
                ? $"phase2: {pinned} loose dress/clutter ref(s) carry Don't Havok Settle (0x{NoSettleFlag:X8}) - CLNCL4"
                : "phase2: propRest.noSettle is off, loose clutter is left for havok to settle");
            // Her 10:00-14:00 Diamond City market walk needs NO record here: the DLL's native mover
            // ([Clinic] bSchedule) teleports her off-screen to ini coordinates in DiamondCityOrigin and back,
            // and CW_ClinicWalk10to14 sandboxes her CURRENT location. No vanilla worldspace cell is overridden.
        }

        // ---- write + verify round trip
        string espPath = Path.Combine(outDir, spec["plugin"]!.GetValue<string>());
        mod.WriteToBinary(espPath, new BinaryWriteParameters
        {
            MastersListOrdering = new MastersListOrderingByLoadOrder(new[]
            { ModKey.FromFileName("Fallout4.esm"), ModKey.FromFileName("Cyberware.esp") }),
        });
        // --frozen never writes the committed lock (2026-11-10, D1): Save retires the ids this run did not use and revives
        // retired ones, so a Phase 1 (dev) run - the ESPGEN gate's exec leg - rewrote clinic.lock.json in the gate's checkout.
        // A frozen run only reads the lock; what it used goes beside the esp for the record.
        lk.Save(lk.Frozen ? Path.Combine(outDir, "clinic.lock.used.json") : (lockPath ?? Path.Combine(outDir, "clinic.lock.json")));
        using (var back = Fallout4Mod.CreateFromBinaryOverlay(espPath, Fallout4Release.Fallout4))
        {
            int n = back.EnumerateMajorRecords().Count();
            bool esl = back.ModHeader.Flags.HasFlag(Fallout4ModHeader.HeaderFlag.Small);
            if (!esl) throw new GenError("round trip: ESL flag lost");
            report.Insert(0, $"wrote {espPath}: ESL, {n} records, masters [{string.Join(", ", back.ModHeader.MasterReferences.Select(m => m.Master.FileName))}]");
        }
        foreach (var line in report) Console.WriteLine(line);
        File.WriteAllText(Path.Combine(outDir, "clinic-report.json"),
            new JsonObject
            {
                ["specSha256"] = specSha,
                ["espSha256"] = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(espPath))).ToLowerInvariant(),
                ["report"] = new JsonArray(report.Select(r => (JsonNode?)r).ToArray()),
            }.ToJsonString(new System.Text.Json.JsonSerializerOptions { WriteIndented = true }) + "\n");
        return 0;
    }

    static P3Float P3(JsonNode n)
    {
        var a = (JsonArray)n;
        return new P3Float((float)a[0]!.GetValue<double>(), (float)a[1]!.GetValue<double>(), (float)a[2]!.GetValue<double>());
    }

    static TeleportDestination Tele(FormKey otherDoor, JsonArray land)
    {
        var t = new TeleportDestination
        {
            Position = new P3Float((float)land[0]!.GetValue<double>(), (float)land[1]!.GetValue<double>(), (float)land[2]!.GetValue<double>()),
            Rotation = new P3Float(0, 0, (float)land[3]!.GetValue<double>()),
        };
        t.Door.SetTo(otherDoor);
        return t;
    }

    static Mutagen.Bethesda.Plugins.Cache.IModContext<IFallout4Mod, IFallout4ModGetter, ICell, ICellGetter> WinningCellContext(IFallout4ModDisposableGetter m, FormKey cellKey)
    {
        var cache = m.ToImmutableLinkCache();
        foreach (var ctx in m.EnumerateMajorRecordContexts<ICell, ICellGetter>(cache))
            if (ctx.Record.FormKey == cellKey) return ctx;
        throw new GenError($"{m.ModKey} has no CELL {cellKey}");
    }
}
