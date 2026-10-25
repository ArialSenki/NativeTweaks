<#
.SYNOPSIS
  Pasa todos los patrones AOB del fork ERNativeUI y de NativeTweaks contra un
  eldenring.exe y valida las RVAs fijas de game_build_profiles.hpp.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File tools\aob_scan.ps1
  powershell -ExecutionPolicy Bypass -File tools\aob_scan.ps1 -Exe D:\Games\eldenring.exe -Build v2_7_0_0 -Csv out.csv
#>
param(
    [string]$Exe = $(if ($env:NT_GAME_EXE) { $env:NT_GAME_EXE } else { "${env:ProgramFiles(x86)}\Steam\steamapps\common\ELDEN RING\Game\eldenring.exe" }),
    [string[]]$SourceDirs = @(
        (Join-Path $PSScriptRoot '..\..\ERNativeUI-main\src'),
        (Join-Path $PSScriptRoot '..\src')
    ),
    [string]$Build = 'v2_7_1_0',
    [string]$Csv = '',
    [int]$MaxListed = 4
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;

public class PeSection {
    public string Name; public uint Rva; public uint VSize; public uint RawPtr; public uint RawSize; public uint Chars;
    public bool Exec { get { return (Chars & 0x20000000) != 0; } }
}

public class PeImage {
    public byte[] Image; public ulong ImageBase; public uint TimeDateStamp;
    public List<PeSection> Sections = new List<PeSection>();

    public static PeImage Load(string path) {
        byte[] f = File.ReadAllBytes(path);
        int pe = BitConverter.ToInt32(f, 0x3C);
        if (BitConverter.ToUInt32(f, pe) != 0x4550) throw new Exception("No es un PE");
        var img = new PeImage();
        img.TimeDateStamp = BitConverter.ToUInt32(f, pe + 8);
        int nsec = BitConverter.ToUInt16(f, pe + 6);
        int optSize = BitConverter.ToUInt16(f, pe + 20);
        int opt = pe + 24;
        img.ImageBase = BitConverter.ToUInt64(f, opt + 24);
        uint sizeOfImage = BitConverter.ToUInt32(f, opt + 56);
        uint sizeOfHeaders = BitConverter.ToUInt32(f, opt + 60);
        img.Image = new byte[sizeOfImage];
        Array.Copy(f, 0, img.Image, 0, Math.Min(sizeOfHeaders, (uint)f.Length));
        int sh = opt + optSize;
        for (int i = 0; i < nsec; i++) {
            int o = sh + i * 40;
            var s = new PeSection();
            s.Name = System.Text.Encoding.ASCII.GetString(f, o, 8).TrimEnd('\0');
            s.VSize = BitConverter.ToUInt32(f, o + 8);
            s.Rva = BitConverter.ToUInt32(f, o + 12);
            s.RawSize = BitConverter.ToUInt32(f, o + 16);
            s.RawPtr = BitConverter.ToUInt32(f, o + 20);
            s.Chars = BitConverter.ToUInt32(f, o + 36);
            uint n = Math.Min(s.RawSize, s.VSize == 0 ? s.RawSize : s.VSize);
            if (s.RawPtr + n > f.Length) n = (uint)Math.Max(0, f.Length - s.RawPtr);
            if (s.Rva + n > sizeOfImage) n = sizeOfImage - s.Rva;
            Array.Copy(f, s.RawPtr, img.Image, s.Rva, n);
            img.Sections.Add(s);
        }
        return img;
    }

    public PeSection SectionOf(ulong rva) {
        foreach (var s in Sections)
            if (rva >= s.Rva && rva < s.Rva + Math.Max(s.VSize, s.RawSize)) return s;
        return null;
    }

    // Devuelve hasta 'limit' RVAs y el total de coincidencias. execOnly limita a secciones ejecutables.
    public int Scan(string pattern, bool execOnly, int limit, List<uint> hits) {
        string[] toks = pattern.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
        int n = toks.Length;
        byte[] val = new byte[n]; bool[] wild = new bool[n];
        for (int i = 0; i < n; i++) {
            if (toks[i].StartsWith("?")) wild[i] = true;
            else val[i] = Convert.ToByte(toks[i], 16);
        }
        int anchor = -1;
        for (int i = 0; i < n; i++) if (!wild[i]) { anchor = i; break; }
        if (anchor < 0) return -1;
        byte a = val[anchor];
        int total = 0;
        byte[] m = Image;
        foreach (var s in Sections) {
            if (execOnly && !s.Exec) continue;
            long start = s.Rva, end = (long)s.Rva + Math.Max(s.VSize, s.RawSize) - n;
            if (end > m.Length - n) end = m.Length - n;
            for (long p = start; p <= end; p++) {
                if (m[p + anchor] != a) continue;
                int k = 0;
                for (; k < n; k++) if (!wild[k] && m[p + k] != val[k]) break;
                if (k == n) { total++; if (hits.Count < limit) hits.Add((uint)p); }
            }
        }
        return total;
    }

    public bool MatchesAt(ulong rva, string pattern) {
        string[] toks = pattern.Split(new[] { ' ', '\t' }, StringSplitOptions.RemoveEmptyEntries);
        if (rva + (ulong)toks.Length > (ulong)Image.Length) return false;
        for (int i = 0; i < toks.Length; i++) {
            if (toks[i].StartsWith("?")) continue;
            if (Image[rva + (ulong)i] != Convert.ToByte(toks[i], 16)) return false;
        }
        return true;
    }

    public long Rel32Target(ulong rva) {
        if (rva + 5 > (ulong)Image.Length) return -1;
        byte op = Image[rva];
        if (op != 0xE8 && op != 0xE9) return -1;
        return (long)rva + 5 + BitConverter.ToInt32(Image, (int)rva + 1);
    }
}
'@

$hexTok = '^([0-9A-Fa-f]{2}|\?{1,2})$'

function Test-PatternText([string]$pat) {
    $toks = $pat -split ' '
    if ($toks.Count -lt 4) { return $false }
    if (@($toks | Where-Object { $_ -notmatch $hexTok }).Count) { return $false }
    return [bool]@($toks | Where-Object { $_ -notmatch '^\?' }).Count
}

function Get-Patterns([string]$file, [string]$text) {
    # Secuencias de literales adyacentes (solo separados por espacio/comentarios de linea).
    $rx = [regex]'"((?:[^"\\\r\n]|\\.)*)"(?:(?:\s|//[^\r\n]*)*"((?:[^"\\\r\n]|\\.)*)")*'
    foreach ($m in $rx.Matches($text)) {
        $parts = [regex]::Matches($m.Value, '"((?:[^"\\\r\n]|\\.)*)"') | ForEach-Object { $_.Groups[1].Value }
        $pat = (($parts -join ' ') -replace '\s+', ' ').Trim()
        if (-not (Test-PatternText $pat)) { continue }
        $line = ($text.Substring(0, $m.Index) -split "`n").Count
        $before = $text.Substring([Math]::Max(0, $m.Index - 240), [Math]::Min(240, $m.Index))
        $const = $null
        $cm = [regex]::Match($before, '(?:string_view|char\s*\*|char)\s+(?:const\s+)?([A-Za-z_]\w*)(?:\[\])?\s*=\s*$')
        if ($cm.Success) { $const = $cm.Groups[1].Value }
        $name = $const
        if (-not $name) {
            $nm = [regex]::Matches($before, '\b([A-Za-z_]\w*)\b')
            $ctx = $nm | Where-Object { $_.Value -notmatch '^(constexpr|std|string_view|const|char|auto|return|if|find_unique|rip_target|static|inline|game|s)$' } | Select-Object -Last 1
            $name = if ($ctx) { "($($ctx.Value))" } else { '?' }
        }
        # Papel especial: 'expected' de un Site (aob, offset, expected) o literal de validacion tras kXxxRva.
        $siteBase = $null; $siteOff = 0; $litRva = $null
        $sm = [regex]::Match($before, '"([^"]*)"\s*,\s*(-?(?:0x[0-9A-Fa-f]+|\d+))\s*,\s*$')
        if ($sm.Success -and (Test-PatternText ($sm.Groups[1].Value -replace '\s+', ' ').Trim())) {
            $siteBase = ($sm.Groups[1].Value -replace '\s+', ' ').Trim()
            $o = $sm.Groups[2].Value
            $siteOff = if ($o -match '0x') { [Convert]::ToInt32(($o -replace '-?0x', ''), 16) * $(if ($o.StartsWith('-')) { -1 } else { 1 }) } else { [int]$o }
        }
        $lm = [regex]::Match($before, '\b(k\w+Rva)\s*,\s*$')
        if ($lm.Success) { $litRva = $lm.Groups[1].Value }
        [pscustomobject]@{ File = $file; Line = $line; Const = $const; Name = $name; Pattern = $pat; SiteBase = $siteBase; SiteOffset = $siteOff; LitRva = $litRva }
    }
}

# Argumentos de nivel superior de la llamada que contiene la posicion idx.
function Get-EnclosingCall([string]$text, [int]$idx) {
    $depth = 0; $open = -1
    for ($i = $idx - 1; $i -ge 0; $i--) {
        $c = $text[$i]
        if ($c -eq ')') { $depth++ }
        elseif ($c -eq '(') { if ($depth -eq 0) { $open = $i; break } else { $depth-- } }
        elseif ($c -eq ';' -or $c -eq '{' -or $c -eq '}') { if ($depth -eq 0) { return $null } }
    }
    if ($open -lt 0) { return $null }
    $fm = [regex]::Match($text.Substring([Math]::Max(0, $open - 80), [Math]::Min(80, $open)), '([\w:]+)\s*(<[^()]*>)?\s*$')
    $fn = if ($fm.Success) { $fm.Groups[1].Value } else { '' }
    $args = @(); $depth = 0; $start = $open + 1
    for ($i = $open + 1; $i -lt $text.Length; $i++) {
        $c = $text[$i]
        if ($c -eq '(' -or $c -eq '{' -or $c -eq '[') { $depth++ }
        elseif ($c -eq ')' -or $c -eq '}' -or $c -eq ']') {
            if ($depth -eq 0) { $args += $text.Substring($start, $i - $start).Trim(); break }
            $depth--
        }
        elseif ($c -eq ',' -and $depth -eq 0) { $args += $text.Substring($start, $i - $start).Trim(); $start = $i + 1 }
    }
    [pscustomobject]@{ Fn = $fn; Args = $args }
}

Write-Host "Cargando $Exe ..."
$pe = [PeImage]::Load($Exe)
Write-Host ("TimeDateStamp 0x{0:X8}  ImageBase 0x{1:X}  secciones: {2}" -f $pe.TimeDateStamp, $pe.ImageBase, (($pe.Sections | ForEach-Object { $_.Name }) -join ' '))

$files = foreach ($d in $SourceDirs) {
    if (Test-Path $d) { Get-ChildItem -Path $d -Recurse -Include *.cpp, *.hpp, *.h -File }
}
$texts = @{}
foreach ($f in $files) { $texts[$f.FullName] = [IO.File]::ReadAllText($f.FullName) }

# --- Constantes kXxxRva y campos de perfil ---
$rvaConst = @{}; $sizeConst = @{}
foreach ($t in $texts.Values) {
    foreach ($m in [regex]::Matches($t, 'constexpr\s+std::uintptr_t\s+(k\w+)\s*=\s*(0x[0-9A-Fa-f]+)')) {
        $rvaConst[$m.Groups[1].Value] = [Convert]::ToUInt64($m.Groups[2].Value.Substring(2), 16)
    }
    foreach ($m in [regex]::Matches($t, 'constexpr\s+std::size_t\s+(k\w+Offset)\s*=\s*(0x[0-9A-Fa-f]+|\d+)')) {
        $sizeConst[$m.Groups[1].Value] = [int]$m.Groups[2].Value
    }
}
$profField = @{}; $profRows = @()
$profileFile = $files | Where-Object { $_.Name -eq 'game_build_profiles.hpp' } | Select-Object -First 1
if ($profileFile) {
    $ptext = $texts[$profileFile.FullName]
    foreach ($sm in [regex]::Matches($ptext, '(?s)struct\s+(\w+AddressProfile)\s*\{(.*?)\n\};')) {
        $sname = $sm.Groups[1].Value
        $fields = @([regex]::Matches($sm.Groups[2].Value, 'std::uintptr_t\s+(\w+)\{\}') | ForEach-Object { $_.Groups[1].Value })
        $arr = [regex]::Match($ptext, "(?s)std::array<$sname,\s*\d+>\s*\w+\{\{(.*?)\}\};")
        if (-not $arr.Success) { continue }
        $blk = [regex]::Match($arr.Groups[1].Value, "EldenRingBuild::$Build,([^}]*)\}")
        if (-not $blk.Success) { continue }
        $vals = @([regex]::Matches($blk.Groups[1].Value, '0x[0-9A-Fa-f]+') | ForEach-Object { [Convert]::ToUInt64($_.Value.Substring(2), 16) })
        for ($i = 0; $i -lt $fields.Count -and $i -lt $vals.Count; $i++) {
            $profField[$fields[$i]] = $vals[$i]
            $profRows += [pscustomobject]@{ Perfil = ($sname -replace 'AddressProfile$', ''); Campo = $fields[$i]; Rva = $vals[$i] }
        }
    }
}

function Resolve-RvaExpr([string]$expr, [hashtable]$vars, [int]$depth = 0) {
    if (-not $expr -or $depth -gt 3) { return $null }
    $e = $expr.Trim()
    if ($rvaConst.ContainsKey($e)) { return $rvaConst[$e] }
    if ($e -match '->\s*(\w+_rva)\s*$' -and $profField.ContainsKey($Matches[1])) { return $profField[$Matches[1]] }
    if ($e -match '^0x[0-9A-Fa-f]+$') { return [Convert]::ToUInt64($e.Substring(2), 16) }
    if ($vars.ContainsKey($e)) { return (Resolve-RvaExpr $vars[$e] $vars ($depth + 1)) }
    return $null
}

$patterns = foreach ($f in $files) { Get-Patterns $f.FullName $texts[$f.FullName] }
$patByConst = @{}
foreach ($p in $patterns) { if ($p.Const) { $patByConst[$p.Const] = $p.Pattern } }

# --- Usos de cada constante de patron: (funcion, rva esperada, validacion, offset) ---
$uses = @{}
foreach ($f in $files) {
    $t = $texts[$f.FullName]
    $vars = @{}
    foreach ($m in [regex]::Matches($t, '(\w+)\s*=[^;=]*?game\.at_rva\(\s*([^()]+?)\s*\)')) { $vars[$m.Groups[1].Value] = $m.Groups[2].Value }
    foreach ($c in @($patByConst.Keys)) {
        foreach ($m in [regex]::Matches($t, "\b$c\b")) {
            $call = Get-EnclosingCall $t $m.Index
            if (-not $call) { continue }
            $pi = [Array]::IndexOf([string[]]$call.Args, $c)
            if ($pi -lt 0) { continue }
            $rva = $null; $ri = -1
            for ($j = 0; $j -lt $call.Args.Count; $j++) {
                if ($j -eq $pi) { continue }
                $v = Resolve-RvaExpr $call.Args[$j] $vars
                if ($null -ne $v) { $rva = $v; $ri = $j; break }
            }
            if ($null -eq $rva) { continue }
            $fn = ($call.Fn -split '::|\.')[-1]
            $isCall = $fn -match 'call_target'
            $offset = 0; $validation = $null; $scanned = $null
            $isResolver = $fn -match '^resolve_(optional_)?(direct|call_target)$'
            if ($isResolver) {
                # resolve_*(game, name, scan_pattern, [call_offset,] fallback_rva, [validation])
                $scanned = $call.Args[2]
                if ($isCall -and $call.Args.Count -gt 3) {
                    $oa = $call.Args[3]
                    if ($oa -match '^\d+$') { $offset = [int]$oa } elseif ($sizeConst.ContainsKey($oa)) { $offset = $sizeConst[$oa] }
                }
                $vi = $ri + 1
                if ($vi -lt $call.Args.Count -and $patByConst.ContainsKey($call.Args[$vi])) { $validation = $patByConst[$call.Args[$vi]] }
            } else { $validation = $patByConst[$c] }
            # Rol: Scan = es el patron que se busca; Valida = solo valida el respaldo.
            $role = if ($isResolver -and $scanned -ne $c) { 'Valida' } elseif ($isResolver) { 'Scan' } else { 'Rva' }
            $scanPat = if ($scanned -and $patByConst.ContainsKey($scanned)) { $patByConst[$scanned] } else { $null }
            if (-not $uses.ContainsKey($c)) { $uses[$c] = @() }
            if (-not ($uses[$c] | Where-Object { $_.Fn -eq $fn -and $_.Rva -eq $rva -and $_.Role -eq $role })) {
                $uses[$c] += [pscustomobject]@{ Fn = $fn; Rva = $rva; Offset = $offset; IsCall = $isCall; Validation = $validation; Role = $role; ScanPattern = $scanPat }
            }
        }
    }
}

# --- Evaluacion ---
$groups = $patterns | Group-Object Pattern
$results = foreach ($g in $groups) {
    $hits = New-Object 'System.Collections.Generic.List[uint32]'
    $cnt = $pe.Scan($g.Name, $true, $MaxListed, $hits)
    $consts = @($g.Group | Where-Object { $_.Const } | ForEach-Object { $_.Const } | Select-Object -Unique)
    $useList = @(foreach ($c in $consts) { if ($uses.ContainsKey($c)) { $uses[$c] } })
    $states = @(); $notes = @()
    foreach ($u in $useList) {
        $scans = $u.Role -eq 'Scan'
        if ($u.Role -eq 'Valida') {
            # Solo valida el respaldo de otro patron; importa si ese otro es unico.
            $h2 = New-Object 'System.Collections.Generic.List[uint32]'
            $c2 = if ($u.ScanPattern) { $pe.Scan($u.ScanPattern, $true, 1, $h2) } else { 0 }
            $ok = $pe.MatchesAt($u.Rva, $u.Validation)
            if ($ok) { $states += 'RESPALDO_OK' }
            elseif ($c2 -eq 1) {
                $real = if ($u.IsCall) { $pe.Rel32Target([uint64]$h2[0] + [uint64]$u.Offset) } else { [int64]$h2[0] }
                $states += 'RESPALDO_OBSOLETO'; $notes += ('respaldo 0x{0:X} no valida; el AOB da 0x{1:X}' -f $u.Rva, $real)
            }
            else { $states += 'ROTO'; $notes += ('AOB no unico y respaldo 0x{0:X} no valida' -f $u.Rva) }
            continue
        }
        if ($scans -and $cnt -eq 1) {
            $got = if ($u.IsCall) { $pe.Rel32Target([uint64]$hits[0] + [uint64]$u.Offset) } else { [int64]$hits[0] }
            if ($got -eq [int64]$u.Rva) { $states += 'OK_AOB' }
            elseif ($got -lt 0) { $states += 'ROTO'; $notes += 'callsite unico pero sin rel32 valido' }
            else { $states += 'RESPALDO_OBSOLETO'; $notes += ('AOB->0x{0:X}, respaldo 0x{1:X}' -f $got, $u.Rva) }
        } else {
            $ok = if ($u.Validation) { $pe.MatchesAt($u.Rva, $u.Validation) } else { ($s = $pe.SectionOf($u.Rva)) -and $s.Exec }
            if ($ok) { $states += $(if ($scans) { 'RESPALDO_OK' } else { 'OK_RVA' }) }
            else { $states += 'ROTO'; $notes += ('{0} no valida en 0x{1:X}' -f $u.Fn, $u.Rva) }
        }
    }
    foreach ($p in $g.Group) {
        if ($p.SiteBase) {
            $h3 = New-Object 'System.Collections.Generic.List[uint32]'
            $c3 = $pe.Scan($p.SiteBase, $true, 1, $h3)
            if ($c3 -ne 1) { $states += 'ROTO'; $notes += ('aob del sitio con {0} coincidencias' -f $c3) }
            elseif ($pe.MatchesAt([uint64]([int64]$h3[0] + $p.SiteOffset), $g.Name)) { $states += 'OK_RVA'; $notes += ('verificado en sitio 0x{0:X}+0x{1:X}' -f $h3[0], $p.SiteOffset) }
            else { $states += 'ROTO'; $notes += ('expected no coincide en 0x{0:X}+0x{1:X}' -f $h3[0], $p.SiteOffset) }
        } elseif ($p.LitRva -and $rvaConst.ContainsKey($p.LitRva)) {
            $r = $rvaConst[$p.LitRva]
            if ($pe.MatchesAt($r, $g.Name)) { $states += 'RESPALDO_OK' }
            else { $states += 'RESPALDO_OBSOLETO'; $notes += ('valida el respaldo {0}=0x{1:X}, que ya no coincide' -f $p.LitRva, $r) }
        } elseif ($p.Const -and -not $useList.Count -and ([regex]::Matches($texts[$p.File], "matches\(\s*\w+\s*,\s*$($p.Const)\s*\)").Count)) {
            $states += 'SOLO_VALIDACION'; $notes += 'se valida en una direccion calculada en tiempo de ejecucion'
        }
    }
    if (-not $states.Count) {
        $states += $(if ($cnt -eq 1) { 'UNICO_SIN_RVA' } elseif ($cnt -gt 1) { 'AMBIGUO_SIN_RVA' } else { 'FALLA' })
    }
    $rank = @{ ROTO = 0; FALLA = 1; AMBIGUO_SIN_RVA = 2; RESPALDO_OBSOLETO = 3; RESPALDO_OK = 4; OK_RVA = 5; OK_AOB = 6; UNICO_SIN_RVA = 7; SOLO_VALIDACION = 8 }
    $worst = $states | Sort-Object { $rank[$_] } | Select-Object -First 1
    [pscustomobject]@{
        Estado   = $worst
        Rank     = $rank[$worst]
        Matches  = $cnt
        RVAs     = (($hits | ForEach-Object { '0x{0:X}' -f $_ }) -join ',')
        Esperada = (($useList | ForEach-Object { '0x{0:X}' -f $_.Rva } | Select-Object -Unique) -join ',')
        Uso      = (($useList | ForEach-Object { $_.Fn } | Select-Object -Unique) -join ',')
        Nombre   = (($g.Group | ForEach-Object { $_.Name } | Select-Object -Unique) -join '/')
        Origen   = (($g.Group | ForEach-Object { '{0}:{1}' -f (Split-Path $_.File -Leaf), $_.Line } | Select-Object -Unique) -join ' ')
        Arbol    = (($g.Group | ForEach-Object { if ($_.File -like '*ERNativeUI-main*') { 'fork' } else { 'hub' } } | Select-Object -Unique) -join '+')
        Notas    = ($notes -join '; ')
        Patron   = $g.Name
    }
}
$results = $results | Sort-Object Rank, Origen

Write-Host ''
Write-Host '=== Patrones AOB ==='
Write-Host 'OK_AOB=unico y coincide con el respaldo | OK_RVA=solo se valida en RVA fija y valida | RESPALDO_OK=AOB no unico, respaldo validado'
Write-Host 'RESPALDO_OBSOLETO=funciona por AOB pero la RVA de respaldo ya no valida (aviso) | ROTO=no resuelve | FALLA=sin coincidencias | *_SIN_RVA=sin respaldo conocido | SOLO_VALIDACION=se comprueba en direccion calculada'
$results | Format-Table Estado, Matches, Esperada, RVAs, Uso, Nombre, Origen, Notas -AutoSize -Wrap | Out-String -Width 260 | Write-Host
$results | Group-Object Estado | Sort-Object { ($_.Group[0]).Rank } | ForEach-Object { Write-Host ("{0,-16} {1}" -f $_.Name, $_.Count) }

# --- RVAs fijas de los perfiles ---
$profResults = foreach ($r in $profRows) {
    $rva = $r.Rva; $f = $r.Campo
    $sec = $pe.SectionOf($rva)
    $bytes = if ($rva + 8 -le $pe.Image.Length) { (($pe.Image[$rva..($rva + 7)] | ForEach-Object { '{0:X2}' -f $_ }) -join ' ') } else { '' }
    $wantCode = $f -notmatch '(vtable|movie_name|slot)_rva$'
    $prev = if ($rva -gt 0 -and $rva -lt $pe.Image.Length) { $pe.Image[$rva - 1] } else { 0 }
    $validated = @($results | Where-Object { $_.Esperada -match ('\b0x{0:X}\b' -f $rva) -and $_.Rank -ge 4 }).Count -gt 0
    $estado = if (-not $sec) { 'FUERA' }
              elseif ($wantCode -and -not $sec.Exec) { 'NO_EJEC' }
              elseif (-not $wantCode -and $sec.Exec) { 'DATO_EN_CODIGO' }
              elseif ($validated) { 'OK_PATRON' }
              elseif ($wantCode -and $bytes -match '^(00 00 00 00|CC CC CC CC)') { 'SOSPECHOSO' }
              elseif ($wantCode -and $prev -notin 0xCC, 0xC3, 0x90) { 'REVISAR' }
              else { 'OK' }
    [pscustomobject]@{ Estado = $estado; Perfil = $r.Perfil; Campo = $f; RVA = ('0x{0:X}' -f $rva); Seccion = $(if ($sec) { $sec.Name } else { '-' }); Bytes = $bytes }
}
if ($profResults) {
    Write-Host ''
    Write-Host "=== RVAs de perfiles ($Build) ==="
    Write-Host 'OK_PATRON=validada por un patron | REVISAR=byte anterior no es CC/C3/90 (puede no ser inicio de funcion)'
    $profResults | Format-Table -AutoSize | Out-String -Width 220 | Write-Host
}

if ($Csv) {
    $results | Select-Object -ExcludeProperty Rank | Export-Csv -NoTypeInformation -Encoding UTF8 $Csv
    if ($profResults) { $profResults | Export-Csv -NoTypeInformation -Encoding UTF8 ([IO.Path]::ChangeExtension($Csv, '.profiles.csv')) }
    Write-Host "CSV: $Csv"
}

$bad = @($results | Where-Object { $_.Rank -le 2 }).Count + @($profResults | Where-Object { $_.Estado -in 'FUERA', 'NO_EJEC', 'DATO_EN_CODIGO', 'SOSPECHOSO' }).Count
exit $(if ($bad) { 1 } else { 0 })
