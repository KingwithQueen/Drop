param([ValidateSet('Debug','Release')][string]$Configuration='Debug')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $root
$env:DROP_SERVICE_POWERSHELL=(Get-Process -Id $PID).Path
& python (Join-Path $root 'tests\service.py') --configuration $Configuration *> (Join-Path $root "build\logs\service-$Configuration.log")
if($LASTEXITCODE -ne 0) { throw 'Isolated service lifecycle verification failed; see build/logs.' }
