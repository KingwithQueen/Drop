param(
    [Parameter(Mandatory=$true)][string]$Directory,
    [ValidateSet('Rar','Zip')][string]$Format='Rar',
    [string]$RarPath='',
    [switch]$Force
)
$ErrorActionPreference='Stop'
$workspace=[System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$directory=[System.IO.Path]::GetFullPath($Directory)
$packages=Join-Path $workspace 'build\packages'
if(!(Split-Path -Parent $directory).Equals($packages,[StringComparison]::OrdinalIgnoreCase) -or !(Test-Path -LiteralPath (Join-Path $directory 'files.json'))) { throw 'Only a completed workspace package can be archived.' }
$archive="$directory.$($Format.ToLowerInvariant())"
if(Test-Path -LiteralPath $archive) {
    if(!$Force) { throw 'Archive output already exists.' }
    Remove-Item -LiteralPath $archive -Force
}
if($Format -eq 'Zip') {
    Compress-Archive -LiteralPath $directory -DestinationPath $archive
    $settings=[ordered]@{format='ZIP';archive=$archive}
} else {
    if(!$RarPath) {
        $command=Get-Command rar.exe -ErrorAction SilentlyContinue
        if($command) { $RarPath=$command.Source }
        else {
            $candidates=@(
                (Join-Path $env:ProgramFiles 'WinRAR\Rar.exe'),
                (Join-Path (Split-Path -Qualifier $workspace) 'Program Files\WinRAR\Rar.exe')
            )
            $RarPath=$candidates | Where-Object {Test-Path -LiteralPath $_} | Select-Object -First 1
        }
    }
    if(!$RarPath -or !(Test-Path -LiteralPath $RarPath)) { throw 'RAR is required. Install WinRAR or provide -RarPath; no archive-format fallback is used.' }
    # Ignore saved preferences and environment defaults. Detect duplicates of all sizes.
    $arguments=@('a','-cfg-','-ma5','-m5','-s','-md32m','-rr3p','-htb','-oi1:0','-r','-y','-idq','-scfr',$archive,(Split-Path -Leaf $directory))
    Push-Location $packages
    try {
        $output=& $RarPath @arguments 2>&1
        if($LASTEXITCODE -ne 0) { throw "RAR creation failed: $output" }
        $output=& $RarPath t -cfg- -idq -scfr $archive 2>&1
        if($LASTEXITCODE -ne 0) { throw "RAR integrity verification failed: $output" }
    } finally { Pop-Location }
    $settings=[ordered]@{
        format='RAR5';archive=$archive;tool=$RarPath;arguments=$arguments;
        compression=5;solid=$true;requested_dictionary_bytes=33554432;
        recovery_percent=3;file_checksum='BLAKE2sp-256';identical_files='references, including files smaller than 64 KiB';
        dictionary_note='RAR may reduce the stored dictionary size for small packages.';
        integrity_test='PASS'
    }
}
(Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() | Set-Content -LiteralPath "$archive.sha256" -Encoding ascii
$settings | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$archive.packaging.json" -Encoding utf8
Write-Output $archive
