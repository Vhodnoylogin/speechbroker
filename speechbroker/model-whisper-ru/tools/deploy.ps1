# Laying a model mod out into mods\. Every path and name is in config/build.json.
#
#   tools\deploy.ps1            show what would be done
#   tools\deploy.ps1 -Apply     do it
#
# A model mod is a module of its own and NOT a program: there is no microphone
# in it, no port and no file to run. It carries a model - a listing and the
# files of the weights. The contract of the listing is in
# adapter-voice\contract\speechbroker-voice-model.md.
#
# The weights of this model live in another module on this machine (the voice
# branch), and there is no point copying gigabytes into the build: the lay-out
# puts junctions on them according to config\build.local.json, which is not in
# git. For somebody who downloaded the mod the weights lie inside the mod and no
# junction is needed.
param([switch]$Apply, [switch]$NoIndex)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cfg  = Get-Content -LiteralPath (Join-Path $root 'config\build.json') -Raw | ConvertFrom-Json
$d    = $cfg.deploy
$enc  = New-Object Text.UTF8Encoding($false)

$game = @(Get-Process -Name SkyrimVR,SkyrimSE -ErrorAction SilentlyContinue)
if ($game.Count) { throw "The game is running ($($game.Name -join ', ')) - laying out is not allowed" }

function Expand-Path([string]$p) { $p.Replace('{root}', $root) }

$adapter = Join-Path $d.modsRoot $d.adapterMod
if (-not (Test-Path -LiteralPath $adapter)) {
    Write-Warning "the adapter '$($d.adapterMod)' was not found in the build - the listings can be put down, but there will be nobody to read them"
}

$listings = Join-Path $root 'models'
if (-not (Test-Path -LiteralPath $listings)) { throw "the folder of listings was not found: $listings" }

$dist = Join-Path $root 'dist'
Remove-Item -LiteralPath $dist -Recurse -Force -ErrorAction SilentlyContinue
$mod = Join-Path $dist $d.modName
$rel = $d.targetRel -replace '/', '\'
$targetDir = Join-Path $mod $rel
New-Item -ItemType Directory -Force $targetDir | Out-Null

# The listings are put down as they are - there is nothing to correct in them
# and no reason to: by the very shape of the contract there are neither paths
# of another machine nor addresses in them.
Get-ChildItem -LiteralPath $listings -Filter '*.json' -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $targetDir -Force
}

# The licence and the list of what was borrowed ride inside the mod.
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
    "notes=Models of Russian speech recognition and synthesis for the SpeechBroker adapter. Needs the mod $($d.adapterMod)."
    ''
    '[installedFiles]'
    'size=0'
)
[IO.File]::WriteAllLines((Join-Path $mod 'meta.ini'), $meta, $enc)

'--- to be laid out ---'
'  {0,-46} {1} files' -f $d.modName, @(Get-ChildItem -LiteralPath $mod -Recurse -File).Count
if (-not $Apply) { ''; 'dry run - add -Apply'; return }

$target = Join-Path $d.modsRoot $d.modName
New-Item -ItemType Directory -Force $target | Out-Null
$added = 0; $updated = 0; $same = 0
Get-ChildItem -LiteralPath $mod -Recurse -File | ForEach-Object {
    $leaf = $_.FullName.Substring($mod.Length).TrimStart('\')
    $dst = Join-Path $target $leaf
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
'  laid out: {0} - new {1}, updated {2}, unchanged {3}' -f $d.modName, $added, $updated, $same

# The weights. On this machine, as a junction to the real place, so as not to
# copy gigabytes into the build. A junction is taken down through
# Directory::Delete: Remove-Item over a junction risks walking into the target
# and wiping the weights themselves.
$localPath = Join-Path $root 'config\build.local.json'
if (Test-Path -LiteralPath $localPath) {
    $local = Get-Content -LiteralPath $localPath -Raw | ConvertFrom-Json
    if ($local.weights) {
        foreach ($entry in $local.weights.PSObject.Properties) {
            $listing = Get-ChildItem -LiteralPath $listings -Filter '*.json' -File |
                Where-Object { (Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json).id -eq $entry.Name } |
                Select-Object -First 1
            if (-not $listing) {
                Write-Warning "there is no listing for the model '$($entry.Name)' - not putting a junction down"
                continue
            }
            $weights = (Get-Content -LiteralPath $listing.FullName -Raw | ConvertFrom-Json).weights
            $link = Join-Path (Join-Path $target $rel) ($weights -replace '/', '\')
            $parent = Split-Path -Parent $link
            if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Force $parent | Out-Null }
            if (Test-Path -LiteralPath $link) { [System.IO.Directory]::Delete($link, $false) }
            New-Item -ItemType Junction -Path $link -Target $entry.Value | Out-Null
            '  weights {0}: junction to {1}' -f $entry.Name, $entry.Value
        }
    }
} else {
    Write-Warning "there is no build.local.json - there will be no weights in the mod and the service will find no models. Make one for a run on this machine."
}

if (-not $NoIndex) {
    & $d.indexScript -Owner $d.indexOwner -Mods $d.modName -Note "SpeechBroker voice model deploy $($d.version)"
}
''
