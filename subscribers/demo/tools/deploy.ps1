# Раскладка тестового слушателя Envoy в mods\. Все пути и имена - в config/build.json.
#
#   tools\deploy.ps1            показать, что будет сделано
#   tools\deploy.ps1 -Apply     выполнить
#
# Слушатель - отдельный модуль: ни исходников моста, ни адаптера здесь нет.
# Объявления моста берутся из установленного мода моста (SDK) при компиляции.
param([switch]$Apply, [switch]$NoIndex)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cfg  = Get-Content -LiteralPath (Join-Path $root 'config\build.json') -Raw | ConvertFrom-Json
$d    = $cfg.deploy
$enc  = New-Object Text.UTF8Encoding($false)

$game = @(Get-Process -Name SkyrimVR,SkyrimSE -ErrorAction SilentlyContinue)
if ($game.Count) { throw "Игра запущена ($($game.Name -join ', ')) - раскладка запрещена" }

$pex = Join-Path $root 'build\pex'
if (-not (Test-Path -LiteralPath $pex)) { throw "Нет собранных скриптов. Сначала tools\build-papyrus.ps1" }

$dist = Join-Path $root 'dist'
Remove-Item -LiteralPath $dist -Recurse -Force -ErrorAction SilentlyContinue
$mod = Join-Path $dist $d.modName
New-Item -ItemType Directory -Force (Join-Path $mod 'Scripts\Source') | Out-Null

foreach ($s in $d.scripts) {
    Copy-Item -LiteralPath (Join-Path $pex "$s.pex")          -Destination (Join-Path $mod 'Scripts') -Force
    Copy-Item -LiteralPath (Join-Path $root "papyrus\$s.psc") -Destination (Join-Path $mod 'Scripts\Source') -Force
}
Copy-Item -LiteralPath (Join-Path $root "esp\$($d.esp)") -Destination $mod -Force

$meta = @(
    '[General]'
    'gameName=SkyrimSE'
    'modid=0'
    "version=$($d.version)"
    "newestVersion=$($d.version)"
    'category="0,"'
    'installationFile='
    "notes=Тестовые подписчики Envoy: наблюдатель, жадный и делящийся. Требует мода $($d.bridgeMod). В рабочие профили не нужны."
    ''
    '[installedFiles]'
    'size=0'
)
[IO.File]::WriteAllLines((Join-Path $mod 'meta.ini'), $meta, $enc)

'--- будет разложено ---'
'  {0,-40} {1} файлов' -f $d.modName, @(Get-ChildItem -LiteralPath $mod -Recurse -File).Count
if (-not $Apply) { ''; 'сухой прогон - добавь -Apply'; return }

$target = Join-Path $d.modsRoot $d.modName
New-Item -ItemType Directory -Force $target | Out-Null
Copy-Item -LiteralPath (Join-Path $mod '*') -Destination $target -Recurse -Force
"  разложено: $($d.modName)"

if (-not $NoIndex) {
    & $d.indexScript -Owner $d.indexOwner -Mods $d.modName -Note "Envoy Demo Subscriber deploy $($d.version)"
}
''
