$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
[xml]$projectXml = Get-Content (Join-Path $projectRoot 'config/project.xml') -Raw
foreach ($spec in $projectXml.'melee-project'.dependencies.dependency) {
    $name = $spec.name
    $checkout = Join-Path $projectRoot "upstream/$name"
    if (-not (Test-Path -LiteralPath $checkout)) {
        git clone --no-checkout --filter=blob:none $spec.url $checkout
        if ($LASTEXITCODE) { throw "Clone failed: $name" }
        git -C $checkout fetch --depth=1 origin $spec.commit
        if ($LASTEXITCODE) { throw "Fetch failed: $name" }
        git -C $checkout checkout --detach $spec.commit
        if ($LASTEXITCODE) { throw "Checkout failed: $name" }
    }
    $revision = git -C $checkout rev-parse HEAD
    if ($LASTEXITCODE -or $revision -ne $spec.commit) {
        throw "$name checkout is not the pinned revision; preserve local work and inspect it."
    }
}

# Preserve the two reviewed compiler fixes on the exact pinned HSDLib sources.
python (Join-Path $projectRoot "scripts/patch_hsdlib.py")
if ($LASTEXITCODE) { throw "HSDLib source patch failed; preserve local changes and inspect the reported files." }

if (Test-Path (Join-Path $projectRoot "upstream/MexManager/mexLib/MexWorkspace.cs")) {
    python (Join-Path $projectRoot "scripts/patch_mex_disc.py")
    if ($LASTEXITCODE) { throw "m-ex disc patch failed; preserve local changes and inspect the reported files." }
}
