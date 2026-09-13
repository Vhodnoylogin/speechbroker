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
New-Item -ItemType Directory -Force (Join-Path $mod 'Scripts') | Out-Null

# В мод едет только собранное. Исходники скриптов - не часть мода: игра их
# не читает, а игрок получает лишние файлы, которые ему нечем открыть.
# Их место - в поставке для авторов, ниже.
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
    Write-Warning "библиотека моста не собрана: $dll"
}

# Поставка для авторов модов - ОТДЕЛЬНАЯ, а не папка внутри мода. Заголовки
# C++, объявления Papyrus и описания контракта игра не читает вовсе: игроку они
# не нужны, а автору мода нужны без игры. На Nexus это разные файлы на одной
# странице: основной и необязательный «modding source».
#
# Собирается она всё равно раскладкой и всё равно кладётся в mods\: против неё
# собираются адаптер и подписчик - ровно так же, как это сделает чужой автор,
# распаковавший необязательный файл. В профилях этот мод держать выключенным:
# игре в нём брать нечего.
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
    'notes=Envoy Framework - поставка для авторов модов: контракт, объявления Papyrus, исходники скриптов. Игре не нужна, в профилях держать выключенной.'
    ''
    '[installedFiles]'
    'size=0'
)
[IO.File]::WriteAllLines((Join-Path $sdkMod 'meta.ini'), $sdkMeta, $enc)

# Лицензия и перечень заимствованного едут в каждый мод. Человек, распаковавший
# архив, обязан найти их внутри: страницу, с которой он качал, он больше
# не откроет, а условия шести чужих проектов требуют, чтобы текст был в поставке.
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
    'notes=Envoy Framework - мост. Контракт для адаптеров и слушателей лежит в папке SDK.'
    ''
    '[installedFiles]'
    'size=0'
)
[IO.File]::WriteAllLines((Join-Path $mod 'meta.ini'), $meta, $enc)

'--- будет разложено ---'
'  {0,-40} {1} файлов' -f $d.modName, @(Get-ChildItem -LiteralPath $mod -Recurse -File).Count
'  {0,-40} {1} файлов' -f $d.sdkName, @(Get-ChildItem -LiteralPath $sdkMod -Recurse -File).Count
if (-not $Apply) { ''; 'сухой прогон - добавь -Apply'; return }

# Поставок две - мод и SDK, - и раскладываются они одинаково. Раньше копировался
# только мод, и папка SDK, собранная рядом, до сборки не доезжала.
$copied = @()
foreach ($pack in Get-ChildItem -LiteralPath $dist -Directory) {
    $target = Join-Path $d.modsRoot $pack.Name
    New-Item -ItemType Directory -Force $target | Out-Null
    # Copy-Item -Recurse -Force над уже существующим деревом молча не перезаписывает
    # файлы во вложенных папках, и раскладка отчитывалась об успехе, оставив в сборке
    # библиотеку прошлой сборки. Копируем пофайлово и говорим, что изменилось.
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
    '  разложено: {0} - новых {1}, обновлено {2}, без изменений {3}' -f $pack.Name, $added, $updated, $same
    $copied += $pack.Name
}

if (-not $NoIndex) {
    & $d.indexScript -Owner $d.indexOwner -Mods $copied -Note "Envoy Framework deploy $($d.version)"
}
''
