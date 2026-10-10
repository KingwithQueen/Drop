$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$manifest = Get-Content (Join-Path $root 'vcpkg.json') -Raw | ConvertFrom-Json
$checkout = Join-Path $root 'third_party\vcpkg'
if (!(Test-Path (Join-Path $checkout '.git'))) {
    & git clone --no-checkout https://github.com/microsoft/vcpkg.git $checkout
    if ($LASTEXITCODE -ne 0) { throw 'Cannot download vcpkg.' }
}
& git -C $checkout fetch origin $manifest.'builtin-baseline' --depth=1
if ($LASTEXITCODE -ne 0) { throw 'Cannot fetch pinned baseline.' }
& git -C $checkout checkout --detach $manifest.'builtin-baseline'
if ($LASTEXITCODE -ne 0) { throw 'Cannot select pinned baseline.' }
& (Join-Path $checkout 'bootstrap-vcpkg.bat') -disableMetrics
if ($LASTEXITCODE -ne 0) { throw 'Cannot initialize vcpkg.' }
