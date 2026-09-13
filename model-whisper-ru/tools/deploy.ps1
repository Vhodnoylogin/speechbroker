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

# Листок кладётся как есть - править его нельзя. Адаптер отказывается запускать
# что-либо за пределами папки моделей, поэтому абсолютный путь к службе в листке
# был бы просто отклонён. Служба этой машины подставляется иначе: рядом с листком
# заводится запускатель whisper-ru\run.cmd, а путь для него берётся из
# build.local.json, которого в git нет.
Copy-Item -LiteralPath $descriptor -Destination $targetDir -Force

$localPath = Join-Path $root 'config\build.local.json'
if (Test-Path -LiteralPath $localPath) {
    $local = Get-Content -LiteralPath $localPath -Raw | ConvertFrom-Json
    if ($local.service) {
        $runDir = Join-Path $targetDir 'whisper-ru'
        New-Item -ItemType Directory -Force $runDir | Out-Null
        $passed = ($local.service.args | ForEach-Object { '"' + $_ + '"' }) -join ' '
        # %* передаёт дальше доводы адаптера: --parent-pid и --envoy-token.
        $run = @(
            '@echo off'
            ('"{0}" {1} %*' -f $local.service.exec, $passed)
        )
        [IO.File]::WriteAllLines((Join-Path $runDir 'run.cmd'), $run, $enc)
        "  запускатель собран из build.local.json: $($local.service.exec)"
    }
} else {
    Write-Warning "build.local.json нет - запускателя whisper-ru\run.cmd не будет, и служба сама не поднимется. Для прогона на этой машине заведи его."
}

# Если мод-модель везёт свою службу, она лежит рядом с листком и уезжает целиком.
$serviceDir = Expand-Path $d.serviceDir
if (Test-Path -LiteralPath $serviceDir) {
    Copy-Item -LiteralPath $serviceDir -Destination $targetDir -Recurse -Force
}

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
