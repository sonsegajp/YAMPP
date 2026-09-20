$ErrorActionPreference = 'Stop'
Set-Location (Split-Path -Parent $PSScriptRoot)
python scripts/project_config.py --launch game --development --runtime-profile config/diddy-test.xml
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
