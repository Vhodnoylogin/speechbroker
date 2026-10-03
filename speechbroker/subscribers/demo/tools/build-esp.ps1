# Плагин и его исходник.
#
#   tools\build-esp.ps1                 собрать esp\<имя> из esp\source
#   tools\build-esp.ps1 -FromPlugin     обратно: разобрать плагин в esp\source
#
# У .esp нет текстового исходника по природе: это база записей, а не результат
# компиляции. Исходником служит его разбор в текст, и делает это Spriggit - файл
# на запись плюс RecordData.yaml и spriggit-meta.json, где записаны версия самого
# Spriggit и редакция игры, чтобы обратная сборка была воспроизводима. Разбор и
# сборка проверены на совпадение побайтно.
#
# Рецепта здесь нарочно нет. Рецепт выражает лишь то, что умеет написавший его
# генератор, и молча перестаёт описывать плагин, как только в том появляется
# запись, которой генератор не знает, - продолжая выглядеть описанием.
#
# Двоичный .esp при этом остаётся в git рядом с исходником: его читает игра,
# и не у всякого, кто возьмёт модуль, есть эта цепочка инструментов.
param([switch]$FromPlugin)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cfg  = Get-Content -LiteralPath (Join-Path $root 'config\build.json') -Raw | ConvertFrom-Json
$r    = $cfg.esp
if (-not $r) { throw "в config\build.json нет раздела esp" }
if (-not (Test-Path -LiteralPath $cfg.spriggit)) {
    throw "Spriggit не найден: $($cfg.spriggit) - поправь ключ spriggit в config\build.json"
}

# PowerShell зовёт просто bash из system32 - это заглушка WSL, и сценарий с
# виндовыми путями она не выполнит. Нужен Git Bash, а он всегда лежит рядом
# с самим git, поэтому путь выводится, а не вписывается.
$git = (Get-Command git -ErrorAction SilentlyContinue).Source
if (-not $git) { throw "git не найден - без него не найти и Git Bash" }
$bash = Join-Path (Join-Path (Split-Path -Parent (Split-Path -Parent $git)) 'bin') 'bash.exe'
if (-not (Test-Path -LiteralPath $bash)) { throw "Git Bash не найден: $bash" }

# PowerShell 5.1 превращает каждую строку stderr родной программы в ошибку, и при
# 'Stop' это обрывает сценарий, хотя программа вернула ноль. Spriggit пишет в stderr
# обычный ход работы, поэтому на время вызова строгость снимается, а решает
# исключительно код возврата.
function Invoke-Spriggit {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $bash $cfg.spriggit @args 2>&1 | ForEach-Object { "$_" } | Out-Null
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $prev
    }
    if ($code -ne 0) { throw "Spriggit отказал, код $code" }
}

function Expand-Path([string]$p) { $p.Replace('{root}', $root) }
$source = Expand-Path $r.source
$plugin = Join-Path $root "esp\$($r.file)"

if ($FromPlugin) {
    if (-not (Test-Path -LiteralPath $plugin)) { throw "нет плагина: $plugin" }
    Invoke-Spriggit serialize --InputPath $plugin --OutputPath $source `
        --GameRelease $r.gameRelease --PackageName $r.package --PackageVersion $r.packageVersion
    "разобрано: $plugin -> $source"
    return
}

if (-not (Test-Path -LiteralPath $source)) { throw "нет исходника плагина: $source" }
Invoke-Spriggit deserialize --InputPath $source --OutputPath $plugin
"собрано: $source -> $plugin"
