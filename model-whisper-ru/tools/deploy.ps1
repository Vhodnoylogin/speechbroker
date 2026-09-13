# Раскладка мод-модели в mods\. Все пути и имена - в config/build.json.
#
#   tools\deploy.ps1            показать, что будет сделано
#   tools\deploy.ps1 -Apply     выполнить
#
# Мод-модель - отдельный модуль: ни исходников адаптера, ни исходников моста
# здесь нет. Всё, что она делает, - кладёт один листок в папку, которую читает
# адаптер. Контракт листка - в adapter-voice\contract\envoy-voice-model.md.
#
# Служба этой модели живёт на ветке voice и в поставку не входит: поэтому
# в листке стоят относительные пути внутрь мода, а путь к службе на ЭТОЙ машине
# берётся из config\build.local.json, которого в git нет.
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
    Write-Warning "адаптер '$($d.adapterMod)' в сборке не найден - листок положить можно, но читать его будет некому"
}

$descriptor = Expand-Path $d.descriptor
if (-not (Test-Path -LiteralPath $descriptor)) { throw "листок модели не найден: $descriptor" }

$dist = Join-Path $root 'dist'
Remove-Item -LiteralPath $dist -Recurse -Force -ErrorAction SilentlyContinue
$mod = Join-Path $dist $d.modName
$targetDir = Join-Path $mod ($d.targetRel -replace '/', '\')
New-Item -ItemType Directory -Force $targetDir | Out-Null

# Листок кладётся как есть, но если на этой машине служба лежит не внутри мода,
# её путь подставляется из build.local.json. В git этот файл не попадает: путь
# к чужому каталогу верен только здесь и публикации не подлежит.
$doc = Get-Content -LiteralPath $descriptor -Raw | ConvertFrom-Json
$localPath = Join-Path $root 'config\build.local.json'
if (Test-Path -LiteralPath $localPath) {
    $local = Get-Content -LiteralPath $localPath -Raw | ConvertFrom-Json
    if ($local.service) {
        $doc.autoStart.exec       = $local.service.exec
        $doc.autoStart.args       = @($local.service.args)
        $doc.autoStart.workingDir = $local.service.dir
        "  служба берётся из build.local.json: $($local.service.exec)"
    }
} else {
    Write-Warning "build.local.json нет - в листке останутся пути внутрь мода, а службы там нет. Для прогона на этой машине заведи его."
}
$doc | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath (Join-Path $targetDir (Split-Path -Leaf $descriptor)) -Encoding utf8

# Если мод-модель везёт свою службу, она лежит рядом с листком и уезжает целиком.
$serviceDir = Expand-Path $d.serviceDir
if (Test-Path -LiteralPath $serviceDir) {
    Copy-Item -LiteralPath $serviceDir -Destination $targetDir -Recurse -Force
}

$meta = @(
    '[General]'
    'gameName=SkyrimSE'
    'modid=0'
    "version=$($d.version)"
    "newestVersion=$($d.version)"
    'category="0,"'
    'installationFile='
    "notes=Модель для адаптера Envoy: распознавание и синтез русской речи. Требует мода $($d.adapterMod)."
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
    & $d.indexScript -Owner $d.indexOwner -Mods $d.modName -Note "Envoy voice model deploy $($d.version)"
}
''
