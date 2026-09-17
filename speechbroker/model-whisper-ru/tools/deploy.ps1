# Laying the model mod out into mods\. Every path and name is in
# config\build.json.
#
#   tools\deploy.ps1            show what would be done
#   tools\deploy.ps1 -Apply     do it
#
# A model mod is a program now: a shim (an SKSE plugin) and a child process
# beside it. So this lays out FOUR things where it used to lay out one listing -
# the DLL, the child, the settings and the text - and puts a junction on the
# weights rather than copying them.
#
# WHY A JUNCTION AND NOT A COPY. The weights are gigabytes. Copying them into
# mods\ on every lay-out would spend minutes and a second copy of the disk for
# nothing, and the copy would then be the thing the game verifies while the
# original is the thing anybody edits. A junction has one set of bytes with two
# names.
#
# THE JUNCTION IS MADE IN mods\ AND NOT IN dist\, so the archive package.ps1
# builds out of dist\ does NOT carry the weights. That is deliberate and it is
# the integrator's call, not a script's: a gigabyte inside a mod archive is a
# decision about a download channel, and the weights are versioned through Git
# LFS with their own rules about who fetches what.
param([switch]$Apply, [switch]$NoIndex)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cfg  = Get-Content -LiteralPath (Join-Path $root 'config\build.json') -Raw | ConvertFrom-Json
$d    = $cfg.deploy
$enc  = New-Object Text.UTF8Encoding($false)

$game = @(Get-Process -Name SkyrimVR,SkyrimSE -ErrorAction SilentlyContinue)
if ($game.Count) { throw "The game is running ($($game.Name -join ', ')) - laying out is not allowed" }

function Expand-Path([string]$p) { $p.Replace('{root}', $root) }

$dist = Join-Path $root 'dist'
Remove-Item -LiteralPath $dist -Recurse -Force -ErrorAction SilentlyContinue
$mod  = Join-Path $dist $d.modName
$own  = Join-Path $mod $d.ownRel
New-Item -ItemType Directory -Force $own | Out-Null

# --- the shim -----------------------------------------------------------------
# A mod without it is a mod that does nothing, so a missing build is a refusal
# rather than a warning: laying out quietly would leave the previous library in
# mods\ and the run would be testing last week.
$dll = Expand-Path $d.dll
if (-not (Test-Path -LiteralPath $dll)) {
    throw "the shim is not built: $dll  (cmake --build build --config Release)"
}
New-Item -ItemType Directory -Force (Join-Path $mod 'SKSE\Plugins') | Out-Null
Copy-Item -LiteralPath $dll -Destination (Join-Path $mod 'SKSE\Plugins') -Force

# --- the child ----------------------------------------------------------------
# Its own build, its own CMakeLists, and it is refused for the same reason: a
# shim with no child raises nothing and says so five times before giving up.
$child = Expand-Path $d.child
if (-not (Test-Path -LiteralPath $child)) {
    throw "the child is not built: $child  (cd child && cmake --build build --config Release)"
}
$childInto = Join-Path $own $d.childRel
New-Item -ItemType Directory -Force $childInto | Out-Null
Copy-Item -LiteralPath $child -Destination $childInto -Force

# The backend the child loads at run time is NOT ours to ship: whisper.dll is
# third-party, it is not vendored by this repository, and the licence of a build
# somebody else made is their business. If a person has put one beside the child
# in the source tree, it rides along; if not, the mod lays out cleanly and the
# child refuses at start-up with one sentence naming the file it wanted.
$backend = Join-Path $root 'child\runtime'
if (Test-Path -LiteralPath $backend) {
    Get-ChildItem -LiteralPath $backend -File | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $childInto -Force
    }
}

# --- the settings and the text ------------------------------------------------
# The settings are PRIVATE to this mod and deliberately do not go where the
# adapter's own models folder used to be: nobody but this DLL reads them, and a
# file in the adapter's folder would be parsed by the adapter as a listing of the
# kind that no longer exists.
foreach ($listing in Get-ChildItem -LiteralPath (Expand-Path $d.settings) -Filter *.json -File) {
    Copy-Item -LiteralPath $listing.FullName -Destination $own -Force
}

# The tables ride as they are, tab separated UTF-8, and are read by the shim
# itself. They are NOT turned into Interface\Translations: a model mod shows the
# player nothing, all of this is the log, and building the engine's format would
# mean depending on the BRIDGE's SDK for a file the engine never reads.
$locInto = Join-Path $own $d.localizationRel
New-Item -ItemType Directory -Force $locInto | Out-Null
$tables = @(Get-ChildItem -LiteralPath (Expand-Path $d.localization) -Filter *.txt -File)
if (-not $tables) { throw "there are no localisation tables in $(Expand-Path $d.localization) - the log would be bare keys" }
foreach ($table in $tables) { Copy-Item -LiteralPath $table.FullName -Destination $locInto -Force }

# --- the licence and the notices ----------------------------------------------
# Whoever unpacked the archive has to find them inside it: they will not open the
# page they downloaded from again.
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
    "notes=A model mod for SpeechBrokerVoiceAdapter: Whisper, Russian, as an SKSE shim plus a child process. Needs the mod $($d.adapterMod)."
    ''
    '[installedFiles]'
    'size=0'
)
[IO.File]::WriteAllLines((Join-Path $mod 'meta.ini'), $meta, $enc)

'--- to be laid out ---'
'  {0,-44} {1} files' -f $d.modName, @(Get-ChildItem -LiteralPath $mod -Recurse -File).Count

# --- the weights --------------------------------------------------------------
# Reported separately and never copied. A set that is not there is a warning and
# not a refusal: the shim lays out, the model refuses at Start with a line naming
# the folder, and the other model may still work.
# The RECIPE says which sets exist; the STORE holds their bytes and is what the
# junction points at. They are two folders now: the recipe is versioned with the
# module, the bytes live in the modding root beside MO2 and SSEEdit like every
# other thing somebody else wrote. weightsStore absent - they are the same folder,
# which is what this was before the bytes moved out.
$recipeRoot = Expand-Path $d.weights
$storeRoot  = if ($d.PSObject.Properties['weightsStore']) { Expand-Path $d.weightsStore } else { $recipeRoot }
$sets = @()
if (Test-Path -LiteralPath $recipeRoot) {
    foreach ($named in (Get-ChildItem -LiteralPath $recipeRoot -Directory)) {
        $here = Join-Path $storeRoot $named.Name
        if (Test-Path -LiteralPath $here) { $sets += (Get-Item -LiteralPath $here) }
        else { Write-Warning "  $($named.Name): the recipe is here but the weights are not - $here. Run tools\weights.ps1 -Fetch." }
    }
}
if ($sets) {
    foreach ($set in $sets) {
        $sums = Join-Path $set.FullName 'SHA256SUMS'
        $signed = if (Test-Path -LiteralPath $sums) { 'signed' } else { 'NOT signed - the shim will refuse it' }
        '  weights {0,-24} {1}' -f $set.Name, $signed
    }
} else {
    Write-Warning "there are no weights in $storeRoot - the models will refuse at Start. See README.md, 'Where the weights go'."
}

if (-not $Apply) { ''; 'dry run - add -Apply'; return }

$target = Join-Path $d.modsRoot $d.modName
New-Item -ItemType Directory -Force $target | Out-Null
# Copy-Item -Recurse -Force over a tree that already exists silently fails to
# overwrite files in nested folders, and the lay-out then reports success while
# leaving the library of the previous build in the build. We copy file by file
# and say what changed.
$added = 0; $updated = 0; $same = 0
Get-ChildItem -LiteralPath $mod -Recurse -File | ForEach-Object {
    $rel = $_.FullName.Substring($mod.Length).TrimStart('\')
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
'  laid out: {0} - new {1}, updated {2}, unchanged {3}' -f $d.modName, $added, $updated, $same

# The junction on each set of weights, made in the laid-out mod and not in dist\:
# dist\ is wiped at the head of every run, and a junction is not a thing to make
# and destroy twice a minute.
foreach ($set in $sets) {
    $link = Join-Path (Join-Path $target $d.ownRel) (Join-Path $d.weightsRel $set.Name)
    $linkParent = Split-Path -Parent $link
    New-Item -ItemType Directory -Force $linkParent | Out-Null
    if (Test-Path -LiteralPath $link) {
        $existing = Get-Item -LiteralPath $link -Force
        if ($existing.LinkType) {
            Remove-Item -LiteralPath $link -Force
        } else {
            # A real folder of real weights is somebody's 2 GB and is never
            # removed by a lay-out script. Left alone and said out loud.
            Write-Warning "  $($set.Name) in the mod is a real folder and not a junction - leaving it as it is"
            continue
        }
    }
    New-Item -ItemType Junction -Path $link -Target $set.FullName | Out-Null
    '  junction: {0} -> {1}' -f (Join-Path $d.weightsRel $set.Name), $set.FullName
}

if (-not $NoIndex) {
    & $d.indexScript -Owner $d.indexOwner -Mods @($d.modName) -Note "SpeechBroker Voice Model Whisper RU deploy $($d.version)"
}
''
