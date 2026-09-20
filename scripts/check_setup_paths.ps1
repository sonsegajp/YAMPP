# Exercise the setup script's actual path guards without running disc extraction.
$ErrorActionPreference = 'Stop'
$source = Join-Path (Split-Path -Parent $PSScriptRoot) 'dist\template\tools\setup.ps1'
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($source, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw ($errors | Out-String) }
$resolver = $ast.FindAll({param($n) $n -is [Management.Automation.Language.CommandAst] -and $n.GetCommandName() -eq 'Add-Type' -and $n.Extent.Text.Contains('MeleePathResolver')}, $true)
Invoke-Expression $resolver[0].Extent.Text
foreach ($name in @('Assert-SetupTree','Remove-SetupTree','Rename-SetupTree')) {
    $definition = $ast.FindAll({param($n) $n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq $name}, $true)
    Invoke-Expression $definition[0].Extent.Text
}
$fixture = Join-Path (Split-Path -Parent $PSScriptRoot) ('build\setup-path-check-' + [Guid]::NewGuid().ToString('N'))
$root = Join-Path $fixture 'release'
$outside = Join-Path $fixture 'unrelated'
$null = New-Item -ItemType Directory -Path $root,$outside
$null = New-Item -ItemType Directory -Path (Join-Path $root 'data\GALE01')
Set-Content -LiteralPath (Join-Path $outside 'sentinel.txt') -Value 'preserved'
$checks = [ordered]@{parser=$true}
function Must-Reject($label, $path) {
    $rejected = $false
    try { Assert-SetupTree $path } catch { $rejected = $true }
    if (-not $rejected) { throw "Did not reject $label" }
    $checks[$label] = $true
}
Assert-SetupTree (Join-Path $root 'data\GALE01')
$checks['ordinaryTree'] = $true
Must-Reject 'outsideRoot' $outside
Must-Reject 'parentDirectory' (Join-Path $root 'data')
Must-Reject 'unrelatedLeaf' (Join-Path $root 'data\Other')
$linked = Join-Path $root 'data\GALE01\linked'
$null = New-Item -ItemType Junction -Path $linked -Target $outside
Must-Reject 'nestedJunction' (Join-Path $root 'data\GALE01')
# Keep the rejected tree intact. A separate safe tree tests actual promotion/removal.
$root = Join-Path $fixture 'ordinary-release'
$null = New-Item -ItemType Directory -Path (Join-Path $root 'data\GALE01.staging\sys')
Set-Content -LiteralPath (Join-Path $root 'data\GALE01.staging\sys\fixture.txt') -Value 'new'
Rename-SetupTree (Join-Path $root 'data\GALE01.staging') 'GALE01'
$checks['promotion'] = Test-Path -LiteralPath (Join-Path $root 'data\GALE01\sys\fixture.txt')
Rename-SetupTree (Join-Path $root 'data\GALE01') 'GALE01.old'
Remove-SetupTree (Join-Path $root 'data\GALE01.old')
$checks['boundedRemoval'] = -not (Test-Path -LiteralPath (Join-Path $root 'data\GALE01.old'))
$checks['unrelatedPreserved'] = (Get-Content -LiteralPath (Join-Path $outside 'sentinel.txt')).Trim() -eq 'preserved'
$root = Join-Path $fixture 'linked-data-release'
$null = New-Item -ItemType Directory -Path $root
$null = New-Item -ItemType Junction -Path (Join-Path $root 'data') -Target $outside
Must-Reject 'linkedDataDirectory' (Join-Path $root 'data\GALE01')
$checks | ConvertTo-Json
if ($checks.Values -contains $false) { throw 'Setup path check failed' }
