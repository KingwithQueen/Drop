param([Parameter(Mandatory=$true)][string]$Directory)
$ErrorActionPreference='Stop'
$root=[System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$destination=[System.IO.Path]::GetFullPath($Directory)
$packages=Join-Path $root 'build\packages'
if(!(Split-Path -Parent $destination).Equals($packages,[StringComparison]::OrdinalIgnoreCase) -or (Split-Path -Leaf $destination) -notmatch '^Drop-0\.1\.0-(x64|Win32)-\d{8}-\d{6}$') { throw 'Only an existing workspace package is supported.' }
if(!(Test-Path -LiteralPath (Join-Path $destination 'files.json')) -or !(Test-Path -LiteralPath (Join-Path $destination 'dropserver.exe'))) { throw 'Package is incomplete.' }
Copy-Item -LiteralPath (Join-Path $root 'docs') -Destination $destination -Recurse -Force
Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination $destination -Force
$manifest=@()
foreach($file in Get-ChildItem -LiteralPath $destination -File -Recurse | Where-Object {$_.FullName -ne (Join-Path $destination 'files.json')}) {
    $manifest+=[pscustomobject]@{path=$file.FullName.Substring($destination.Length+1);bytes=$file.Length;sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $destination 'files.json') -Encoding utf8
$zip="$destination.zip"
Compress-Archive -LiteralPath $destination -DestinationPath $zip -Force
(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant() | Set-Content -LiteralPath "$zip.sha256" -Encoding ascii
Write-Output "Package documents, file manifest and archive hash updated: $zip"
