# Раскладка мод-модели в mods\. Все пути и имена - в config/build.json.
#
#   tools\deploy.ps1            показать, что будет сделано
#   tools\deploy.ps1 -Apply     выполнить
#
# Мод-модель - отдельный модуль и НЕ программа: ни микрофона, ни порта, ни
# запускаемого файла в ней нет. Она везёт модель - листок и файлы весов.
# Контракт листка - в adapter-voice\contract\envoy-voice-model.md.
#
# Веса этой модели на нашей машине лежат в чужом модуле (ветка voice), и копировать
# гигабайты в сборку незачем: раскладка ставит на них связку каталогов по
# config\build.local.json, которого в git нет. У человека, скачавшего мод, веса
# лежат внутри мода, и связка не понадобится.
param([switch]$Apply, [switch]$NoIndex)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cfg  = Get-Content -LiteralPath (Join-Path $root 'config\build.json') -Raw | ConvertFrom-Json
$d    = $cfg.deploy
$enc  = New-Object Text.UTF8Encoding($false)

$game = @(Get-Process -Name SkyrimVR,SkyrimSE -ErrorAction SilentlyContinue)
if ($game.Count) { throw "Игра запущена ($($game.Name -join ', ')) - раскладка запрещена" }

function Expand-Path([string]$p) { $p.Replace('{root}', $root) }

$adapter = Join-Path $d.modsRoot $d.adapterMod
if (-not (Test-Path -LiteralPath $adapter)) {
    Write-Warning "адаптер '$($d.adapterMod)' в сборке не найден - листки положить можно, но читать их будет некому"
}

$listings = Join-Path $root 'models'
if (-not (Test-Path -LiteralPath $listings)) { throw "папка с листками не найдена: $listings" }

$dist = Join-Path $root 'dist'
Remove-Item -LiteralPath $dist -Recurse -Force -ErrorAction SilentlyContinue
$mod = Join-Path $dist $d.modName
$rel = $d.targetRel -replace '/', '\'
$targetDir = Join-Path $mod $rel
New-Item -ItemType Directory -Force $targetDir | Out-Null

# Листки кладутся как есть - править их нечем и незачем: ни путей чужой машины,
# ни адресов в них нет по самому устройству контракта.
Get-ChildItem -LiteralPath $listings -Filter '*.json' -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $targetDir -Force
}

# Лицензия и перечень заимствованного едут внутри мода.
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
    "notes=Модели распознавания и синтеза русской речи для адаптера Envoy. Требует мода $($d.adapterMod)."
    ''
    '[installedFiles]'
    'size=0'
)
[IO.File]::WriteAllLines((Join-Path $mod 'meta.ini'), $meta, $enc)

'--- будет разложено ---'
'  {0,-46} {1} файлов' -f $d.modName, @(Get-ChildItem -LiteralPath $mod -Recurse -File).Count
if (-not $Apply) { ''; 'сухой прогон - добавь -Apply'; return }

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
'  разложено: {0} - новых {1}, обновлено {2}, без изменений {3}' -f $d.modName, $added, $updated, $same

# Веса. На этой машине - связкой каталогов на настоящее место, чтобы не копировать
# гигабайты в сборку. Связка снимается через Directory::Delete: Remove-Item над
# связкой рискует уйти в цель и вычистить сами веса.
$localPath = Join-Path $root 'config\build.local.json'
if (Test-Path -LiteralPath $localPath) {
    $local = Get-Content -LiteralPath $localPath -Raw | ConvertFrom-Json
    if ($local.weights) {
        foreach ($entry in $local.weights.PSObject.Properties) {
            $listing = Get-ChildItem -LiteralPath $listings -Filter '*.json' -File |
                Where-Object { (Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json).id -eq $entry.Name } |
                Select-Object -First 1
            if (-not $listing) {
                Write-Warning "листка для модели '$($entry.Name)' нет - связку не ставлю"
                continue
            }
            $weights = (Get-Content -LiteralPath $listing.FullName -Raw | ConvertFrom-Json).weights
            $link = Join-Path (Join-Path $target $rel) ($weights -replace '/', '\')
            $parent = Split-Path -Parent $link
            if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Force $parent | Out-Null }
            if (Test-Path -LiteralPath $link) { [System.IO.Directory]::Delete($link, $false) }
            New-Item -ItemType Junction -Path $link -Target $entry.Value | Out-Null
            '  веса {0}: связка на {1}' -f $entry.Name, $entry.Value
        }
    }
} else {
    Write-Warning "build.local.json нет - весов в моде не будет, и служба не найдёт моделей. Для прогона на этой машине заведи его."
}

if (-not $NoIndex) {
    & $d.indexScript -Owner $d.indexOwner -Mods $d.modName -Note "Envoy voice model deploy $($d.version)"
}
''
