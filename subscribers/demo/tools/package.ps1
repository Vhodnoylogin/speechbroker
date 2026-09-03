# Упаковка модуля в архив и установка из него, как любого другого мода.
# Все пути - в config/build.json.
#
#   tools\package.ps1           показать, что будет сделано
#   tools\package.ps1 -Apply    упаковать, положить в downloads и установить
param([switch]$Apply)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cfg  = Get-Content -LiteralPath (Join-Path $root 'config\build.json') -Raw | ConvertFrom-Json
$d    = $cfg.deploy
$enc  = New-Object Text.UTF8Encoding($false)

$game = @(Get-Process -Name SkyrimVR,SkyrimSE -ErrorAction SilentlyContinue)
if ($game.Count) { throw "Игра запущена ($($game.Name -join ', ')) - состав сборки менять нельзя" }
if (-not (Test-Path -LiteralPath $d.sevenZip)) { throw "7-Zip не найден: $($d.sevenZip)" }

# dist собирается тем же скриптом, что и раскладка: одна логика на оба пути.
& (Join-Path $PSScriptRoot 'deploy.ps1') | Out-Null

$dist = Join-Path $root 'dist'
$mods = Get-ChildItem -LiteralPath $dist -Directory
if (-not $mods) { throw "dist пуст - сначала собери скрипты и плагин" }

$plan = foreach ($m in $mods) {
    [pscustomobject]@{
        Name    = $m.Name
        Source  = $m.FullName
        Archive = Join-Path $d.downloadsRoot ("{0}-{1}.7z" -f $m.Name, $d.version)
        Target  = Join-Path $d.modsRoot $m.Name
        Files   = @(Get-ChildItem -LiteralPath $m.FullName -Recurse -File).Count - 1  # meta.ini в архив не идёт
    }
}

'--- план ---'
foreach ($p in $plan) { '  {0,-38} {1} файлов -> {2}' -f $p.Name, $p.Files, (Split-Path -Leaf $p.Archive) }
if (-not $Apply) { ''; 'сухой прогон - добавь -Apply'; return }

''
foreach ($p in $plan) {
    Remove-Item -LiteralPath $p.Archive -Force -ErrorAction SilentlyContinue
    Push-Location $p.Source
    try {
        & $d.sevenZip a -t7z -mx=5 -bso0 -bsp0 $p.Archive '*' '-xr!meta.ini' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "7-Zip вернул $LASTEXITCODE" }
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
    '  архив: {0} ({1:N0} b)' -f (Split-Path -Leaf $p.Archive), (Get-Item $p.Archive).Length

    # установка из архива - той же распаковкой, что делает установщик MO2,
    # но без его диалогов: при открытой MO2 их нельзя пройти автоматически.
    if (Test-Path -LiteralPath $p.Target) { Remove-Item -LiteralPath $p.Target -Recurse -Force }
    New-Item -ItemType Directory -Force $p.Target | Out-Null
    & $d.sevenZip x $p.Archive "-o$($p.Target)" -y -bso0 -bsp0 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "распаковка вернула $LASTEXITCODE" }

    $modMeta = Get-Content -LiteralPath (Join-Path $p.Source 'meta.ini') -Raw
    $modMeta = $modMeta -replace 'installationFile=', ("installationFile=" + (Split-Path -Leaf $p.Archive))
    [IO.File]::WriteAllText((Join-Path $p.Target 'meta.ini'), $modMeta, $enc)
    '  установлен: {0}' -f $p.Name
}

# мост попросим перечитать список модов - иначе открытая MO2 их не увидит
try {
    $token = (Get-Content -LiteralPath $d.bridgeToken -Raw).Trim()
    Invoke-RestMethod "$($d.bridgeUrl)/refresh" -Method Post -Headers @{ 'X-Token' = $token } -TimeoutSec 20 | Out-Null
    '  MO2 обновила список модов'
} catch {
    "  мост не ответил, обнови список в MO2 вручную: $($_.Exception.Message)"
}

& $d.indexScript -Owner $d.indexOwner -Mods $plan.Name -Note "$($d.modName) $($d.version) установлен из архива"

