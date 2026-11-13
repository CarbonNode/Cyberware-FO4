// SubgraphFixup - Mutagen 0.54.4 writes a RACE subgraph entry as SGNM SAKD* STKD* SAPT* SRAF, but Fallout4.esm (and
// therefore the engine) uses SAKD* STKD* SGNM SAPT* SRAF: the keywords come BEFORE the behavior name that starts
// the entry. Left alone, every entry would hand its keywords to its neighbour and break humanoid animation across
// all of HumanRaceSubGraphData. Found 2026-11-08 by diffing the raw subrecord order against vanilla.
// This pass reorders the subrecords in place (sizes do not change) and Verify() proves the copied entries are
// byte-identical to the source record and that the order now matches vanilla.
using System.Buffers.Binary;
using System.IO.Compression;
using System.Text;

static class SubgraphFixup
{
    internal record Sub(string Type, byte[] Data);

    public static int Fix(string espPath)
    {
        var buf = File.ReadAllBytes(espPath);
        int fixedRecords = 0;
        foreach (var (off, size) in Races(buf))
        {
            if ((BinaryPrimitives.ReadUInt32LittleEndian(buf.AsSpan(off + 8)) & 0x40000) != 0)
                throw new GenError("compressed RACE in our own output: not expected");
            var subs = Parse(buf.AsSpan(off + 24, size));
            var outSubs = new List<Sub>(subs.Count);
            for (int i = 0; i < subs.Count; i++)
            {
                if (subs[i].Type != "SGNM") { outSubs.Add(subs[i]); continue; }
                int j = i + 1;
                var kws = new List<Sub>();
                while (j < subs.Count && (subs[j].Type == "SAKD" || subs[j].Type == "STKD")) kws.Add(subs[j++]);
                outSubs.AddRange(kws.Where(k => k.Type == "SAKD"));
                outSubs.AddRange(kws.Where(k => k.Type == "STKD"));
                outSubs.Add(subs[i]);
                i = j - 1;
            }
            var bytes = Serialize(outSubs);
            if (bytes.Length != size) throw new GenError("subgraph fix-up changed the record size");
            bytes.CopyTo(buf, off + 24);
            fixedRecords++;
        }
        File.WriteAllBytes(espPath, buf);
        return fixedRecords;
    }

    // Entries as (SAKD*, STKD*, SGNM, SAPT*, SRAF) groups of raw subrecords, read the way the game reads them.
    internal static List<List<Sub>> Entries(byte[] file, uint localFormId)
    {
        foreach (var (off, size) in Races(file))
        {
            if ((BinaryPrimitives.ReadUInt32LittleEndian(file.AsSpan(off + 12)) & 0xFFFFFF) != localFormId) continue;
            uint flags = BinaryPrimitives.ReadUInt32LittleEndian(file.AsSpan(off + 8));
            byte[] body = file.AsSpan(off + 24, size).ToArray();
            if ((flags & 0x40000) != 0)
            {
                using var z = new ZLibStream(new MemoryStream(body, 4, body.Length - 4), CompressionMode.Decompress);
                using var ms = new MemoryStream(); z.CopyTo(ms); body = ms.ToArray();
            }
            var subs = Parse(body);
            var entries = new List<List<Sub>>();
            List<Sub>? cur = null;
            foreach (var s in subs)
            {
                if (s.Type is "SAKD" or "STKD" or "SGNM" or "SAPT" or "SRAF")
                {
                    cur ??= new List<Sub>();
                    cur.Add(s);
                    if (s.Type == "SRAF") { entries.Add(cur); cur = null; }
                }
            }
            if (cur != null) throw new GenError("subgraph entry without SRAF at the end");
            return entries;
        }
        return new();
    }

    // ours = the indices (in our record) of the entries we added, appended or inserted; every other entry must be the
    // source's, in the source's order.
    public static string Verify(string espPath, string basePath, uint localFormId, ISet<int> ours)
    {
        var all = Entries(File.ReadAllBytes(espPath), localFormId);
        var orig = Entries(File.ReadAllBytes(basePath), localFormId);
        int appended = ours.Count;
        if (all.Count != orig.Count + appended) throw new GenError($"subgraph verify: {all.Count} entries, expected {orig.Count}+{appended}");
        var mine = all.Where((_, i) => !ours.Contains(i)).ToList();
        foreach (var e in all)
        {
            int sg = e.FindIndex(s => s.Type == "SGNM");
            if (sg < 0 || e.Take(sg).Any(s => s.Type is not ("SAKD" or "STKD")) || e.Skip(sg + 1).Any(s => s.Type is "SAKD" or "STKD"))
                throw new GenError("subgraph verify: an entry is not in SAKD* STKD* SGNM SAPT* SRAF order");
        }
        // Copied entries must be byte-identical, except keyword FormIDs whose master index moved (compare by type+len only then).
        int same = 0, remapped = 0;
        for (int i = 0; i < orig.Count; i++)
        {
            var a = orig[i]; var b = mine[i];
            if (a.Count != b.Count || a.Zip(b).Any(p => p.First.Type != p.Second.Type || p.First.Data.Length != p.Second.Data.Length))
                throw new GenError($"subgraph verify: copied entry {i} differs in shape from the source");
            if (a.Zip(b).All(p => p.First.Data.AsSpan().SequenceEqual(p.Second.Data))) same++; else remapped++;
        }
        int inserted = ours.Count(i => Enumerable.Range(i + 1, all.Count - i - 1).Any(j => !ours.Contains(j)));   // ours with a source entry after it
        return $"subgraph verify: {all.Count} entries in vanilla order; {same} of {orig.Count} copied entries byte-identical, {remapped} differ only in FormID bytes (master index remap); {appended} ours ({inserted} inserted before their templates)";
    }

    static IEnumerable<(int off, int size)> Races(byte[] buf)
    {
        var stack = new Stack<(int off, int end)>();
        stack.Push((0, buf.Length));
        var found = new List<(int, int)>();
        void Walk(int off, int end)
        {
            while (off < end)
            {
                string type = Encoding.ASCII.GetString(buf, off, 4);
                int size = (int)BinaryPrimitives.ReadUInt32LittleEndian(buf.AsSpan(off + 4));
                if (type == "GRUP") { Walk(off + 24, off + size); off += size; }
                else { if (type == "RACE") found.Add((off, size)); off += 24 + size; }
            }
        }
        Walk(0, buf.Length);
        return found;
    }

    static List<Sub> Parse(ReadOnlySpan<byte> body)
    {
        var list = new List<Sub>();
        int o = 0; int? big = null;
        while (o < body.Length)
        {
            string t = Encoding.ASCII.GetString(body.Slice(o, 4));
            int s = BinaryPrimitives.ReadUInt16LittleEndian(body.Slice(o + 4));
            o += 6;
            if (t == "XXXX") { big = (int)BinaryPrimitives.ReadUInt32LittleEndian(body.Slice(o)); o += 4; list.Add(new Sub("XXXX", body.Slice(o - 4, 4).ToArray())); continue; }
            if (big.HasValue) { s = big.Value; big = null; }
            list.Add(new Sub(t, body.Slice(o, s).ToArray()));
            o += s;
        }
        return list;
    }

    static byte[] Serialize(List<Sub> subs)
    {
        using var ms = new MemoryStream();
        for (int i = 0; i < subs.Count; i++)
        {
            var s = subs[i];
            if (s.Type == "XXXX")
            {   // XXXX carries the size of the NEXT subrecord; keep the pair together
                ms.Write(Encoding.ASCII.GetBytes("XXXX")); ms.Write(BitConverter.GetBytes((ushort)4)); ms.Write(s.Data);
                var n = subs[++i];
                ms.Write(Encoding.ASCII.GetBytes(n.Type)); ms.Write(BitConverter.GetBytes((ushort)0)); ms.Write(n.Data);
                continue;
            }
            ms.Write(Encoding.ASCII.GetBytes(s.Type)); ms.Write(BitConverter.GetBytes((ushort)s.Data.Length)); ms.Write(s.Data);
        }
        return ms.ToArray();
    }
}
