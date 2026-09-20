# First run: take a Melee disc image, check it is the right one, and unpack
# everything YAMPP and the Workshop need out of it. Nothing from the game is
# shipped with this release, so this has to happen on the player's machine.
#
# Both plain images (.iso/.gcm) and Dolphin's compressed ones (.rvz/.wia) work.
# The unpacking is done by the game executable, which carries the decompressor;
# this script only drives it and checks the result.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$expectedDol = '08e0bf20134dfcb260699671004527b2d6bb1a45'

function Say($text, $colour = 'Gray') { Write-Host $text -ForegroundColor $colour }
function Fail($text) { Say ''; Say $text 'Red'; Say ''; Read-Host 'Press Enter to close'; exit 1 }

Say ''
Say '  YAMPP - first run setup' 'Cyan'
Say '  ---------------------------------------------------------------'
Say '  A Super Smash Bros. Melee disc image is needed. Nothing from the'
Say '  game ships with this release, so your own copy supplies it.'
Say ''
Say '  Wanted: NTSC-U version 1.02   (.iso, .gcm, .rvz or .wia)'
Say ''

$iso = $args | Select-Object -First 1
if (-not $iso) {
    Say '  Choose your Melee disc image...' 'Yellow'
    Add-Type -AssemblyName System.Windows.Forms
    $picker = New-Object System.Windows.Forms.OpenFileDialog
    $picker.Title = 'Select your Super Smash Bros. Melee disc image'
    $picker.Filter = 'GameCube disc image (*.iso;*.gcm;*.rvz;*.wia)|*.iso;*.gcm;*.rvz;*.wia|All files (*.*)|*.*'
    if ($picker.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { Fail '  No disc image chosen. Nothing was changed.' }
    $iso = $picker.FileName
}
if (-not (Test-Path -LiteralPath $iso)) { Fail "  That file does not exist: $iso" }
$iso = (Resolve-Path -LiteralPath $iso).Path
Say "  Using: $iso"

$magic = New-Object byte[] 4
$probe = [System.IO.File]::OpenRead($iso)
try { $null = $probe.Read($magic, 0, 4) } finally { $probe.Dispose() }
$compressed = ($magic[0] -eq 0x52 -and $magic[1] -eq 0x56 -and $magic[2] -eq 0x5A) -or
              ($magic[0] -eq 0x57 -and $magic[1] -eq 0x49 -and $magic[2] -eq 0x41)
if ($compressed) { Say '  Compressed image - it is expanded as it is read.' }
Say ''

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class MeleePathResolver {
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern IntPtr CreateFileW(string name, uint access, uint share,
        IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)]
    static extern uint GetFinalPathNameByHandleW(IntPtr handle,
        StringBuilder buf, uint cap, uint flags);
    [DllImport("kernel32.dll", SetLastError=true)]
    static extern bool CloseHandle(IntPtr handle);
    public static string Resolve(string path) {
        IntPtr h = CreateFileW(path, 0, 7, IntPtr.Zero, 3, 0x02000000, IntPtr.Zero);
        if (h == new IntPtr(-1)) return null;
        StringBuilder sb = new StringBuilder(1024);
        uint n = GetFinalPathNameByHandleW(h, sb, 1024, 0);
        CloseHandle(h);
        if (n == 0 || n >= 1024) return null;
        string r = sb.ToString();
        if (r.StartsWith("\\\\?\\")) r = r.Substring(4);
        return r;
    }
}
"@ -ErrorAction SilentlyContinue
# Resolve and validate every directory we may replace or recursively remove.
# Refuse junctions/symlinks, including links buried inside a previous install.
function Assert-SetupTree([string]$path) {
    $full = [IO.Path]::GetFullPath($path).TrimEnd('\')
    $base = [IO.Path]::GetFullPath((Join-Path $root 'data')).TrimEnd('\')
    if ([IO.Path]::GetDirectoryName($full) -ne $base -or
        [IO.Path]::GetFileName($full) -notin @('GALE01', 'GALE01.staging', 'GALE01.old')) {
        throw 'Unsafe setup directory'
    }
    $resolvedRoot = [MeleePathResolver]::Resolve($root)
    if (-not $resolvedRoot) { throw 'Could not resolve the release directory' }
    if (Test-Path -LiteralPath $base) {
        $resolvedBase = [MeleePathResolver]::Resolve($base)
        if (-not $resolvedBase -or [IO.Path]::GetDirectoryName($resolvedBase.TrimEnd('\')) -ne $resolvedRoot.TrimEnd('\') -or
            ((Get-Item -LiteralPath $base -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'The data directory must be an ordinary folder inside this release'
        }
    }
    if (Test-Path -LiteralPath $full) {
        $pending = New-Object 'System.Collections.Generic.Stack[string]'
        $pending.Push($full)
        while ($pending.Count) {
            $item = Get-Item -LiteralPath $pending.Pop() -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Setup cannot replace linked files or directories' }
            if ($item.PSIsContainer) {
                foreach ($child in (Get-ChildItem -LiteralPath $item.FullName -Force)) { $pending.Push($child.FullName) }
            }
        }
    }
}
function Remove-SetupTree([string]$path) {
    Assert-SetupTree $path
    if (Test-Path -LiteralPath $path) { Remove-Item -Recurse -Force -LiteralPath $path }
}
function Rename-SetupTree([string]$path, [string]$name) {
    Assert-SetupTree $path
    Assert-SetupTree (Join-Path ([IO.Path]::GetDirectoryName($path)) $name)
    Rename-Item -LiteralPath $path -NewName $name
}
$dataDir = Join-Path $root 'data'
if (Test-Path -LiteralPath $dataDir) {
    $resolvedData = [MeleePathResolver]::Resolve($dataDir)
    $resolvedIso = [MeleePathResolver]::Resolve($iso)
    if (-not $resolvedData -or -not $resolvedIso) { Fail '  Could not resolve the disc and data paths safely.' }
    $resolvedData = $resolvedData.TrimEnd('\') + '\'
    if ($resolvedIso.StartsWith($resolvedData, [System.StringComparison]::OrdinalIgnoreCase)) {
        Fail '  The disc image is inside the data directory and would be deleted during setup. Choose one stored elsewhere.'
    }
}

$exe = Join-Path $root 'bin\YAMPP.exe'
if (-not (Test-Path -LiteralPath $exe)) { Fail "  This release is incomplete: $exe is missing." }

Assert-SetupTree (Join-Path $root 'data\GALE01')
$null = New-Item -ItemType Directory -Force -Path (Join-Path $root 'data')
$lockPath = Join-Path $root 'data\.setup.lock'
try { $script:setupLock = [System.IO.File]::Open($lockPath, 'OpenOrCreate', 'ReadWrite', 'None') }
catch { Fail '  Another setup process is already running. Close it first.' }

$assets = Join-Path $root 'data\GALE01'
$staging = Join-Path $root 'data\GALE01.staging'
$backup = Join-Path $root 'data\GALE01.old'
if (-not (Test-Path -LiteralPath $assets) -and (Test-Path -LiteralPath $backup)) {
    Say '  Recovering from interrupted previous setup...' 'Yellow'
    Rename-SetupTree $backup 'GALE01'
}
if (Test-Path -LiteralPath $staging) { Remove-SetupTree $staging }
Say '  Unpacking the disc...' 'Yellow'
& $exe '--extract' $iso $staging
if ($LASTEXITCODE -ne 0) {
    if (Test-Path -LiteralPath $staging) { Remove-SetupTree $staging }
    Fail '  The disc image could not be unpacked. See the message above.'
}

$dol = Join-Path $staging 'sys\main.dol'
if (-not (Test-Path -LiteralPath $dol)) {
    Remove-SetupTree $staging
    Fail '  Unpacking did not produce the game executable.'
}
$dolHash = (Get-FileHash -Algorithm SHA1 -LiteralPath $dol).Hash.ToLower()
if ($dolHash -ne $expectedDol) {
    Remove-SetupTree $staging
    Say '  That disc is not NTSC-U 1.02. YAMPP is built against that one' 'Red'
    Say '  version only, so another will not run.' 'Red'
    Fail "  (expected $expectedDol, found $dolHash)"
}
Say '  NTSC-U 1.02 confirmed.' 'Green'
if (Test-Path -LiteralPath $backup) { Remove-SetupTree $backup }
if (Test-Path -LiteralPath $assets) { Rename-SetupTree $assets 'GALE01.old' }
try {
    Rename-SetupTree $staging 'GALE01'
} catch {
    Say '  Promoting the new assets failed; restoring the previous installation.' 'Red'
    if (Test-Path -LiteralPath $backup) { Rename-SetupTree $backup 'GALE01' }
    if (Test-Path -LiteralPath $staging) { Remove-SetupTree $staging }
    Fail "  Setup could not replace the asset directory: $_"
}

# Cleanup failure after promotion must not roll back an already valid install.
if (Test-Path -LiteralPath $backup) {
    try { Remove-SetupTree $backup }
    catch { Say '  The new assets are ready; the previous copy remains in data\GALE01.old.' 'Yellow' }
}

# Remember the disc and lay down the folders the game writes to.
$user = Join-Path $root 'user'
foreach ($folder in @('saves', 'mods\music\stage', 'cache')) {
    $null = New-Item -ItemType Directory -Force -Path (Join-Path $user $folder)
}
$discXml = New-Object System.Xml.XmlDocument
$discNode = $discXml.CreateElement('disc')
$discNode.SetAttribute('schema', '1')
$discNode.SetAttribute('path', $iso)
$null = $discXml.AppendChild($discNode)
$discXml.Save((Join-Path $user 'disc.xml'))
# The launcher is a batch file and reads the path from here rather than parsing XML.
[System.IO.File]::WriteAllText((Join-Path $user 'disc.path'), $iso)
$settings = Join-Path $user 'settings.xml'
if (-not (Test-Path -LiteralPath $settings)) {
    "<?xml version=""1.0"" encoding=""utf-8""?>`r`n<melee-settings schema=""1"" width=""1280"" height=""720"" renderScale=""1"" widescreen=""1"" fullscreen=""0"" vsync=""1"" volume=""100"" mute=""0"" showFps=""0"" netplayServer="""" netplayName=""Player"" />" |
        Set-Content -Encoding utf8 $settings
}

Say ''
Say '  Setup complete.' 'Green'
Say ''
Say '  Play YAMPP.cmd   - start the game'
Say '  Melee Workshop.cmd  - open the Workshop'
Say ''
Say '  Custom stage music goes in user\mods\music\stage\<stage name>\.'
Say ''
if ($script:setupLock) { $script:setupLock.Close(); Remove-Item -LiteralPath $lockPath -ErrorAction SilentlyContinue }
