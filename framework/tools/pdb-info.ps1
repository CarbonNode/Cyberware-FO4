# pdb-info.ps1 - does this PDB belong to this DLL? Reads both files directly (no dumpbin, no DIA). ASCII only.
# Dot-source it:  . .\pdb-info.ps1;  Get-PeCodeView <dll>;  Get-PdbInfo <pdb>;  Assert-PdbMatches <dll> <pdb>
# Why (2026-11-09): the rig's release build wrote no PDB, so the first Sandevistan crash was read against the compile lane's PDB,
# which came from a different build (builds are not byte-reproducible) and pointed at the wrong code. A debugger loads a PDB only
# when the DLL's CodeView record (RSDS: GUID + age) equals the PDB's own GUID and age. This checks exactly that.

# The DLL's CodeView record from its debug directory: @{ Guid; Age; PdbPath }. Throws if it has none.
function Get-PeCodeView([string]$Path) {
    $b = [IO.File]::ReadAllBytes($Path)
    if ($b.Length -lt 0x40 -or $b[0] -ne 0x4D -or $b[1] -ne 0x5A) { throw "$Path is not a PE file (no MZ)" }
    $pe = [BitConverter]::ToInt32($b, 0x3C)
    if ([BitConverter]::ToUInt32($b, $pe) -ne 0x4550) { throw "$Path is not a PE file (no PE signature)" }
    $nsec = [BitConverter]::ToUInt16($b, $pe + 6)
    $optSize = [BitConverter]::ToUInt16($b, $pe + 20)
    $opt = $pe + 24
    $dd = if ([BitConverter]::ToUInt16($b, $opt) -eq 0x20B) { $opt + 112 } else { $opt + 96 }   # PE32+ / PE32
    $dbgRva = [BitConverter]::ToUInt32($b, $dd + 6 * 8)       # data directory 6 = debug
    $dbgSize = [BitConverter]::ToUInt32($b, $dd + 6 * 8 + 4)
    if ($dbgRva -eq 0) { throw "$Path has no debug directory (built without /DEBUG)" }
    $secs = $opt + $optSize
    $off = -1
    for ($i = 0; $i -lt $nsec; $i++) {
        $s = $secs + 40 * $i
        $va = [BitConverter]::ToUInt32($b, $s + 12); $vs = [BitConverter]::ToUInt32($b, $s + 8)
        $raw = [BitConverter]::ToUInt32($b, $s + 20); $rs = [BitConverter]::ToUInt32($b, $s + 16)
        if ($dbgRva -ge $va -and $dbgRva -lt $va + [Math]::Max($vs, $rs)) { $off = [int]($raw + $dbgRva - $va) }
    }
    if ($off -lt 0) { throw ("debug directory RVA 0x{0:X} is in no section" -f $dbgRva) }
    for ($e = 0; $e -lt [int]($dbgSize / 28); $e++) {
        $d = $off + 28 * $e
        if ([BitConverter]::ToUInt32($b, $d + 12) -ne 2) { continue }   # IMAGE_DEBUG_TYPE_CODEVIEW
        $p = [int][BitConverter]::ToUInt32($b, $d + 24)                 # PointerToRawData
        if ([Text.Encoding]::ASCII.GetString($b, $p, 4) -ne 'RSDS') { throw "$Path CodeView record is not RSDS" }
        $g = New-Object byte[] 16; [Array]::Copy($b, $p + 4, $g, 0, 16)
        $end = $p + 24; while ($end -lt $b.Length -and $b[$end] -ne 0) { $end++ }
        return @{ Guid = (New-Object Guid (, $g)); Age = [BitConverter]::ToUInt32($b, $p + 20); PdbPath = [Text.Encoding]::ASCII.GetString($b, $p + 24, $end - $p - 24) }
    }
    throw "$Path has no CodeView debug entry"
}

# The PDB's own identity from its MSF 7.00 container: @{ Guid (info stream 1); Age (DBI stream 3, the age a debugger compares) }.
function Get-PdbInfo([string]$Path) {
    $b = [IO.File]::ReadAllBytes($Path)
    $magic = "Microsoft C/C++ MSF 7.00`r`n"
    if ($b.Length -lt 56 -or [Text.Encoding]::ASCII.GetString($b, 0, $magic.Length) -ne $magic) { throw "$Path is not an MSF 7.00 PDB" }
    $bs = [int][BitConverter]::ToUInt32($b, 32)
    $dirBytes = [int][BitConverter]::ToUInt32($b, 44)
    $mapAddr = [int][BitConverter]::ToUInt32($b, 52)
    if ($bs -notin 512, 1024, 2048, 4096, 8192, 16384, 32768) { throw "$Path has a bad MSF block size $bs" }
    # the directory, gathered from the blocks the block map lists
    $nDirBlocks = [int][Math]::Ceiling($dirBytes / $bs)
    $dir = New-Object byte[] ($nDirBlocks * $bs)
    for ($i = 0; $i -lt $nDirBlocks; $i++) { $blk = [int][BitConverter]::ToUInt32($b, $mapAddr * $bs + 4 * $i); [Array]::Copy($b, $blk * $bs, $dir, $i * $bs, $bs) }
    $nStreams = [int][BitConverter]::ToUInt32($dir, 0)
    if ($nStreams -lt 4) { throw "$Path has only $nStreams streams" }
    $sizes = @(for ($i = 0; $i -lt $nStreams; $i++) { $v = [BitConverter]::ToUInt32($dir, 4 + 4 * $i); if ($v -eq [uint32]::MaxValue) { 0 } else { [int]$v } })
    $pos = 4 + 4 * $nStreams
    $first = @{}   # first block of each stream: the 28 bytes read below always sit in it
    for ($s = 0; $s -lt $nStreams; $s++) {
        $nb = [int][Math]::Ceiling($sizes[$s] / $bs)
        if ($nb -gt 0) { $first[$s] = [int][BitConverter]::ToUInt32($dir, $pos) }
        $pos += 4 * $nb
    }
    if (-not $first.ContainsKey(1) -or $sizes[1] -lt 28) { throw "$Path has no PDB info stream" }
    if (-not $first.ContainsKey(3) -or $sizes[3] -lt 12) { throw "$Path has no DBI stream" }
    $info = $first[1] * $bs
    $g = New-Object byte[] 16; [Array]::Copy($b, $info + 12, $g, 0, 16)
    return @{ Guid = (New-Object Guid (, $g)); InfoAge = [BitConverter]::ToUInt32($b, $info + 8); Age = [BitConverter]::ToUInt32($b, $first[3] * $bs + 8) }
}

# Throws unless the PDB is the one this DLL was linked with. Returns "guid age N" for the log.
function Assert-PdbMatches([string]$Dll, [string]$Pdb) {
    if (-not (Test-Path $Pdb)) { throw "no PDB at $Pdb" }
    $cv = Get-PeCodeView $Dll; $pi = Get-PdbInfo $Pdb
    if ($cv.Guid -ne $pi.Guid -or $cv.Age -ne $pi.Age) { throw ("PDB does not match the DLL: DLL wants {0} age {1}, PDB is {2} age {3}" -f $cv.Guid, $cv.Age, $pi.Guid, $pi.Age) }
    return ("{0} age {1}" -f $cv.Guid, $cv.Age)
}
