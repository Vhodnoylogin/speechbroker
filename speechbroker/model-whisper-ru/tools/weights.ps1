# The weights, and whether they are the weights.
#
#   tools\weights.ps1                       verify every weights\<id>
#   tools\weights.ps1 -Id whisper-ru-small  verify one
#   tools\weights.ps1 -Write                write SHA256SUMS beside each set
#
# WHY THIS EXISTS AT ALL, and it is measured rather than feared: five hundred
# bytes flipped inside a converted model.bin produce NO error of any kind - not
# at load, not at inference - and a model that answers rubbish. Nothing above the
# shim can tell that apart from a bad recording or a hard accent, because an
# answer is text and a score and rubbish has both. So the check has to happen
# before the child is ever raised, or it does not exist.
#
# The shim does the same check itself at Start, against the same file, in
# src/core/Sha256.cpp. This script is the convenience and the way to CREATE the
# file; it is not the only way to read it. The format is the one sha256sum has
# written since 1999 - "<64 hex>  <relative path>", two spaces - so any tool a
# person already has can verify the same file.
#
# Every path comes out of config\build.json. Nothing here is true only on one
# machine, and nothing here moves a weight file: they are gigabytes, and copying
# them about is the integrator's business and not a script's.
param(
    [string]$Id,
    [switch]$Write
)
$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$cfg  = Get-Content -LiteralPath (Join-Path $root 'config\build.json') -Raw | ConvertFrom-Json
$d    = $cfg.deploy
function Expand-Path([string]$p) { $p.Replace('{root}', $root) }

$weightsRoot = Expand-Path $d.weights
if (-not (Test-Path -LiteralPath $weightsRoot)) {
    throw "there is no weights folder: $weightsRoot. It is where weights\<id>\ goes; the module ships none, and the README says where they come from."
}

$sets = @(Get-ChildItem -LiteralPath $weightsRoot -Directory)
if ($Id) { $sets = @($sets | Where-Object { $_.Name -eq $Id }) }
if (-not $sets) { throw "no set of weights found in $weightsRoot$(if ($Id) { " under the name $Id" })" }

# UTF-8 without a byte order mark: sha256sum and every other tool reads this
# file as plain bytes, and a BOM would become part of the first hash.
$enc = New-Object Text.UTF8Encoding($false)
$bad = 0

foreach ($set in $sets) {
    $sumsFile = Join-Path $set.FullName 'SHA256SUMS'

    if ($Write) {
        # Files are listed in a stable order so that two runs of this script on
        # the same folder produce the same file and git shows no diff where
        # nothing changed.
        $lines = @()
        foreach ($file in (Get-ChildItem -LiteralPath $set.FullName -Recurse -File | Sort-Object FullName)) {
            if ($file.Name -eq 'SHA256SUMS') { continue }
            $rel = $file.FullName.Substring($set.FullName.Length).TrimStart('\') -replace '\\', '/'
            $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            $lines += "$hash  $rel"
        }
        if (-not $lines) { Write-Warning "  $($set.Name): the folder is empty - nothing to sign"; continue }
        [IO.File]::WriteAllLines($sumsFile, $lines, $enc)
        '  {0,-24} written, {1} files' -f $set.Name, $lines.Count
        continue
    }

    if (-not (Test-Path -LiteralPath $sumsFile)) {
        # Not a crash: an author building a model mod of their own meets this
        # before they have generated anything. It IS what the shim refuses to
        # start on, unless requireSums is turned off in the settings.
        Write-Warning "  $($set.Name): no SHA256SUMS - the weights are NOT verified. Run this script with -Write, or set requireSums to false while developing."
        $bad++
        continue
    }

    $checked = 0
    $failed = @()
    foreach ($line in (Get-Content -LiteralPath $sumsFile -Encoding UTF8)) {
        if (-not $line -or $line.StartsWith('#')) { continue }
        # "<64 hex>  <path>", and a star instead of the second space is
        # sha256sum's binary mode. Both are accepted: a person pastes in
        # whatever their own tool produced.
        if ($line -notmatch '^([0-9a-fA-F]{64})[ *]+(.+)$') { continue }
        $expected = $Matches[1].ToLowerInvariant()
        $rel = $Matches[2]
        if ($rel -match '\.\.' -or $rel -match '^[/\\]' -or $rel -match ':') {
            $failed += "$rel  leaves the folder"
            continue
        }
        $path = Join-Path $set.FullName $rel
        if (-not (Test-Path -LiteralPath $path)) { $failed += "$rel  missing"; continue }
        $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actual -ne $expected) { $failed += "$rel  $actual"; continue }
        $checked++
    }

    if ($failed.Count) {
        $bad++
        '  {0,-24} BAD - {1} verified, {2} wrong' -f $set.Name, $checked, $failed.Count
        foreach ($f in $failed) { "      $f" }
    } else {
        '  {0,-24} ok, {1} files' -f $set.Name, $checked
    }
}

''
if ($bad) { throw "$bad set(s) of weights did not verify - the shim will refuse to start those models" }
