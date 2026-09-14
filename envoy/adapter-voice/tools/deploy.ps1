# Laying the Envoy test adapter out into mods\. Every path and name is in
# config/build.json.
#
#   tools\deploy.ps1            show what would be done
#   tools\deploy.ps1 -Apply     do it
#
# The adapter is a module of its own: the sources of the bridge are not here and
# it does not see them. The one thing they share is the contract, and it is
# taken from the installed mod of the bridge (SDK).
param([switch]$Apply, [switch]$NoIndex)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cfg  = Get-Content -LiteralPath (Join-Path $root 'config\build.json') -Raw | ConvertFrom-Json
$d    = $cfg.deploy
$enc  = New-Object Text.UTF8Encoding($false)

$game = @(Get-Process -Name SkyrimVR,SkyrimSE -ErrorAction SilentlyContinue)
if ($game.Count) { throw "The game is running ($($game.Name -join ', ')) - laying out is not allowed" }

if (-not (Test-Path -LiteralPath (Join-Path $d.sdk 'envoy-adapter.h'))) {
    throw "The contract of the bridge was not found in $($d.sdk). Lay the bridge out first."
}

function Expand-Path([string]$p) { $p.Replace('{root}', $root) }

$dist = Join-Path $root 'dist'
Remove-Item -LiteralPath $dist -Recurse -Force -ErrorAction SilentlyContinue
$mod = Join-Path $dist $d.modName
New-Item -ItemType Directory -Force (Join-Path $mod $d.settingsTargetRel) | Out-Null
Copy-Item -LiteralPath (Expand-Path $d.settings) -Destination (Join-Path $mod $d.settingsTargetRel) -Force

# The library itself. A mod without it is a mod that does nothing, so a missing
# build is a refusal rather than a warning: laying out quietly would leave the
# previous library in mods\ and the run would be testing last week.
$dll = Expand-Path $d.dll
if (-not (Test-Path -LiteralPath $dll)) {
    throw "the library of the adapter is not built: $dll"
}
New-Item -ItemType Directory -Force (Join-Path $mod 'SKSE\Plugins') | Out-Null
Copy-Item -LiteralPath $dll -Destination (Join-Path $mod 'SKSE\Plugins') -Force

# The text the adapter puts out - which is to say its log; it has no window of its
# own. The tables are kept as UTF-8 in localization/ and turned here into the
# UTF-16LE files the engine reads, by the script the bridge publishes in its SDK.
# If the built-in baseline changes, the DLL next to it is older than the text and
# has to be rebuilt - we say so out loud rather than shipping the mismatch.
$loc = $d.localization
if ($loc) {
    $builder = Join-Path $d.sdkTools 'build-localization.py'
    if (-not (Test-Path -LiteralPath $builder)) {
        throw "The SDK of the bridge has no build-localization.py ($builder). Lay the bridge out first."
    }

    $strings = Expand-Path $loc.header
    $before = if (Test-Path -LiteralPath $strings) { (Get-FileHash -LiteralPath $strings).Hash } else { '' }

    $built = Join-Path $root 'build\localization'
    & python $builder --source (Expand-Path $loc.source) --out $built `
             --name $loc.name --base $loc.base --header $strings | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "building the translations failed with code $LASTEXITCODE" }

    if ((Get-FileHash -LiteralPath $strings).Hash -ne $before) {
        Write-Warning "the built-in strings changed - rebuild the plugin, the DLL is older than the text"
    }

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

# The contract of a model mod is for authors, not for the game: the game does
# not read it. So it rides as a SEPARATE package, like the SDK of the bridge,
# and not as a folder inside the mod. On the Nexus that is an optional file on
# the same page.
if ($d.publish) {
    $sdkMod = Join-Path $dist $d.sdkName
    New-Item -ItemType Directory -Force $sdkMod | Out-Null
    foreach ($f in $d.publish) { Copy-Item -LiteralPath (Expand-Path $f) -Destination $sdkMod -Force }
    if ($d.docs) {
        foreach ($f in $d.docs) { Copy-Item -LiteralPath (Expand-Path $f) -Destination $sdkMod -Force }
    }
    $sdkMeta = @(
        '[General]'
        'gameName=SkyrimSE'
        'modid=0'
        "version=$($d.version)"
        "newestVersion=$($d.version)"
        'category="0,"'
        'installationFile='
        'notes=The contract of a model mod for EnvoyVoiceAdapter. An author of a mod needs it, the game does not; keep it disabled in the profiles.'
        ''
        '[installedFiles]'
        'size=0'
    )
    [IO.File]::WriteAllLines((Join-Path $sdkMod 'meta.ini'), $sdkMeta, $enc)
}

# The licence and the list of what was borrowed ride in every mod. Whoever
# unpacked the archive has to find them inside it: they will not open the page
# they downloaded from again, and the terms of six other projects require the
# text to be in the package.
if ($d.docs) {
    foreach ($f in $d.docs) { Copy-Item -LiteralPath (Expand-Path $f) -Destination $mod -Force }
}

$meta = @(
    '[General]'
    'gameName=SkyrimSE'
    'modid=0'
    "version=$($d.version)"
    "newestVersion=$($d.version)"
    'category="0,"'
    'installationFile='
    "notes=The Envoy test adapter to the service of speech recognition and synthesis. Needs the mod $($d.bridgeMod)."
    ''
    '[installedFiles]'
    'size=0'
)
[IO.File]::WriteAllLines((Join-Path $mod 'meta.ini'), $meta, $enc)

'--- to be laid out ---'
foreach ($pack in Get-ChildItem -LiteralPath $dist -Directory) {
    '  {0,-44} {1} files' -f $pack.Name, @(Get-ChildItem -LiteralPath $pack.FullName -Recurse -File).Count
}
if (-not $Apply) { ''; 'dry run - add -Apply'; return }

# There are two packages - the mod and the contract for authors - and they are
# laid out the same way.
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
        $rel = $_.FullName.Substring($pack.FullName.Length).TrimStart('')
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
    & $d.indexScript -Owner $d.indexOwner -Mods $copied -Note "Envoy Voice Adapter deploy $($d.version)"
}
''
