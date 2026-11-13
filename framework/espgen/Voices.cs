// voices: builds "Cyberware - Voices.esp" (ESL) + voices-formmap.json from data/ripperdoc-voice.json (RIPPERDOC VOICES,
// docs/RIPPERDOC-VOICE.md). One custom DIAL + one INFO per voiced line, all in one start-game-enabled quest, plus a
// silent "hush" topic the DLL says to cut a line off. The DLL calls ObjectReference.Say on the doctor with the topic;
// the engine plays Sound\Voice\Cyberware - Voices.esp\<VoiceType>\<INFO id>_1.fuz with its .lip.
//
//   voices --manifest ../data/ripperdoc-voice.json --out <dir> [--lock ../data/voices.formids.lock.json] [--frozen]
//
// Records match the shape vanilla CUST Say topics use (Fallout4.esm: DATA category 0, TRDA emotion -1, unknown byte 1,
// cameras -1, ENAM and INAM present, text never empty). Response text is a single space so the vanilla subtitle stays
// blank but the response is not empty. No master is needed: the records reference nothing outside this plugin.
using System.Text.Json;
using System.Text.Json.Nodes;
using Mutagen.Bethesda;
using Mutagen.Bethesda.Fallout4;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Binary.Parameters;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Strings;

static class Voices
{
    public static readonly ModKey Me = ModKey.FromFileName("Cyberware - Voices.esp");

    // Vanilla's "no emotion" is FFFFFFFF in TRDA; Mutagen writes a null FormLink as 0. Patched in the written file.
    static void PatchEmotionNone(string path)
    {
        var b = File.ReadAllBytes(path);
        for (int i = 0; i + 26 <= b.Length; i++)
        {
            if (b[i] != 'T' || b[i + 1] != 'R' || b[i + 2] != 'D' || b[i + 3] != 'A' || b[i + 4] != 20 || b[i + 5] != 0) continue;
            if (b[i + 6 + 9] != 1) continue;                            // the Unknown byte we set on the vanilla shape
            if (BitConverter.ToUInt32(b, i + 6) != 0) continue;
            b[i + 6] = b[i + 7] = b[i + 8] = b[i + 9] = 0xFF;
        }
        File.WriteAllBytes(path, b);
    }

    public static int Run(Args a)
    {
        string manPath = a.Req("manifest"), outDir = a.Req("out");
        string lockPath = a.Opt("lock") ?? Path.Combine(Path.GetDirectoryName(Path.GetFullPath(manPath))!, "voices.formids.lock.json");
        var man = JsonNode.Parse(File.ReadAllText(manPath))!.AsObject();
        if ((string?)man["plugin"] != Me.FileName.String) throw new GenError($"manifest plugin must be {Me.FileName}");
        var lines = man["lines"]!.AsObject();
        Directory.CreateDirectory(outDir);

        var lk = IdLock.Load(lockPath);
        lk.Frozen = a.Opt("frozen") == "true";
        var mod = new Fallout4Mod(Me, Fallout4Release.Fallout4);
        mod.ModHeader.Flags |= Fallout4ModHeader.HeaderFlag.Small;   // ESL
        mod.ModHeader.Author = "Cyberware RIPPERDOC VOICES generator";
        mod.ModHeader.Description = "Ripperdoc voice topics. Generated from data/ripperdoc-voice.json. Do not edit by hand.";
        FormKey K(string edid) => new FormKey(Me, lk.Get(edid));

        var q = mod.Quests.AddNew(K("CW_RDV_Quest"));
        q.EditorID = "CW_RDV_Quest";
        q.Name = "Cyberware ripperdoc voices";
        q.Data = new QuestData { Flags = Quest.Flag.StartGameEnabled, Priority = 50 };

        DialogTopic Topic(string edid, string infoEdid)
        {
            var t = new DialogTopic(K(edid), Fallout4Release.Fallout4)
            {
                EditorID = edid,
                Quest = q.ToLink(),
                Priority = 50,
                Category = (DialogTopic.CategoryEnum)0,
                Subtype = DialogTopic.SubtypeEnum.Custom0,
                SubtypeName = new RecordType("CUST"),
            };
            var info = new DialogResponses(K(infoEdid), Fallout4Release.Fallout4) { EditorID = infoEdid };
            var resp = new DialogResponse { ResponseNumber = 1, InterruptPercentage = 0 };
            resp.Text = " ";
            resp.Emotion.SetTo(FormKey.Null);   // FFFFFFFF, the vanilla "no emotion"
            resp.Unknown = 1;
            resp.CameraTargetAlias = -1;
            resp.CameraLocationAlias = -1;
            info.Flags = new DialogResponseFlags();
            info.SubtitlePriority = (DialogResponses.SubtitlePriorityLevel)1;
            info.Responses.Add(resp);
            t.Responses.Add(info);
            q.DialogTopics.Add(t);
            return t;
        }

        var map = new JsonObject();
        var dll = new JsonObject();   // voices.json for Cyberware.dll: only lines with a measured clip (ms > 0)
        foreach (var (key, node) in lines.OrderBy(kv => kv.Key, StringComparer.Ordinal))
        {
            if (key.Length != 8 || !key.All(Uri.IsHexDigit)) throw new GenError("bad line key " + key);
            var t = Topic("CW_RDV_" + key, "CW_RDVI_" + key);
            map[key] = new JsonObject
            {
                ["doc"] = (string?)node!["doc"],
                ["topic"] = t.FormKey.ID.ToString("X3"),
                ["info"] = t.Responses[0].FormKey.ID.ToString("X3"),
            };
            int ms = node["ms"] is JsonValue v && v.TryGetValue<int>(out var m) ? m : 0;
            if (ms > 0) dll[key] = new JsonObject { ["doc"] = (string?)node["doc"], ["topic"] = t.FormKey.ID.ToString("X3"), ["ms"] = ms };
        }
        var hush = Topic("CW_RDV_Hush", "CW_RDVI_Hush");

        string espPath = Path.Combine(outDir, Me.FileName);
        mod.WriteToBinary(espPath, new BinaryWriteParameters { MastersListOrdering = new MastersListOrderingByLoadOrder(Array.Empty<ModKey>()) });
        PatchEmotionNone(espPath);
        EmotionFixup.AssertNone(espPath);   // the same rule Cyberware.esp is held to: no response with emotion 00000000
        using (var back = Fallout4Mod.Create(Fallout4Release.Fallout4).FromPath(espPath).Construct())   // verify by reading back
        {
            int topics = back.Quests.SelectMany(x => x.DialogTopics).Count();
            if (topics != lines.Count + 1) throw new GenError($"read-back: {topics} topics, expected {lines.Count + 1}");
        }
        var fm = new JsonObject
        {
            ["plugin"] = Me.FileName.String,
            ["quest"] = q.FormKey.ID.ToString("X3"),
            ["hush"] = new JsonObject { ["topic"] = hush.FormKey.ID.ToString("X3"), ["info"] = hush.Responses[0].FormKey.ID.ToString("X3") },
            ["lines"] = map,
        };
        File.WriteAllText(Path.Combine(outDir, "voices-formmap.json"), fm.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");
        var dj = new JsonObject
        {
            ["plugin"] = Me.FileName.String,
            ["hush"] = new JsonObject { ["topic"] = hush.FormKey.ID.ToString("X3") },
            ["lines"] = dll,
        };
        File.WriteAllText(Path.Combine(outDir, "voices.json"), dj.ToJsonString(new JsonSerializerOptions { WriteIndented = true }) + "\n");
        // --frozen never writes the lock (as gen and armory): Save() also moves every id this run did not use into
        // "retired", so a frozen build that rewrote it could change the committed lock it was meant to prove
        // (the build machine's exec-gate checkout drifted this way on 2026-11-10; CLINIC found the same bug in clinicgen).
        if (!lk.Frozen) lk.Save(lockPath, "Cyberware - Voices.esp FormID lock. Commit it. Never hand-edit an id that has shipped.");
        Console.WriteLine($"voices: {espPath} ({lines.Count} line topics + hush), lock {lockPath}{(lk.Frozen ? " (frozen: read only)" : "")}");
        return 0;
    }
}
