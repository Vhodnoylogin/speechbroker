# Laying the Envoy bridge out into mods\. Every path and name is in
# config/build.json; none of them are in the code.
#
#   tools\deploy.ps1            show what would be done
#   tools\deploy.ps1 -Apply     do it
#
# The bridge lays out only itself. The adapter and the listener are modules of
# their own on their own branches, and the files of a neighbour are simply not
# here. The one thing they share is the contract, and the bridge hands it over as
# the SDK folder inside its own mod: both of our test modules build against it,
# and so does any other mod that wants to talk to Envoy.
#
# This script does NOT touch the make-up of the profiles: enabling a mod is done
# through the MO2 bridge.
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
New-Item -ItemType Directory -Force (Join-Path $mod 'Scripts') | Out-Null

# Only what was built goes into the mod. The sources of the scripts are not part
# of it: the game does not read them, and the player gets spare files with
# nothing to open them with. Their place is in the package for authors, below.
foreach ($s in $d.scripts) {
    Copy-Item -LiteralPath (Join-Path $pex "$s.pex") -Destination (Join-Path $mod 'Scripts') -Force
}
Copy-Item -LiteralPath (Join-Path $root "esp\$($d.esp)") -Destination $mod -Force

$dll = Expand-Path $d.dll
if (Test-Path -LiteralPath $dll) {
    New-Item -ItemType Directory -Force (Join-Path $mod 'SKSE\Plugins\envoy') | Out-Null
    Copy-Item -LiteralPath $dll -Destination (Join-Path $mod 'SKSE\Plugins') -Force
    Copy-Item -LiteralPath (Expand-Path $d.defaultConfig) `
              -Destination (Join-Path $mod 'SKSE\Plugins\envoy\envoy.json') -Force
} else {
    Write-Warning "the library of the bridge is not built: $dll"
}

# Text for the player. The tables are kept as UTF-8 in localization/ and turned
# here into what the game reads: Interface\Translations\Envoy_<language>.txt,
# UTF-16LE. The same generator writes the baseline compiled into the plugin, so
# if the header changes here, the DLL next to it is older than the strings and
# has to be rebuilt - we say so out loud rather than shipping the mismatch.
$loc = $d.localization
if ($loc) {
    $strings = Join-Path $root 'src\core\LocStrings.h'
    $before = if (Test-Path -LiteralPath $strings) { (Get-FileHash -LiteralPath $strings).Hash } else { '' }

    $built = Join-Path $root 'build\localization'
    & python (Join-Path $PSScriptRoot 'build-localization.py') `
             --source (Expand-Path $loc.source) --out $built --name $loc.name --base $loc.base | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "building the translations failed with code $LASTEXITCODE" }

    $after = (Get-FileHash -LiteralPath $strings).Hash
    if ($before -ne $after) {
        Write-Warning "the built-in strings changed - rebuild the plugin, the DLL is older than the text"
    }

    # The language of the source goes into the mod itself: it is not optional, it
    # is what the plugin is written in. Every other language is a mod of its own,
    # so that adding a language means adding a mod and nothing else.
    $into = Join-Path $mod 'Interface\Translations'
    New-Item -ItemType Directory -Force $into | Out-Null
    Copy-Item -LiteralPath (Join-Path $built "$($loc.name)_$($loc.base).txt") -Destination $into -Force

    foreach ($lang in $loc.languageMods.PSObject.Properties) {
        $file = Join-Path $built "$($loc.name)_$($lang.Name).txt"
        if (-not (Test-Path -LiteralPath $file)) {
            Write-Warning "no table for $($lang.Name) - the mod $($lang.Value) will not be built"
            continue
        }
        $langMod = Join-Path $dist $lang.Value
        $langInto = Join-Path $langMod 'Interface\Translations'
        New-Item -ItemType Directory -Force $langInto | Out-Null
        Copy-Item -LiteralPath $file -Destination $langInto -Force

        $langMeta = @(
            '[General]'
            'gameName=SkyrimSE'
            'modid=0'
            "version=$($d.version)"
            "newestVersion=$($d.version)"
            'category="0,"'
            'installationFile='
            "notes=Envoy Framework - $($lang.Name) text. One folder of translations; put it below the bridge."
            ''
            '[installedFiles]'
            'size=0'
        )
        [IO.File]::WriteAllLines((Join-Path $langMod 'meta.ini'), $langMeta, $enc)
    }
}

# The package for mod authors is SEPARATE, not a folder inside the mod. The C++
# headers, the Papyrus declarations and the descriptions of the contract are not
# read by the game at all: a player has no use for them, and an author needs them
# without the game. On the Nexus these are different files on one page: the main
# one and the optional "modding source".
#
# It is still built by the lay-out script and still put into mods\: the adapter
# and the subscriber build against it - exactly as somebody else will, having
# unpacked the optional file. Keep this mod disabled in the profiles: there is
# nothing in it for the game.
$sdkMod = Join-Path $dist $d.sdkName
foreach ($part in $d.sdkLayout.PSObject.Properties) {
    $into = Join-Path $sdkMod $part.Name
    New-Item -ItemType Directory -Force $into | Out-Null
    foreach ($f in $part.Value) { Copy-Item -LiteralPath (Expand-Path $f) -Destination $into -Force }
}
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
    'notes=Envoy Framework - the package for mod authors: the contract, the Papyrus declarations, the script sources and the string tables. The game does not need it; keep it disabled in the profiles.'
    ''
    '[installedFiles]'
    'size=0'
)
[IO.File]::WriteAllLines((Join-Path $sdkMod 'meta.ini'), $sdkMeta, $enc)

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
    'notes=Envoy Framework - the bridge. The contract for adapters and listeners is in the SDK package.'
    ''
    '[installedFiles]'
    'size=0'
)
[IO.File]::WriteAllLines((Join-Path $mod 'meta.ini'), $meta, $enc)

'--- to be laid out ---'
foreach ($pack in Get-ChildItem -LiteralPath $dist -Directory) {
    '  {0,-40} {1} files' -f $pack.Name, @(Get-ChildItem -LiteralPath $pack.FullName -Recurse -File).Count
}
if (-not $Apply) { ''; 'dry run - add -Apply'; return }

# There is more than one package - the mod, the SDK and a translation per
# language - and they are all laid out the same way. Only the mod used to be
# copied, and the folders built next to it never reached the build.
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
    & $d.indexScript -Owner $d.indexOwner -Mods $copied -Note "Envoy Framework deploy $($d.version)"
}
''
