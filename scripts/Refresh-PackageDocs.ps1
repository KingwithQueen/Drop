param(
    [Parameter(Mandatory=$true)][string]$Directory,
    [string]$RarPath=''
)
$ErrorActionPreference='Stop'
$root=[System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$destination=[System.IO.Path]::GetFullPath($Directory)
$packages=Join-Path $root 'build\packages'
if(!(Split-Path -Parent $destination).Equals($packages,[StringComparison]::OrdinalIgnoreCase) -or (Split-Path -Leaf $destination) -notmatch '^(Drop|Drop-Client|DropServer)-\d+\.\d+\.\d+-(x64|Win32)-\d{8}-\d{6}$') { throw 'Only an existing workspace package is supported.' }
if(!(Test-Path -LiteralPath (Join-Path $destination 'files.json'))) { throw 'Package is incomplete.' }
$client=(Split-Path -Leaf $destination).StartsWith('Drop-Client-',[StringComparison]::OrdinalIgnoreCase)
$server=(Split-Path -Leaf $destination).StartsWith('DropServer-',[StringComparison]::OrdinalIgnoreCase)
$exe=if($client) {'drop.exe'} else {'dropserver.exe'}
if(!(Test-Path -LiteralPath (Join-Path $destination $exe))) { throw 'Package executable is missing.' }
if(!$client) { Copy-Item -LiteralPath (Join-Path $root 'docs') -Destination $destination -Recurse -Force }
$readme=if($client) {'docs\CLIENT_DEPLOYMENT.md'} elseif($server) {'docs\SERVER_DEPLOYMENT.md'} else {'README.md'}
Copy-Item -LiteralPath (Join-Path $root $readme) -Destination (Join-Path $destination 'README.md') -Force
if($server) {
    $packageReadme=Join-Path $destination 'README.md'
    $content=[IO.File]::ReadAllText($packageReadme).Replace('(ACCOUNT_MANAGEMENT.md)','(docs/ACCOUNT_MANAGEMENT.md)')
    [IO.File]::WriteAllText($packageReadme,$content,[Text.UTF8Encoding]::new($true))
}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Setup.Common.ps1') -Destination $destination -Force
if(!$server) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Setup-Drop.ps1') -Destination $destination -Force }
if(!$client) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Setup-DropServer.ps1') -Destination $destination -Force }
if($client) {
    $notes=Join-Path $destination 'PACKAGE-NOTES.txt'
    $content=[IO.File]::ReadAllText($notes).Replace('Client data is created only when the application runs.','Optional setup creates the selected cache folder and shortcut; no login credentials are included.')
    [IO.File]::WriteAllText($notes,$content,[Text.UTF8Encoding]::new($true))
}
$manifest=@()
foreach($file in Get-ChildItem -LiteralPath $destination -File -Recurse | Where-Object {$_.FullName -ne (Join-Path $destination 'files.json')}) {
    $manifest+=[pscustomobject]@{path=$file.FullName.Substring($destination.Length+1);bytes=$file.Length;sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $destination 'files.json') -Encoding utf8
$rar="$destination.rar"
$format=if((Test-Path -LiteralPath $rar) -or !(Test-Path -LiteralPath "$destination.zip")) {'Rar'} else {'Zip'}
$archive=& (Join-Path $PSScriptRoot 'New-PackageArchive.ps1') -Directory $destination -Format $format -RarPath $RarPath -Force
Write-Output "Package documents, file manifest and archive hash updated: $archive"
