param(
    [ValidateSet('x64','Win32')][string]$Platform = 'x64',
    [ValidateSet('Debug','Release')][string]$Configuration = 'Debug',
    [switch]$Dependencies,
    [switch]$Test
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$triplet = if ($Platform -eq 'x64') { 'x64-windows' } else { 'x86-windows' }
$vcpkg = Join-Path $root 'third_party\vcpkg\vcpkg.exe'
$baseline = (Get-Content (Join-Path $root 'vcpkg.json') -Raw | ConvertFrom-Json).'builtin-baseline'
if ($Dependencies) {
    if (!(Test-Path $vcpkg)) { throw 'Initialize third_party/vcpkg with scripts/Bootstrap.ps1 first.' }
    $actual = & git -C (Join-Path $root 'third_party\vcpkg') rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $actual -ne $baseline) { throw 'vcpkg checkout differs from the pinned baseline.' }
    $env:VCPKG_DISABLE_METRICS = '1'
    # Keep generated tools, caches and installed packages inside the project.
    $env:VCPKG_DEFAULT_BINARY_CACHE = Join-Path $root 'build\dependency-cache'
    New-Item -ItemType Directory -Force $env:VCPKG_DEFAULT_BINARY_CACHE | Out-Null
    & $vcpkg install "--triplet=$triplet" "--x-manifest-root=$root" "--x-install-root=$root\third_party\installed" --disable-metrics
    if ($LASTEXITCODE -ne 0) { throw 'Dependency build failed.' }
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (!$msbuild) { throw 'Visual Studio C build tools were not found.' }
New-Item -ItemType Directory -Force (Join-Path $root 'build\logs') | Out-Null
& $msbuild (Join-Path $root 'drop.slnx') /m /nologo "/p:Configuration=$Configuration" "/p:Platform=$Platform" "/flp:logfile=$root\build\logs\$Platform-$Configuration.log;verbosity=normal" /verbosity:minimal
if ($LASTEXITCODE -ne 0) { throw 'Solution build failed.' }
if ($Test) {
    $testRoot = Join-Path $root "build\tests\$Platform-$Configuration"
    New-Item -ItemType Directory -Force $testRoot | Out-Null
    & (Join-Path $root "build\bin\$Platform\$Configuration\drop.exe") --self-test $testRoot
    if ($LASTEXITCODE -ne 0) { throw 'Client/common verification failed.' }
    & (Join-Path $root "build\bin\$Platform\$Configuration\dropserver.exe") --self-test $testRoot
    if ($LASTEXITCODE -ne 0) { throw 'Encrypted storage verification failed.' }
}
