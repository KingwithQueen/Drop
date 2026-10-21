param(
    [ValidateSet('x64','Win32')][string]$Platform='x64',
    [ValidateSet('All','Client','Server')][string]$Component='All',
    [ValidateSet('Rar','Zip')][string]$ArchiveFormat='Rar',
    [string]$RarPath=''
)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$versionHeader=Get-Content -LiteralPath (Join-Path $root 'dropcommon\version.h') -Raw
if($versionHeader -notmatch '(?m)^#define DROP_VERSION "(\d+\.\d+\.\d+)"\s*$') { throw 'Product version is missing or invalid.' }
$productVersion=$Matches[1]
& (Join-Path $PSScriptRoot 'Build.ps1') -Platform $Platform -Configuration Release -Production
$triplet=if($Platform -eq 'x64') {'x64-windows'} else {'x86-windows'}
$dependencies=Join-Path $root "third_party\installed-$Platform\$triplet"
$binary=Join-Path $root "build\production\$Platform"
$vswhere="${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$dumpbin=& $vswhere -latest -products * -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe' | Select-Object -First 1
if(!$dumpbin) { throw 'Dependency inspection tool unavailable.' }
$stamp=Get-Date -Format yyyyMMdd-HHmmss
$packageName=switch($Component) {
    'Client' {"Drop-Client-$productVersion-$Platform-$stamp"}
    'Server' {"DropServer-$productVersion-$Platform-$stamp"}
    default {"Drop-$productVersion-$Platform-$stamp"}
}
$destination=Join-Path $root "build\packages\$packageName"
if(Test-Path -LiteralPath $destination) { throw 'Package output already exists.' }
New-Item -ItemType Directory -Path $destination | Out-Null
$queue=[System.Collections.Generic.Queue[string]]::new()
$copied=[System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
$executables=switch($Component) {
    'Client' {@('drop.exe')}
    'Server' {@('dropserver.exe')}
    default {@('drop.exe','dropserver.exe')}
}
foreach($name in $executables) { $queue.Enqueue((Join-Path $binary $name)) }
if($Component -ne 'Client') {
    foreach($file in Get-ChildItem -LiteralPath (Join-Path $dependencies 'bin') -File | Where-Object {$_.Name -match '^lib(ssl|crypto)-.*\.dll$'}) { $queue.Enqueue($file.FullName) }
}
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
Copy-Item -LiteralPath (Join-Path $root 'vendor\csrp\LICENSE') -Destination (Join-Path $licences 'csrp.txt')
Copy-Item -LiteralPath (Join-Path $root 'vendor\csrp\PROVENANCE.md') -Destination (Join-Path $licences 'csrp-source.md')
$licenceNames=if($Component -eq 'Client') {@('cjson','openssl','sqlite3','libsodium','zstd','zlib')} else {@('cjson','civetweb','openssl','sqlcipher','sqlite3','libsodium','zstd','zlib','tcl','libtommath','minizip')}
foreach($name in $licenceNames) {
    $source=Join-Path $dependencies "share\$name\copyright"
    if(Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination (Join-Path $licences "$name.txt") }
}
Copy-Item -LiteralPath (Join-Path $root 'LICENSE.txt') -Destination $destination
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Setup.Common.ps1') -Destination $destination
if($Component -ne 'Server') { Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Setup-Drop.ps1') -Destination $destination }
if($Component -ne 'Client') { Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Setup-DropServer.ps1') -Destination $destination }
if($Component -eq 'Client') {
    Copy-Item -LiteralPath (Join-Path $root 'docs\CLIENT_DEPLOYMENT.md') -Destination (Join-Path $destination 'README.md')
    @(
        "Drop client $productVersion ($Platform). Read README.md to configure your server connection.",
        'Requires the Microsoft Visual C++ runtime matching v145 and the selected architecture.',
        'Runtime DLLs and third-party licences are included. Optional setup creates the selected cache folder and shortcut; no login credentials are included.'
    ) | Set-Content -LiteralPath (Join-Path $destination 'PACKAGE-NOTES.txt') -Encoding utf8
} else {
    $readme=if($Component -eq 'Server') {'docs\SERVER_DEPLOYMENT.md'} else {'README.md'}
    Copy-Item -LiteralPath (Join-Path $root $readme) -Destination (Join-Path $destination 'README.md')
    if($Component -eq 'Server') {
        $packageReadme=Join-Path $destination 'README.md'
        $content=[IO.File]::ReadAllText($packageReadme).Replace('(ACCOUNT_MANAGEMENT.md)','(docs/ACCOUNT_MANAGEMENT.md)')
        [IO.File]::WriteAllText($packageReadme,$content,[Text.UTF8Encoding]::new($true))
    }
    # 发布文档只取维护中的 Markdown 文件，不能递归带入本机设计稿或验收材料。
    $packageDocs=Join-Path $destination 'docs'
    New-Item -ItemType Directory -Path $packageDocs | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $root 'docs') -File -Filter '*.md' | Copy-Item -Destination $packageDocs
    Copy-Item -LiteralPath (Join-Path $root 'config') -Destination $destination -Recurse
@'
Development candidate. Read docs/DEVELOPMENT_STATUS.md and docs/VERIFICATION.md for actual acceptance limits.
Requires the Microsoft Visual C++ runtime matching v145 and the selected architecture. Install the official redistributable if unavailable.
No test account, password, certificate, server data, token, recovery package, debugging symbols or build tool is included.
The repository MIT licence still has original author/year placeholders; resolve the owner attribution before public redistribution.
'@ | Set-Content -LiteralPath (Join-Path $destination 'PACKAGE-NOTES.txt') -Encoding utf8
}
$manifest=@()
foreach($file in Get-ChildItem -LiteralPath $destination -File -Recurse) {
    $manifest+=[pscustomobject]@{path=$file.FullName.Substring($destination.Length+1);bytes=$file.Length;sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $destination 'files.json') -Encoding utf8
$archive=& (Join-Path $PSScriptRoot 'New-PackageArchive.ps1') -Directory $destination -Format $ArchiveFormat -RarPath $RarPath
Write-Output "Reviewable package: $archive"
