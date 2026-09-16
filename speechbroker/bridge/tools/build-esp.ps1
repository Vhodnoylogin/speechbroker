# Building the plugin out of the recipe in config\build.json.
#
#   tools\build-esp.ps1     rebuild esp\<name> from scratch
#
# The plugin used to be a binary in git that nobody could reproduce: it had been
# made by hand once, and the names of the scripts live INSIDE it, so renaming one
# meant editing bytes. It is built here instead, by the AutoMod CLI the rest of
# the project uses. The binary is still committed - the game needs it and not
# everybody has the toolchain - but it is now a product and not a relic.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cfg  = Get-Content -LiteralPath (Join-Path $root 'config\build.json') -Raw | ConvertFrom-Json
$r    = $cfg.esp
if (-not $r) { throw "config\build.json holds no esp recipe" }

$cli = $cfg.automod
if (-not (Test-Path -LiteralPath $cli)) {
    throw "AutoMod CLI not found: $cli - correct the automod key of config\build.json"
}
function Invoke-Automod {
    dotnet $cli @args
    if ($LASTEXITCODE -ne 0) { throw "automod refused: $($args -join ' ')" }
}

$out = Join-Path $root 'esp'
New-Item -ItemType Directory -Force $out | Out-Null
$plugin = Join-Path $out $r.file
Remove-Item -LiteralPath $plugin -Force -ErrorAction SilentlyContinue

Invoke-Automod esp create $r.file -o $out --author $r.author --description $r.description
foreach ($q in $r.quests) {
    $a = @('esp', 'add-quest', $plugin, $q.editorId, '--name', $q.name,
           '--priority', $q.priority)
    if ($q.startEnabled) { $a += '--start-enabled' }
    Invoke-Automod @a
    Invoke-Automod esp attach-script $plugin --quest $q.editorId --script $q.script
}

Invoke-Automod esp info $plugin

