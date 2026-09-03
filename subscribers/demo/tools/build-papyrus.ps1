# Компиляция скриптов тестового слушателя. Все пути - в config/build.json.
#
# Объявления моста (Envoy.psc) берутся из установленного мода моста, папки SDK:
# копии чужого файла в этом модуле нет и быть не должно.
param([switch]$Quiet)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cfg  = Get-Content -LiteralPath (Join-Path $root 'config\build.json') -Raw | ConvertFrom-Json

function Expand-CfgPath([string]$p) {
    $p.Replace('{root}', $root).Replace('{creationKit}', $cfg.creationKit).Replace('{sdk}', $cfg.deploy.sdk) -replace '/', '\'
}

$compiler = Join-Path $cfg.creationKit 'Papyrus Compiler\PapyrusCompiler.exe'
if (-not (Test-Path -LiteralPath $compiler)) { throw "Компилятор не найден: $compiler. Нужен Creation Kit." }
if (-not (Test-Path -LiteralPath (Join-Path $cfg.deploy.sdk 'Envoy.psc'))) {
    throw "Объявления моста не найдены в $($cfg.deploy.sdk). Сначала выложи мост."
}

$src     = Expand-CfgPath $cfg.papyrusSource
$out     = Expand-CfgPath $cfg.papyrusOutput
$imports = ($cfg.imports | ForEach-Object { Expand-CfgPath $_ }) -join ';'
New-Item -ItemType Directory -Force $out | Out-Null

# Компилятор Papyrus читает исходник без BOM как ANSI (здесь cp1251) и молча
# портит кириллицу: словари подписчиков перестают совпадать с распознанным
# текстом, а надписи на экране превращаются в мусор. BOM снимает вопрос.
$bom = [byte[]](0xEF, 0xBB, 0xBF)
foreach ($f in Get-ChildItem -LiteralPath $src -Filter *.psc -Recurse) {
    $raw = [IO.File]::ReadAllBytes($f.FullName)
    if ($raw.Length -ge 3 -and $raw[0] -eq $bom[0] -and $raw[1] -eq $bom[1] -and $raw[2] -eq $bom[2]) { continue }
    [IO.File]::WriteAllBytes($f.FullName, $bom + $raw)
    Write-Host "  BOM добавлен: $($f.Name)"
}

$argv = @($src, "-f=$($cfg.papyrusFlags)", "-i=$imports", "-o=$out", '-all')
& $compiler @argv
if ($LASTEXITCODE -ne 0) { throw "Компиляция Papyrus завершилась с кодом $LASTEXITCODE" }

if (-not $Quiet) {
    Get-ChildItem $out -Filter *.pex | ForEach-Object { '  {0,-22} {1,7:N0} b' -f $_.Name, $_.Length }
}
