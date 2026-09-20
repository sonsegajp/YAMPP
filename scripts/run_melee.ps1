param([switch]$Development)
$ErrorActionPreference = 'Stop'
$launchArgs = @('--launch', 'game')
if ($Development) { $launchArgs += '--development' }
python (Join-Path $PSScriptRoot 'project_config.py') @launchArgs
if ($LASTEXITCODE) { throw 'Melee startup failed. See the message above.' }
