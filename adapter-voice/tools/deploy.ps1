# Раскладка тестового адаптера Envoy в mods\. Все пути и имена - в config/build.json.
#
#   tools\deploy.ps1            показать, что будет сделано
#   tools\deploy.ps1 -Apply     выполнить
#
# Адаптер - отдельный модуль: исходников моста здесь нет, и он их не видит.
# Общий у них только контракт, и берётся он из установленного мода моста (SDK).
param([switch]$Apply, [switch]$NoIndex)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cfg  = Get-Content -LiteralPath (Join-Path $root 'config\build.json') -Raw | ConvertFrom-Json
$d    = $cfg.deploy
$enc  = New-Object Text.UTF8Encoding($false)

$game = @(Get-Process -Name SkyrimVR,SkyrimSE -ErrorAction SilentlyContinue)
if ($game.Count) { throw "Игра запущена ($($game.Name -join ', ')) - раскладка запрещена" }

if (-not (Test-Path -LiteralPath (Join-Path $d.sdk 'envoy-adapter.h'))) {
    throw "Контракт моста не найден в $($d.sdk). Сначала выложи мост."
}

function Expand-Path([string]$p) { $p.Replace('{root}', $root) }

$dist = Join-Path $root 'dist'
Remove-Item -LiteralPath $dist -Recurse -Force -ErrorAction SilentlyContinue
$mod = Join-Path $dist $d.modName
New-Item -ItemType Directory -Force (Join-Path $mod $d.settingsTargetRel) | Out-Null
Copy-Item -LiteralPath (Expand-Path $d.settings) -Destination (Join-Path $mod $d.settingsTargetRel) -Force

$dll = Expand-Path $d.dll
if (Test-Path -LiteralPath $dll) {
    New-Item -ItemType Directory -Force (Join-Path $mod 'SKSE\Plugins') | Out-Null
    Copy-Item -LiteralPath $dll -Destination (Join-Path $mod 'SKSE\Plugins') -Force
} else {
    Write-Warning "библиотека адаптера не собрана: $dll"
}

$meta = @(
    '[General]'
    'gameName=SkyrimSE'
    'modid=0'
    "version=$($d.version)"
    "newestVersion=$($d.version)"
    'category="0,"'
    'installationFile='
    "notes=Тестовый адаптер Envoy к службе распознавания и синтеза речи. Требует мода $($d.bridgeMod)."
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
    & $d.indexScript -Owner $d.indexOwner -Mods $d.modName -Note "Envoy Voice Adapter deploy $($d.version)"
}
''
