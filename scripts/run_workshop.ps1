$ErrorActionPreference = 'Stop'
python (Join-Path $PSScriptRoot 'project_config.py') --launch workshop
if ($LASTEXITCODE) { throw 'Workshop startup failed. See the message above.' }
