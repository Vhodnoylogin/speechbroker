# Packing the module into an archive and installing from it, like any other mod.
# Every path is in config/build.json.
#
#   tools\package.ps1           show what would be done
#   tools\package.ps1 -Apply    pack, put into downloads and install
param([switch]$Apply)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cfg  = Get-Content -LiteralPath (Join-Path $root 'config\build.json') -Raw | ConvertFrom-Json
$d    = $cfg.deploy
$enc  = New-Object Text.UTF8Encoding($false)
function Expand-Path([string]$p) { $p.Replace('{root}', $root) }
$bridgeToken = Expand-Path $d.bridgeToken

$game = @(Get-Process -Name SkyrimVR,SkyrimSE -ErrorAction SilentlyContinue)
if ($game.Count) { throw "The game is running ($($game.Name -join ', ')) - the make-up of the build must not be changed" }
if (-not (Test-Path -LiteralPath $d.sevenZip)) { throw "7-Zip not found: $($d.sevenZip)" }

# dist is built by the same script that lays out: one logic for both paths.
& (Join-Path $PSScriptRoot 'deploy.ps1') | Out-Null

$dist = Join-Path $root 'dist'
$mods = Get-ChildItem -LiteralPath $dist -Directory
if (-not $mods) { throw "dist is empty - build the scripts and the plugin first" }

$plan = foreach ($m in $mods) {
    [pscustomobject]@{
        Name    = $m.Name
        Source  = $m.FullName
        Archive = Join-Path $d.downloadsRoot ("{0}-{1}.7z" -f $m.Name, $d.version)
        Target  = Join-Path $d.modsRoot $m.Name
        Files   = @(Get-ChildItem -LiteralPath $m.FullName -Recurse -File).Count - 1  # meta.ini does not go into the archive
    }
}

'--- plan ---'
foreach ($p in $plan) { '  {0,-38} {1} files -> {2}' -f $p.Name, $p.Files, (Split-Path -Leaf $p.Archive) }
if (-not $Apply) { ''; 'dry run - add -Apply'; return }

''
foreach ($p in $plan) {
    Remove-Item -LiteralPath $p.Archive -Force -ErrorAction SilentlyContinue
    Push-Location $p.Source
    try {
        & $d.sevenZip a -t7z -mx=5 -bso0 -bsp0 $p.Archive '*' '-xr!meta.ini' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "7-Zip returned $LASTEXITCODE" }
    } finally { Pop-Location }

    $meta = @(
        '[General]'
        'gameName=SkyrimSE'
        'modID=0'
        'fileID=0'
        "name=$($p.Name)"
        "modName=$($p.Name)"
        "version=$($d.version).0"
        'newestVersion='
        'fileCategory=1'
        'repository='
        'installed=true'
        'uninstalled=false'
        'paused=false'
        'removed=false'
    )
    [IO.File]::WriteAllLines("$($p.Archive).meta", $meta, $enc)
    '  archive: {0} ({1:N0} b)' -f (Split-Path -Leaf $p.Archive), (Get-Item $p.Archive).Length

    # MO2 installs it, not us. The /install route of the bridge plugin creates the
    # mod through createMod and unpacks the archive itself - without a single
    # dialogue. Unpacking on our own, past MO2, gave a mod it had never created:
    # with no source archive and no record in its own database.
    $body = @{ archive = $p.Archive; name = $p.Name; paths = @(''); mode = 'replace' } |
            ConvertTo-Json -Compress
    $token = (Get-Content -LiteralPath $bridgeToken -Raw).Trim()
    $res = Invoke-RestMethod "$($d.bridgeUrl)/install" -Method Post `
               -Body ([Text.Encoding]::UTF8.GetBytes($body)) `
               -ContentType 'application/json; charset=utf-8' `
               -Headers @{ 'X-Token' = $token } -TimeoutSec 600
    '  installed through MO2: {0}' -f $p.Name
}

# ask the bridge to reread the list of mods - an open MO2 will not see them otherwise
try {
    $token = (Get-Content -LiteralPath $bridgeToken -Raw).Trim()
    Invoke-RestMethod "$($d.bridgeUrl)/refresh" -Method Post -Headers @{ 'X-Token' = $token } -TimeoutSec 20 | Out-Null
    '  MO2 refreshed its list of mods'
} catch {
    "  the bridge did not answer, refresh the list in MO2 by hand: $($_.Exception.Message)"
}

& $d.indexScript -Owner $d.indexOwner -Mods $plan.Name -Note "$($d.modName) $($d.version) installed from an archive"

