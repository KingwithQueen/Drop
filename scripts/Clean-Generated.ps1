param(
    [Parameter(Mandatory=$true)][string[]]$KeepPackageNames,
    [switch]$Apply,
    [switch]$SkipInaccessible,
    [ValidatePattern('^[a-z][a-z0-9-]*\.json$')][string]$ReportName=''
)
$ErrorActionPreference='Stop'
$workspace=[System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot)).TrimEnd('\')
$hash=[System.Security.Cryptography.SHA256]::Create()
try { $lockName='Local\DropGeneratedCleanup-'+([BitConverter]::ToString($hash.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($workspace.ToLowerInvariant())))).Replace('-','').Substring(0,20) } finally { $hash.Dispose() }
$mutex=[System.Threading.Mutex]::new($false,$lockName)
$ownership=$false
try {
    $ownership=$mutex.WaitOne(60000)
    if(!$ownership) { throw 'Another generated-data cleanup is still running.' }
$generated=Join-Path $workspace 'build'
$packages=Join-Path $generated 'packages'
$tests=Join-Path $generated 'tests'
$keep=[System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach($name in $KeepPackageNames) {
    if($name -notmatch '^(Drop|Drop-Client|DropServer)-\d+\.\d+\.\d+-(x64|Win32)-\d{8}-\d{6}$') { throw 'Invalid release package name.' }
    if(!(Test-Path -LiteralPath (Join-Path $packages "$name.rar")) -or !(Test-Path -LiteralPath (Join-Path $packages "$name.rar.sha256"))) { throw "Verified release is missing: $name" }
    [void]$keep.Add($name)
}
function Assert-WorkspaceTarget([string]$path) {
    $absolute=[System.IO.Path]::GetFullPath($path)
    if(!$absolute.StartsWith($workspace+'\',[StringComparison]::OrdinalIgnoreCase)) { throw "Target leaves workspace: $absolute" }
    $relative=$absolute.Substring($workspace.Length+1)
    $current=$workspace
    foreach($part in $relative.Split('\')) {
        $current=Join-Path $current $part
        if(Test-Path -LiteralPath $current) {
            if((Get-Item -LiteralPath $current -Force).Attributes -band [System.IO.FileAttributes]::ReparsePoint) { throw "Reparse point is not supported: $current" }
        }
    }
    if(Test-Path -LiteralPath $absolute -PathType Container) {
        if(Get-ChildItem -LiteralPath $absolute -Recurse -Force -Attributes ReparsePoint | Select-Object -First 1) { throw "Target contains a reparse point: $absolute" }
    }
    return $absolute
}
$targets=[System.Collections.Generic.List[object]]::new()
$skipped=[System.Collections.Generic.List[object]]::new()
$protected=[System.Collections.Generic.List[object]]::new()
function Add-Target([string]$path,[string]$kind) {
    if(!(Test-Path -LiteralPath $path)) { return }
    try {
    $absolute=Assert-WorkspaceTarget $path
    $item=Get-Item -LiteralPath $absolute -Force
    if($item.PSIsContainer -and $kind -ne 'isolated-test-data-preserve-results') {
        $runtime=Get-ChildItem -LiteralPath $absolute -Recurse -File -Force | Where-Object {
            $_.Extension -eq '.dpapi' -or $_.Name -in @('device.id','client.db','client.lock','server.lock') -or $_.FullName -match '\\Data\\'
        } | Select-Object -First 1
        if($runtime) {
            $protected.Add([pscustomobject]@{path=$absolute;reason='Contains application data or protected credentials';example=$runtime.FullName})
            return
        }
    }
    $bytes=if($item.PSIsContainer) { (Get-ChildItem -LiteralPath $absolute -Recurse -File -Force | Measure-Object Length -Sum).Sum } else { $item.Length }
    $targets.Add([pscustomobject]@{path=$absolute;kind=$kind;bytes=[long]$bytes})
    } catch {
        if($SkipInaccessible -and $_.CategoryInfo.Category -eq [System.Management.Automation.ErrorCategory]::PermissionDenied) {
            $skipped.Add([pscustomobject]@{path=[System.IO.Path]::GetFullPath($path);reason='Windows administrator permissions required'})
            return
        }
        throw
    }
}
foreach($relative in @('build\bin','build\obj','build\dependency-cache','build\task-temp','build\tool-download','build\sites','build\style-work','build\design-browser-1.0.1','build\design-browser-1.0.1-final','build\design-win32-1.0.1','build\ui-1.0.1','third_party\downloads','third_party\vcpkg\downloads','third_party\vcpkg\buildtrees','third_party\vcpkg\packages')) {
    Add-Target (Join-Path $workspace $relative) 'generated-cache'
}
foreach($relative in @('build\collect_dual_evidence.py','build\Run-Dual-ServiceAcceptance.ps1')) { Add-Target (Join-Path $workspace $relative) 'generated-test-script' }
foreach($item in Get-ChildItem -LiteralPath $packages -Force) {
    if($item.Name -match '^(?<package>(?:Drop|Drop-Client|DropServer)-\d+\.\d+\.\d+-(?:x64|Win32)-\d{8}-\d{6})(?:\.(?:zip|rar)(?:\.sha256|\.packaging\.json)?)?$') {
        if(!$keep.Contains($Matches['package'])) { Add-Target $item.FullName 'superseded-release' }
    }
}
foreach($item in Get-ChildItem -LiteralPath $tests -Directory -Force) {
    if($item.Name -like 'google-real-*') { continue }
    if($item.Name -notmatch '^(account-creation|backup-(Debug|Release)-\u4e2d\u6587|certificate-mode|drive-(Debug|Release)|finalize-Release|icons|integration-(x64|Win32)-(Debug|Release)-\u4e2d\u6587|notifications-(x64|Win32)-(Debug|Release)|pake-(x64|Win32)-(Debug|Release)-\u4e2d\u6587|password-confirmation|production-(x64|Win32)|rar-packages|server-directory-(x64|Win32)-(Debug|Release)|service-(pake-|tls-)?(Debug|Release)|setup-wizard|ui-1\.0\.1-(x64|Win32)-(Debug|Release)|version-icons-1\.0\.0|(x64|Win32)-(Debug|Release))-[0-9a-f]{16,32}$') {
        $protected.Add([pscustomobject]@{path=$item.FullName;reason='Not a known isolated test directory';example=$null})
        continue
    }
    Add-Target $item.FullName 'isolated-test-data-preserve-results'
}
foreach($item in Get-ChildItem -LiteralPath (Join-Path $generated 'production') -File -Recurse -Force | Where-Object {$_.Extension -in @('.pdb','.lib','.obj','.ilk','.exp')}) { Add-Target $item.FullName 'compiler-output' }
foreach($relative in @('tests\__pycache__','scripts\__pycache__')) { Add-Target (Join-Path $workspace $relative) 'python-cache' }
# Abort before deleting anything if an installed service or running program uses a target.
foreach($service in Get-CimInstance Win32_Service) {
    foreach($target in $targets) {
        if($service.PathName -and $service.PathName.IndexOf($target.path,[StringComparison]::OrdinalIgnoreCase) -ge 0) { throw "Service still references generated data: $($service.Name)" }
    }
}
foreach($process in Get-CimInstance Win32_Process) {
    if(!$process.ExecutablePath) { continue }
    foreach($target in $targets) {
        if($process.ExecutablePath.Equals($target.path,[StringComparison]::OrdinalIgnoreCase) -or $process.ExecutablePath.StartsWith($target.path+'\',[StringComparison]::OrdinalIgnoreCase) -or ($process.CommandLine -and $process.CommandLine.IndexOf($target.path,[StringComparison]::OrdinalIgnoreCase) -ge 0)) { throw "Process still uses generated data: $($process.ProcessId); executable=$($process.ExecutablePath); target=$($target.path)" }
    }
}
$report=[ordered]@{date=(Get-Date -Format 'yyyy-MM-dd');result=if($Apply) {'IN_PROGRESS'} else {'PLANNED'};applied=$Apply.IsPresent;workspace=$workspace;kept_packages=@($KeepPackageNames);preserved_real_authorization=@(Get-ChildItem -LiteralPath $tests -Directory | Where-Object {$_.Name -like 'google-real-*'} | ForEach-Object {$_.FullName});targets=$targets;skipped_targets=$skipped;protected_targets=$protected;preserved_results=0;preserved_evidence_files=0;removed_bytes=0;free_bytes_before=([System.IO.DriveInfo]::new([System.IO.Path]::GetPathRoot($workspace))).AvailableFreeSpace;free_bytes_after=0}
$verification=Join-Path $generated 'verification'
New-Item -ItemType Directory -Path $verification -Force | Out-Null
$reportPath=Join-Path $verification $(if($ReportName) {$ReportName} elseif($SkipInaccessible) {'cleanup-accessible-20261011.json'} else {'cleanup-20261011.json'})
if($Apply) {
    foreach($target in $targets) {
        $absolute=Assert-WorkspaceTarget $target.path
        $results=@()
        if($target.kind -eq 'isolated-test-data-preserve-results') {
            $results=@(Get-ChildItem -LiteralPath $absolute -Recurse -File | Where-Object {
                $_.Name -eq 'results.json' -or $_.Extension -in @('.png','.log') -or $_.Name -like '*.listing.txt' -or $_.Name -in @('resources.json','geometry.json')
            } | ForEach-Object { [pscustomobject]@{path=$_.FullName;bytes=[System.IO.File]::ReadAllBytes($_.FullName);sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash} })
            # Remove only disposable files, then prune empty directories. Evidence never needs to be deleted and restored.
            $evidence=[System.Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
            foreach($result in $results) { [void]$evidence.Add($result.path) }
            foreach($file in Get-ChildItem -LiteralPath $absolute -Recurse -File -Force) {
                if(!$evidence.Contains($file.FullName)) { Remove-Item -LiteralPath (Assert-WorkspaceTarget $file.FullName) -Force }
            }
            foreach($directory in Get-ChildItem -LiteralPath $absolute -Recurse -Directory -Force | Sort-Object {$_.FullName.Length} -Descending) {
                if(!(Get-ChildItem -LiteralPath $directory.FullName -Force | Select-Object -First 1)) { Remove-Item -LiteralPath (Assert-WorkspaceTarget $directory.FullName) -Force }
            }
            if(!(Get-ChildItem -LiteralPath $absolute -Force | Select-Object -First 1)) { Remove-Item -LiteralPath $absolute -Force }
        } else {
            Remove-Item -LiteralPath $absolute -Recurse -Force
        }
        foreach($result in $results) {
            if((Get-FileHash -LiteralPath $result.path -Algorithm SHA256).Hash -ne $result.sha256) { throw "Verification evidence changed during cleanup: $($result.path)" }
        }
        $report.preserved_results+=@($results | Where-Object {(Split-Path -Leaf $_.path) -eq 'results.json'}).Count
        $report.preserved_evidence_files+=$results.Count
        $report.removed_bytes+=$target.bytes-([long](($results | ForEach-Object {$_.bytes.Length} | Measure-Object -Sum).Sum))
        $report | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $reportPath -Encoding utf8
    }
    $report.result=if($skipped.Count) {'PARTIAL'} else {'PASS'}
    $report.free_bytes_after=([System.IO.DriveInfo]::new([System.IO.Path]::GetPathRoot($workspace))).AvailableFreeSpace
}
$report | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath $reportPath -Encoding utf8
Write-Output "Generated-data cleanup report: $reportPath"
Write-Output "Targets: $($targets.Count); removed GiB: $([math]::Round($report.removed_bytes/1GB,3)); preserved result files: $($report.preserved_results)"
Write-Output "Protected targets awaiting administrator cleanup: $($skipped.Count)"
Write-Output "Application-data or unknown targets retained: $($protected.Count); evidence files retained: $($report.preserved_evidence_files)"
} finally {
    if($ownership) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
