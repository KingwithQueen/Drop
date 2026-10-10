param(
    [ValidateSet('x64','Win32')][string]$Platform = 'x64',
    [ValidateSet('Debug','Release')][string]$Configuration = 'Debug'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
& python (Join-Path $root 'tests\integration.py') --platform $Platform --configuration $Configuration
if ($LASTEXITCODE -ne 0) { throw 'Integration verification failed.' }
