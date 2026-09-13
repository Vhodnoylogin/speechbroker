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
'  {0,-40} {1} files' -f $d.modName, @(Get-ChildItem -LiteralPath $mod -Recurse -File).Count
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
