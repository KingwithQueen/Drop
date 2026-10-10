param([ValidateSet('x64','Win32')][string]$Platform='x64')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
& (Join-Path $PSScriptRoot 'Build.ps1') -Platform $Platform -Configuration Release -Production
$triplet=if($Platform -eq 'x64') {'x64-windows'} else {'x86-windows'}
$dependencies=Join-Path $root "third_party\installed-$Platform\$triplet"
$binary=Join-Path $root "build\production\$Platform"
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$dumpbin=& $vswhere -latest -products * -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe' | Select-Object -First 1
if(!$dumpbin) { throw 'Dependency inspection tool unavailable.' }
$stamp=Get-Date -Format yyyyMMdd-HHmmss
$destination=Join-Path $root "build\packages\Drop-0.1.0-$Platform-$stamp"
if(Test-Path -LiteralPath $destination) { throw 'Package output already exists.' }
New-Item -ItemType Directory -Path $destination | Out-Null
$queue=[System.Collections.Generic.Queue[string]]::new()
$copied=[System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach($name in @('drop.exe','dropserver.exe')) { $queue.Enqueue((Join-Path $binary $name)) }
foreach($file in Get-ChildItem -LiteralPath (Join-Path $dependencies 'bin') -File | Where-Object {$_.Name -match '^lib(ssl|crypto)-.*\.dll$'}) { $queue.Enqueue($file.FullName) }
while($queue.Count) {
    $source=$queue.Dequeue()
    $name=Split-Path -Leaf $source
    if(!$copied.Add($name)) { continue }
    Copy-Item -LiteralPath $source -Destination (Join-Path $destination $name)
    $imports=& $dumpbin /nologo /dependents $source
    if($LASTEXITCODE -ne 0) { throw "Cannot inspect $name" }
    foreach($line in $imports) {
        if($line -match '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') {
            $dependency=Join-Path $dependencies "bin\$($Matches[1])"
            if(Test-Path -LiteralPath $dependency) { $queue.Enqueue($dependency) }
        }
    }
}
# Third-party licences include transitive runtime sources; build tools are not copied.
$licences=Join-Path $destination 'licenses'
New-Item -ItemType Directory -Path $licences | Out-Null
foreach($name in @('cjson','civetweb','openssl','sqlcipher','sqlite3','libsodium','zstd','zlib','tcl','libtommath','minizip')) {
    $source=Join-Path $dependencies "share\$name\copyright"
    if(Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination (Join-Path $licences "$name.txt") }
}
Copy-Item -LiteralPath (Join-Path $root 'LICENSE.txt') -Destination $destination
Copy-Item -LiteralPath (Join-Path $root 'README.md') -Destination $destination
Copy-Item -LiteralPath (Join-Path $root 'docs') -Destination $destination -Recurse
Copy-Item -LiteralPath (Join-Path $root 'config') -Destination $destination -Recurse
@'
Development candidate. Read docs/DEVELOPMENT_STATUS.md and docs/VERIFICATION.md for actual acceptance limits.
Requires the Microsoft Visual C++ runtime matching v145 and the selected architecture. Install the official redistributable if unavailable.
No test account, password, certificate, server data, token, recovery package, debugging symbols or build tool is included.
The repository MIT licence still has original author/year placeholders; resolve the owner attribution before public redistribution.
'@ | Set-Content -LiteralPath (Join-Path $destination 'PACKAGE-NOTES.txt') -Encoding utf8
$manifest=@()
foreach($file in Get-ChildItem -LiteralPath $destination -File -Recurse) {
    $manifest+=[pscustomobject]@{path=$file.FullName.Substring($destination.Length+1);bytes=$file.Length;sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $destination 'files.json') -Encoding utf8
$zip="$destination.zip"
Compress-Archive -LiteralPath $destination -DestinationPath $zip
(Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant() | Set-Content -LiteralPath "$zip.sha256" -Encoding ascii
Write-Output "Reviewable package: $zip"
