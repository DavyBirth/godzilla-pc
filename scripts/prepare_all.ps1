# M1: merge the base game with the 01.01 update and prepare the loader files.
# Only reads the original game folders; everything is written under $Work (outside the repo).
param(
    [string]$Base  = '',
    [string]$Patch = '',
    [string]$Work  = '',
    [string]$Firmware = ''
)
$ErrorActionPreference = 'Stop'
# Machine-specific defaults live in an untracked scripts\local-paths.ps1.
$local = Join-Path $PSScriptRoot 'local-paths.ps1'
if (Test-Path $local) { . $local }
if (-not $Base)  { $Base  = $LocalBase }
if (-not $Patch) { $Patch = $LocalPatch }
if (-not $Work)  { $Work  = if ($LocalWork) { $LocalWork } else { Join-Path (Split-Path $PSScriptRoot) 'game' } }
if (-not $Firmware) { $Firmware = $LocalFirmware }
if (-not $Base -or -not $Patch -or -not $Firmware) { throw 'Pass -Base <game folder>, -Patch <01.01 update folder> and -Firmware <sys_modules folder>' }
$scripts = $PSScriptRoot
$game = Join-Path $Work 'CUSA02046'
$out = Join-Path $Work 'out'
foreach ($step in @(
        @('merge_game.py', $Base, $Patch, $game),
        @('prepare.py', $game, '--out', $out),
        @('link_modules.py', $game, '--out', $out, '--firmware', $Firmware),
        @('content_profile.py', $game, '--out', $out))) {
    python (Join-Path $scripts $step[0]) @($step[1..($step.Count - 1)])
    if ($LASTEXITCODE) { throw "$($step[0]) failed (exit $LASTEXITCODE)" }
}
