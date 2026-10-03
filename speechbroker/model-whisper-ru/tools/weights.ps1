# The weights, and whether they are the weights.
#
#   tools\weights.ps1                       verify every weights\<id>
#   tools\weights.ps1 -Id whisper-ru-small  verify one
#   tools\weights.ps1 -Fetch                download what is missing, then verify
#   tools\weights.ps1 -Write                write SHA256SUMS beside each set
#
# THE WEIGHTS ARE NOT IN THE REPOSITORY. They are unmodified public releases and
# a recipe reproduces them exactly, so what is versioned is the recipe: SOURCE
# says where each set comes from, SHA256SUMS says which bytes are right, and
# weights\README.md says the rest. -Fetch is those two files made to run.
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
    [switch]$Write,
    [switch]$Fetch
)
$ErrorActionPreference = 'Stop'

if ($Fetch -and $Write) {
    # Signing what was just downloaded would record whatever arrived as correct,
    # which is the one thing SHA256SUMS exists to prevent. -Write is for the
    # person who PRODUCED a set of weights; -Fetch is for everybody after them.
    throw "-Fetch and -Write together would sign an unverified download. Fetch first, look at what it says, then decide."
}

$root = Split-Path -Parent $PSScriptRoot
$cfg  = Get-Content -LiteralPath (Join-Path $root 'config\build.json') -Raw | ConvertFrom-Json
$d    = $cfg.deploy
function Expand-Path([string]$p) { $p.Replace('{root}', $root) }

# TWO ROOTS, AND THE SPLIT IS THE WHOLE IDEA OF THIS SCRIPT.
#
#   the RECIPE  weights\<id>\   - SOURCE and SHA256SUMS, versioned with the module, ours
#   the STORE   <weightsStore>\<id>\ - the files themselves, outside the repository
#
# The recipe decides which sets exist: a folder in the store that no recipe names
# is not a set of weights, it is somebody's folder, and this script does not touch
# it. weightsStore absent means the two are the same folder, which is what this
# was before the bytes moved out to the modding root.
$recipeRoot = Expand-Path $d.weights
$storeRoot  = if ($d.PSObject.Properties['weightsStore']) { Expand-Path $d.weightsStore } else { $recipeRoot }
if (-not (Test-Path -LiteralPath $recipeRoot)) {
    throw "there is no weights recipe: $recipeRoot. It is where weights\<id>\ with SOURCE and SHA256SUMS goes; the module ships no weight files at all, and weights\README.md says where they come from."
}

$sets = @(Get-ChildItem -LiteralPath $recipeRoot -Directory)
if ($Id) { $sets = @($sets | Where-Object { $_.Name -eq $Id }) }
if (-not $sets) { throw "no set of weights found in $recipeRoot$(if ($Id) { " under the name $Id" })" }
if ($storeRoot -ne $recipeRoot) { "  store: $storeRoot" }

# UTF-8 without a byte order mark: sha256sum and every other tool reads this
# file as plain bytes, and a BOM would become part of the first hash.
$enc = New-Object Text.UTF8Encoding($false)
$bad = 0

foreach ($set in $sets) {
    # The recipe is read from the repository; every weight FILE is resolved
    # against the store. SHA256SUMS is authored here and copied there, because the
    # shim verifies it beside the weights at Start and the lay-out shows the mod
    # the store through a junction - so the copy has to travel with the bytes.
    $recipe   = $set.FullName
    $store    = Join-Path $storeRoot $set.Name
    $sumsFile = Join-Path $recipe 'SHA256SUMS'

    if ($Fetch) {
        # FETCHING IS A SEPARATE PASS AND THEN FALLS THROUGH TO VERIFYING, which
        # is the whole point: a download that is not checked has bought nothing.
        # What to fetch comes from the two files that ARE versioned - SHA256SUMS
        # names the files, SOURCE names where they come from - so this script
        # knows no URLs of its own and a new set needs no edit here.
        $sourceFile = Join-Path $recipe 'SOURCE'
        if (-not (Test-Path -LiteralPath $sourceFile)) {
            Write-Warning "  $($set.Name): no SOURCE file - nothing says where these weights come from. See weights\README.md."
            $bad++
            continue
        }
        if (-not (Test-Path -LiteralPath $sumsFile)) {
            Write-Warning "  $($set.Name): no SHA256SUMS - nothing says WHICH files to fetch."
            $bad++
            continue
        }
        $base = (Get-Content -LiteralPath $sourceFile -Encoding UTF8 |
            Where-Object { $_ -and -not $_.StartsWith('#') } | Select-Object -First 1).Trim().TrimEnd('/')

        foreach ($line in (Get-Content -LiteralPath $sumsFile -Encoding UTF8)) {
            if ($line -notmatch '^([0-9a-fA-F]{64})[ *]+(.+)$') { continue }
            $rel = $Matches[2]
            if ($rel -match '\.\.' -or $rel -match '^[/\\]' -or $rel -match ':') {
                Write-Warning "  $($set.Name): $rel leaves the folder - refusing to fetch it"
                $bad++
                continue
            }
            $path = Join-Path $store $rel
            # Already there and already right: gigabytes are not re-fetched to
            # prove a point. A file that is there but WRONG is left alone too -
            # the verify pass below reports it, and deleting somebody's file
            # because a hash disagreed is not this script's decision to make.
            if (Test-Path -LiteralPath $path) {
                '  {0,-24} {1} already here' -f $set.Name, $rel
                continue
            }
            $url = "$base/$rel"
            '  {0,-24} fetching {1}' -f $set.Name, $url
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $path) | Out-Null
            # curl.exe streams to disk; Invoke-WebRequest buffers the whole body
            # in memory, and these files are measured in gigabytes.
            $curl = (Get-Command curl.exe -ErrorAction SilentlyContinue)
            if ($curl) {
                & $curl.Source -L --fail --retry 3 -o $path $url
                if ($LASTEXITCODE -ne 0) { throw "curl failed with $LASTEXITCODE on $url" }
            } else {
                Invoke-WebRequest -Uri $url -OutFile $path -UseBasicParsing
            }
        }
        # The sums travel WITH the bytes: the shim checks SHA256SUMS beside the
        # weights at Start, the lay-out shows the mod the store through a
        # junction, and beside the weights is therefore the store.
        New-Item -ItemType Directory -Force $store | Out-Null
        Copy-Item -LiteralPath $sumsFile -Destination (Join-Path $store 'SHA256SUMS') -Force
        # and on to the verification, which is the part that matters
    }

    if ($Write) {
        # Files are listed in a stable order so that two runs of this script on
        # the same folder produce the same file and git shows no diff where
        # nothing changed.
        if (-not (Test-Path -LiteralPath $store)) {
            Write-Warning "  $($set.Name): nothing to sign - $store does not exist"
            continue
        }
        $lines = @()
        # SHA256SUMS cannot sign itself, and the other two are the RECIPE rather
        # than the weights: SOURCE says where to fetch from, README says the rest.
        # Signing them would make -Fetch try to download its own instructions.
        $notWeights = @('SHA256SUMS', 'SOURCE', 'README.md', 'README.ru.md')
        foreach ($file in (Get-ChildItem -LiteralPath $store -Recurse -File | Sort-Object FullName)) {
            if ($notWeights -contains $file.Name) { continue }
            $rel = $file.FullName.Substring($store.Length).TrimStart('\') -replace '\\', '/'
            $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            $lines += "$hash  $rel"
        }
        if (-not $lines) { Write-Warning "  $($set.Name): the folder is empty - nothing to sign"; continue }
        [IO.File]::WriteAllLines($sumsFile, $lines, $enc)
        Copy-Item -LiteralPath $sumsFile -Destination (Join-Path $store 'SHA256SUMS') -Force
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
        $path = Join-Path $store $rel
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
