param([string]$Disc, [switch]$Audit)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
Set-Location $projectRoot
& (Join-Path $PSScriptRoot 'bootstrap.ps1')
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'Visual Studio C++ tools are required.' }
$cmake = Join-Path $vsPath 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$clang = Join-Path $vsPath 'VC/Tools/Llvm/x64/bin/clang.exe'
if (-not (Test-Path -LiteralPath $clang)) { throw 'Install the Visual Studio Clang tools component.' }
if ($Disc) {
    python scripts/extract_disc.py $Disc
    if ($LASTEXITCODE) { throw 'Disc validation or extraction failed.' }
}
if (-not (Test-Path data/GALE01/sys/main.dol)) {
    throw 'First run requires -Disc <path to Melee USA 1.02 ISO>.'
}
python scripts/extract_embedded.py
if ($LASTEXITCODE) { throw 'Embedded data extraction failed.' }
function Invoke-NativeLogged {
    param([string]$Program, [string[]]$Arguments, [string]$Log)
    if (-not (Test-Path -LiteralPath $Program)) { throw "Tool missing: $Program" }
    $oldPreference = $ErrorActionPreference
    try {
        # Windows PowerShell treats native stderr text as ErrorRecords.
        # Judge native tools by their exit status, not their choice of stream.
        $ErrorActionPreference = 'Continue'
        & $Program @Arguments *> $Log
        $processCode = $LASTEXITCODE
    } finally { $ErrorActionPreference = $oldPreference }
    if ($processCode -ne 0) {
        Get-Content $Log -Tail 40
        throw "Native tool failed with exit code $processCode. See $Log"
    }
}
New-Item -ItemType Directory -Force -Path build | Out-Null
Invoke-NativeLogged -Program $cmake -Arguments @('-S', '.', '-B', 'build/pc', '-G', 'Visual Studio 17 2022', '-A', 'x64', "-DMELEE_CLANG_EXECUTABLE=$clang", '-DAURORA_DAWN_PROVIDER=package') -Log build/configure.log
Invoke-NativeLogged -Program $cmake -Arguments @('--build', 'build/pc', '--config', 'Release', '--target', 'melee_renderer_check', '--parallel', '12') -Log build/renderer-build.log
Write-Host 'Built build/pc/bin/melee_renderer_check.exe (renderer verification; game boot is not implemented).'
if ($Audit) {
    python scripts/audit_native.py --clang $clang --jobs 12
    if ($LASTEXITCODE) { throw 'Native game source gate is incomplete. See build/native-audit/report.json.' }
}
