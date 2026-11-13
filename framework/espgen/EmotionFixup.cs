// EmotionFixup - a response's "no emotion" is FFFFFFFF in Fallout4.esm; Mutagen 0.54.4 writes a null FormLink as
// 00000000. Voices.cs has patched that for its own plugin since the first-Say crash. The dialogue guard's one info
// (CW_RipperdocBlockInfo, written into Cyberware.esp with --ripperdoc-dialogue-guard) never got the patch, so its
// TRDA carried emotion 00000000: a NULL form inside TESResponse, the function boot C of 2026-11-12 crashed in (on a
// vanilla quest, so that crash is not shown to be this record; this is the same defect waiting in ours).
//
// Fix(path)        every TRDA of every INFO in the written plugin: emotion 00000000 -> FFFFFFFF. Sizes do not change.
//                  It walks the record structure (groups, records, subrecords); it never scans for a byte pattern.
// AssertNone(path) reads the file back and THROWS if any response still carries emotion 00000000, so a generator
//                  that writes one can no longer finish. Called for every plugin of ours that holds an info.
// MEASURED on Fallout4.esm ae7be31a (tools/check_info_forms.py --census, 2026-11-12): 74,998 TRDA, all 20 bytes;
// emotion is FFFFFFFF on 33,999 and a form on 40,999, and 00000000 on NONE. The sound field is 00000000 on 74,847 and
// a form on 151, never FFFFFFFF: so a zero sound is vanilla's "no sound file" and is left alone.
using System.Buffers.Binary;
using System.Text;

static class EmotionFixup
{
    // One pass over the plugin. patch = rewrite a null emotion; returns how many responses had one.
    static int Walk(byte[] b, bool patch, List<string> where)
    {
        int found = 0, pos = 0;
        while (pos + 24 <= b.Length)
        {
            string type = Encoding.ASCII.GetString(b, pos, 4);
            if (type == "GRUP") { pos += 24; continue; }               // step into the group: its children follow its header
            int size = (int)BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(pos + 4));
            uint flags = BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(pos + 8));
            uint formId = BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(pos + 12));
            if (type == "INFO")
            {
                if ((flags & 0x00040000) != 0)
                    throw new InvalidOperationException($"EmotionFixup: INFO {formId:X8} is compressed; its responses cannot be read in place. Write it uncompressed.");
                int p = pos + 24, end = pos + 24 + size;
                uint big = 0;
                while (p + 6 <= end)
                {
                    string sub = Encoding.ASCII.GetString(b, p, 4);
                    int len = BinaryPrimitives.ReadUInt16LittleEndian(b.AsSpan(p + 4));
                    if (sub == "XXXX") { big = BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(p + 6)); p += 6 + len; continue; }
                    if (big != 0) { len = (int)big; big = 0; }
                    if (sub == "TRDA")
                    {
                        if (len < 4) throw new InvalidOperationException($"EmotionFixup: INFO {formId:X8} has a TRDA of {len} bytes");
                        if (BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(p + 6)) == 0)
                        {
                            found++;
                            where.Add($"{formId:X8}");
                            if (patch) BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(p + 6), 0xFFFFFFFF);
                        }
                    }
                    p += 6 + len;
                }
            }
            pos += 24 + size;
        }
        return found;
    }

    public static int Fix(string path)
    {
        var b = File.ReadAllBytes(path);
        var where = new List<string>();
        int n = Walk(b, true, where);
        if (n > 0) File.WriteAllBytes(path, b);
        return n;
    }

    public static void AssertNone(string path)
    {
        var where = new List<string>();
        int n = Walk(File.ReadAllBytes(path), false, where);
        if (n > 0)
            throw new InvalidOperationException($"EmotionFixup: {Path.GetFileName(path)} still has {n} response(s) with emotion 00000000 (a NULL form) in INFO {string.Join(", ", where)}; vanilla's \"no emotion\" is FFFFFFFF");
    }

    // ---------------------------------------------------------------- force subtitle
    // TOPIC_INFO_FLAGS bit 0x0200 = Force Subtitle, the first u16 of an INFO's ENAM. Mutagen 0.54.4 exposes the ENAM struct
    // but not this bit by a name this generator can rely on, so it is OR'd in here, in place: ENAM is 4 bytes in every one of
    // Fallout4.esm's 78,089 INFOs and in ours, so no record, group or file size moves (the same property that makes
    // EmotionFixup safe). It matters twice: the ripperdoc's greeting and replies are TEXT ONLY in this update, so without it a
    // player with dialogue subtitles off reads nothing; and the DLL recognises the shop choice by finding that INFO in the
    // SubtitleManager, which only holds a line the engine actually displayed.
    //
    // ForceSubtitleOn(path, ids)   OR 0x0200 into the ENAM of exactly these INFO FormIDs (local ids, 0x800-0xFFF). Returns how
    //                              many it set. An id with no ENAM is a build failure: silently not forcing it would ship a
    //                              line nobody can read.
    // AssertForcedSubtitle(...)    reads the file back and THROWS unless every one of those ids has the bit set.
    const ushort kForceSubtitle = 0x0200;

    static int WalkEnam(byte[] b, HashSet<uint> ids, bool patch, List<string> missing, out HashSet<uint> seen)
    {
        int found = 0, pos = 0;
        seen = new HashSet<uint>();
        while (pos + 24 <= b.Length)
        {
            string type = Encoding.ASCII.GetString(b, pos, 4);
            if (type == "GRUP") { pos += 24; continue; }
            int size = (int)BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(pos + 4));
            uint flags = BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(pos + 8));
            uint formId = BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(pos + 12));
            if (type == "INFO" && ids.Contains(formId & 0x00FFFFFF))
            {
                if ((flags & 0x00040000) != 0)
                    throw new InvalidOperationException($"EmotionFixup: INFO {formId:X8} is compressed; its ENAM cannot be read in place. Write it uncompressed.");
                int p = pos + 24, end = pos + 24 + size;
                uint big = 0;
                bool hasEnam = false;
                while (p + 6 <= end)
                {
                    string sub = Encoding.ASCII.GetString(b, p, 4);
                    int len = BinaryPrimitives.ReadUInt16LittleEndian(b.AsSpan(p + 4));
                    if (sub == "XXXX") { big = BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(p + 6)); p += 6 + len; continue; }
                    if (big != 0) { len = (int)big; big = 0; }
                    if (sub == "ENAM")
                    {
                        if (len < 2) throw new InvalidOperationException($"EmotionFixup: INFO {formId:X8} has an ENAM of {len} bytes");
                        hasEnam = true;
                        ushort v = BinaryPrimitives.ReadUInt16LittleEndian(b.AsSpan(p + 6));
                        if (patch && (v & kForceSubtitle) == 0)
                            BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(p + 6), (ushort)(v | kForceSubtitle));
                        // patch: the bit is set once this returns. assert: count only what already carried it.
                        if (patch || (v & kForceSubtitle) != 0) found++;
                    }
                    p += 6 + len;
                }
                if (!hasEnam) missing.Add($"{formId:X8}");
                else seen.Add(formId & 0x00FFFFFF);
            }
            pos += 24 + size;
        }
        return found;
    }

    public static int ForceSubtitleOn(string path, IEnumerable<uint> localIds)
    {
        var ids = localIds.Select(i => i & 0x00FFFFFFu).ToHashSet();
        if (ids.Count == 0) return 0;
        var b = File.ReadAllBytes(path);
        var missing = new List<string>();
        int n = WalkEnam(b, ids, true, missing, out var seen);
        if (missing.Count > 0)
            throw new InvalidOperationException($"EmotionFixup.ForceSubtitleOn: INFO {string.Join(", ", missing)} has no ENAM, so Force Subtitle cannot be set. Set info.Flags = new DialogResponseFlags() on it.");
        var absent = ids.Where(i => !seen.Contains(i)).ToList();
        if (absent.Count > 0)
            throw new InvalidOperationException($"EmotionFixup.ForceSubtitleOn: {string.Join(", ", absent.Select(i => i.ToString("X3")))} is not an INFO in {Path.GetFileName(path)}");
        File.WriteAllBytes(path, b);
        return n;
    }

    public static void AssertForcedSubtitle(string path, IEnumerable<uint> localIds)
    {
        var ids = localIds.Select(i => i & 0x00FFFFFFu).ToHashSet();
        if (ids.Count == 0) return;
        var missing = new List<string>();
        int n = WalkEnam(File.ReadAllBytes(path), ids, false, missing, out var seen);
        if (missing.Count > 0 || n != ids.Count)
            throw new InvalidOperationException($"EmotionFixup: {Path.GetFileName(path)} has Force Subtitle (ENAM 0x0200) on {n} of {ids.Count} ripperdoc dialogue INFO(s)"
                                                + (missing.Count > 0 ? $"; no ENAM at all on {string.Join(", ", missing)}" : ""));
    }
}
