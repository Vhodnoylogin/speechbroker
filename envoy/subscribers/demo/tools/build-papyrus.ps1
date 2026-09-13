# Compiling the scripts of the test listener. Every path is in config/build.json.
#
# The declarations of the bridge (Envoy.psc) are taken from the installed mod of
# the bridge, the SDK folder: there is no copy of somebody else file in this
# module and there must not be one.
param([switch]$Quiet)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cfg  = Get-Content -LiteralPath (Join-Path $root 'config\build.json') -Raw | ConvertFrom-Json

function Expand-CfgPath([string]$p) {
    $p.Replace('{root}', $root).Replace('{creationKit}', $cfg.creationKit).Replace('{sdk}', $cfg.deploy.sdk) -replace '/', '\'
}

$compiler = Join-Path $cfg.creationKit 'Papyrus Compiler\PapyrusCompiler.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw "Compiler not found: $compiler. The Creation Kit is needed." }
if (-not (Test-Path -LiteralPath (Join-Path $cfg.deploy.sdk 'Envoy.psc'))) {
    throw "The declarations of the bridge were not found in $($cfg.deploy.sdk). Lay the bridge out first."
}

$src     = Expand-CfgPath $cfg.papyrusSource
$out     = Expand-CfgPath $cfg.papyrusOutput
$imports = ($cfg.imports | ForEach-Object { Expand-CfgPath $_ }) -join ';'
New-Item -ItemType Directory -Force $out | Out-Null

# The Papyrus compiler reads a source without a BOM as ANSI (cp1251 here) and
# silently ruins Cyrillic: the vocabularies of the subscribers stop matching the
# recognised text and the text on screen turns to rubbish. A BOM settles it.
$bom = [byte[]](0xEF, 0xBB, 0xBF)
foreach ($f in Get-ChildItem -LiteralPath $src -Filter *.psc -Recurse) {
    $raw = [IO.File]::ReadAllBytes($f.FullName)
    if ($raw.Length -ge 3 -and $raw[0] -eq $bom[0] -and $raw[1] -eq $bom[1] -and $raw[2] -eq $bom[2]) { continue }
    [IO.File]::WriteAllBytes($f.FullName, $bom + $raw)
    Write-Host "  BOM added: $($f.Name)"
}

$argv = @($src, "-f=$($cfg.papyrusFlags)", "-i=$imports", "-o=$out", '-all')
& $compiler @argv
if ($LASTEXITCODE -ne 0) { throw "Papyrus compilation ended with code $LASTEXITCODE" }

if (-not $Quiet) {
    Get-ChildItem $out -Filter *.pex | ForEach-Object { '  {0,-22} {1,7:N0} b' -f $_.Name, $_.Length }
}
