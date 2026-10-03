# Laying the SpeechBroker test listener out into mods\. Every path and name is in
# config/build.json.
#
#   tools\deploy.ps1            show what would be done
#   tools\deploy.ps1 -Apply     do it
#
# The listener is a module of its own: neither the sources of the bridge nor
# those of the adapter are here. The declarations of the bridge are taken from
# the installed mod of the bridge (SDK) at compile time, and so is the script
# that builds the string tables - a subscriber needs it for exactly the same
# reason any other mod does.
param([switch]$Apply, [switch]$NoIndex)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cfg  = Get-Content -LiteralPath (Join-Path $root 'config\build.json') -Raw | ConvertFrom-Json
$d    = $cfg.deploy
$enc  = New-Object Text.UTF8Encoding($false)

$game = @(Get-Process -Name SkyrimVR,SkyrimSE -ErrorAction SilentlyContinue)
if ($game.Count) { throw "The game is running ($($game.Name -join ', ')) - laying out is not allowed" }

function Expand-Path([string]$p) { $p.Replace('{root}', $root) }

$pex = Join-Path $root 'build\pex'
if (-not (Test-Path -LiteralPath $pex)) { throw "No compiled scripts. Run tools\build-papyrus.ps1 first" }

$dist = Join-Path $root 'dist'
Remove-Item -LiteralPath $dist -Recurse -Force -ErrorAction SilentlyContinue
$mod = Join-Path $dist $d.modName
New-Item -ItemType Directory -Force (Join-Path $mod 'Scripts\Source') | Out-Null

foreach ($s in $d.scripts) {
    Copy-Item -LiteralPath (Join-Path $pex "$s.pex")          -Destination (Join-Path $mod 'Scripts') -Force
    Copy-Item -LiteralPath (Join-Path $root "papyrus\$s.psc") -Destination (Join-Path $mod 'Scripts\Source') -Force
}
Copy-Item -LiteralPath (Join-Path $root "esp\$($d.esp)") -Destination $mod -Force

# Text for the player, and the vocabulary with it: the phrases a subscriber
# registers have to be in the language the player speaks. The tables are kept as
# UTF-8 in localization/ and turned here into the UTF-16LE files the game and
# SpeechBroker.Translate both read.
$loc = $d.localization
if ($loc) {
    $builder = Join-Path $d.sdkTools 'build-localization.py'
    if (-not (Test-Path -LiteralPath $builder)) {
        throw "The SDK of the bridge has no build-localization.py ($builder). Lay the bridge out first."
    }

    $built = Join-Path $root 'build\localization'
    & python $builder --source (Expand-Path $loc.source) --out $built `
             --name $loc.name --base $loc.base --no-header | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "building the translations failed with code $LASTEXITCODE" }

    # Every language goes inside the mod, the source one and the translations
    # alike: a translation belongs to the module and is not a mod to install
    # beside it. The engine reads the file that matches the language of the game.
    $into = Join-Path $mod 'Interface\Translations'
    New-Item -ItemType Directory -Force $into | Out-Null
    $tables = @(Get-ChildItem -LiteralPath $built -Filter "$($loc.name)_*.txt" -File)
    if (-not $tables) { throw "no tables were built in $built" }
    foreach ($table in $tables) {
        Copy-Item -LiteralPath $table.FullName -Destination $into -Force
    }
    $baseTable = Join-Path $into "$($loc.name)_$($loc.base).txt"
    if (-not (Test-Path -LiteralPath $baseTable)) {
        throw "the table of the source language $($loc.base) is missing - the mod would show bare keys"
    }
}

$meta = @(
    '[General]'
    'gameName=SkyrimSE'
    'modid=0'
    "version=$($d.version)"
    "newestVersion=$($d.version)"
    'category="0,"'
    'installationFile='
    "notes=The SpeechBroker test subscribers: the observer, the greedy one and the sharing one. Needs the mod $($d.bridgeMod). Not wanted in the working profiles."
    ''
    '[installedFiles]'
    'size=0'
)
[IO.File]::WriteAllLines((Join-Path $mod 'meta.ini'), $meta, $enc)

'--- to be laid out ---'
foreach ($pack in Get-ChildItem -LiteralPath $dist -Directory) {
    '  {0,-46} {1} files' -f $pack.Name, @(Get-ChildItem -LiteralPath $pack.FullName -Recurse -File).Count
}
if (-not $Apply) { ''; 'dry run - add -Apply'; return }

$copied = @()
foreach ($pack in Get-ChildItem -LiteralPath $dist -Directory) {
    $target = Join-Path $d.modsRoot $pack.Name
    New-Item -ItemType Directory -Force $target | Out-Null
    # Copy-Item -Recurse -Force over a tree that already exists silently fails to
    # overwrite files in nested folders, and the lay-out reported success while
    # leaving the library of the previous build in the build. We copy file by file
    # and say what changed.
    $added = 0; $updated = 0; $same = 0
    Get-ChildItem -LiteralPath $pack.FullName -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($pack.FullName.Length).TrimStart('\')
        $dst = Join-Path $target $rel
        $dir = Split-Path -Parent $dst
        if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force $dir | Out-Null }
        if (-not (Test-Path -LiteralPath $dst)) {
            Copy-Item -LiteralPath $_.FullName -Destination $dst -Force
            $added++
        } elseif ((Get-FileHash -LiteralPath $_.FullName).Hash -ne (Get-FileHash -LiteralPath $dst).Hash) {
            Copy-Item -LiteralPath $_.FullName -Destination $dst -Force
            $updated++
        } else {
            $same++
        }
    }
    '  laid out: {0} - new {1}, updated {2}, unchanged {3}' -f $pack.Name, $added, $updated, $same
    $copied += $pack.Name
}

if (-not $NoIndex) {
    & $d.indexScript -Owner $d.indexOwner -Mods $copied -Note "SpeechBroker demo subscriber deploy $($d.version)"
}
''
