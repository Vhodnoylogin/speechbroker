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
function Expand-Path([string]$p) { $p.Replace('{root}', $root) }
$bridgeToken = Expand-Path $d.bridgeToken

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

    # Ставит MO2, а не мы. Маршрут /install плагина-моста заводит мод через
    # createMod и распаковывает архив сам - без единого диалога. Своя распаковка
    # мимо MO2 давала мод, которого она не заводила: без архива-источника
    # и без записи в своей базе.
    $body = @{ archive = $p.Archive; name = $p.Name; paths = @(''); mode = 'replace' } |
            ConvertTo-Json -Compress
    $token = (Get-Content -LiteralPath $bridgeToken -Raw).Trim()
    $res = Invoke-RestMethod "$($d.bridgeUrl)/install" -Method Post `
               -Body ([Text.Encoding]::UTF8.GetBytes($body)) `
               -ContentType 'application/json; charset=utf-8' `
               -Headers @{ 'X-Token' = $token } -TimeoutSec 600
    '  установлен через MO2: {0}' -f $p.Name
}

# мост попросим перечитать список модов - иначе открытая MO2 их не увидит
try {
    $token = (Get-Content -LiteralPath $bridgeToken -Raw).Trim()
    Invoke-RestMethod "$($d.bridgeUrl)/refresh" -Method Post -Headers @{ 'X-Token' = $token } -TimeoutSec 20 | Out-Null
    '  MO2 обновила список модов'
} catch {
    "  мост не ответил, обнови список в MO2 вручную: $($_.Exception.Message)"
}

& $d.indexScript -Owner $d.indexOwner -Mods $plan.Name -Note "$($d.modName) $($d.version) установлен из архива"

