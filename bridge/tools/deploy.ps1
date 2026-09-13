# Раскладка моста Envoy в mods\. Все пути и имена - в config/build.json, в коде их нет.
#
#   tools\deploy.ps1            показать, что будет сделано
#   tools\deploy.ps1 -Apply     выполнить
#
# Мост выкладывает только себя. Адаптер и слушатель - отдельные модули на своих
# ветках, и файлов соседа здесь просто нет. Общее у них одно - контракт, и мост
# отдаёт его папкой SDK внутри собственного мода: против неё собираются и наши
# два тестовых модуля, и любой чужой мод, который захочет говорить с Envoy.
#
# Состав профилей этот скрипт НЕ трогает: включение мода делается через мост MO2.
param([switch]$Apply, [switch]$NoIndex)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cfg  = Get-Content -LiteralPath (Join-Path $root 'config\build.json') -Raw | ConvertFrom-Json
$d    = $cfg.deploy
$enc  = New-Object Text.UTF8Encoding($false)

$game = @(Get-Process -Name SkyrimVR,SkyrimSE -ErrorAction SilentlyContinue)
if ($game.Count) { throw "Игра запущена ($($game.Name -join ', ')) - раскладка запрещена" }

function Expand-Path([string]$p) { $p.Replace('{root}', $root) }

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

$dll = Expand-Path $d.dll
if (Test-Path -LiteralPath $dll) {
    New-Item -ItemType Directory -Force (Join-Path $mod 'SKSE\Plugins\envoy') | Out-Null
    Copy-Item -LiteralPath $dll -Destination (Join-Path $mod 'SKSE\Plugins') -Force
    Copy-Item -LiteralPath (Expand-Path $d.defaultConfig) `
              -Destination (Join-Path $mod 'SKSE\Plugins\envoy\envoy.json') -Force
} else {
    Write-Warning "библиотека моста не собрана: $dll"
}

# SDK - единственное, что мост отдаёт наружу. Адаптеры и слушатели собираются
# против него, а не против нашего репозитория.
$sdk = Join-Path $mod 'SDK'
New-Item -ItemType Directory -Force $sdk | Out-Null
foreach ($f in $d.sdk) { Copy-Item -LiteralPath (Expand-Path $f) -Destination $sdk -Force }

$meta = @(
    '[General]'
    'gameName=SkyrimSE'
    'modid=0'
    "version=$($d.version)"
    "newestVersion=$($d.version)"
    'category="0,"'
    'installationFile='
    'notes=Envoy Framework - мост. Контракт для адаптеров и слушателей лежит в папке SDK.'
    ''
    '[installedFiles]'
    'size=0'
)
[IO.File]::WriteAllLines((Join-Path $mod 'meta.ini'), $meta, $enc)

'--- будет разложено ---'
'  {0,-40} {1} файлов' -f $d.modName, @(Get-ChildItem -LiteralPath $mod -Recurse -File).Count
'  в том числе SDK: {0}' -f ((Get-ChildItem -LiteralPath $sdk -File).Name -join ', ')
if (-not $Apply) { ''; 'сухой прогон - добавь -Apply'; return }

$target = Join-Path $d.modsRoot $d.modName
New-Item -ItemType Directory -Force $target | Out-Null
# Copy-Item -Recurse -Force над уже существующим деревом молча не перезаписывает
# файлы во вложенных папках, и раскладка отчитывалась об успехе, оставив в сборке
# библиотеку прошлой сборки. Копируем пофайлово и говорим, что изменилось.
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
'  разложено: {0} - новых {1}, обновлено {2}, без изменений {3}' -f $d.modName, $added, $updated, $same

if (-not $NoIndex) {
    & $d.indexScript -Owner $d.indexOwner -Mods $d.modName -Note "Envoy Framework deploy $($d.version)"
}
''
