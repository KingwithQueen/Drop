#Requires -Version 5.1
<#!
首次配置。管理员 PowerShell 中执行；已有服务器请沿用现有数据及服务。
Preview 只显示目录/端口计划，不创建文件、不要求管理员权限。
!#>
param(
    [string]$ProgramDirectory='',
    [string]$DataDirectory='',
    [ValidateRange(1,65535)][int]$Port=8443,
    [ValidateSet('Https','HttpPake')][string]$Transport='Https',
    [string]$CertificateFile='',
    [string]$PrivateKeyFile='',
    [string]$OAuthClientFile='',
    [switch]$SkipGoogle,
    [string]$ServerHost='',
    [string]$Login='',
    [string]$DisplayName='',
    [ValidatePattern('^[A-Za-z0-9_-]{3,80}$')][string]$ServiceName='DropServer',
    [ValidateRange(60,604800)][int]$BackupSeconds=3600,
    [switch]$SkipFirewall,
    [switch]$Preview
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Setup.Common.ps1')

if(!$ProgramDirectory) { $ProgramDirectory=Read-DropValue '程序目录（包含 dropserver.exe）' $PSScriptRoot }
$ProgramDirectory=Resolve-DropPath $ProgramDirectory
$executable=Assert-DropExecutable $ProgramDirectory 'dropserver.exe'
if(!$DataDirectory) { $DataDirectory=Read-DropValue '数据根目录' (Join-Path $ProgramDirectory 'Data') }
$DataDirectory=Resolve-DropPath $DataDirectory
Assert-DropDataLocation $ProgramDirectory $DataDirectory
if(!$Preview -and !$PSBoundParameters.ContainsKey('Port')) {
    $portText=Read-DropValue '监听端口' '8443'
    if($portText -notmatch '^\d{1,5}$' -or [int]$portText -lt 1 -or [int]$portText -gt 65535) { throw '端口应为 1–65535。' }
    $Port=[int]$portText
}
$serverDirectory=Join-Path $DataDirectory 'server'
$certificateDirectory=Join-Path $DataDirectory 'certificates'
$backupDirectory=Join-Path $DataDirectory 'backups'
$recoveryDirectory=Join-Path $DataDirectory 'recovery'
$configurationDirectory=Join-Path $DataDirectory 'config'
$certificateTarget=Join-Path $certificateDirectory 'server.pem'
$oauthTarget=Join-Path $configurationDirectory 'google-oauth-client.json'
$recoveryTarget=Join-Path $recoveryDirectory 'recovery.bin'
$stateFile=Join-Path $configurationDirectory 'setup-state.json'
$plan=[ordered]@{
    ProgramDirectory=$ProgramDirectory; DataDirectory=$DataDirectory; ServerDirectory=$serverDirectory
    CertificateDirectory=$certificateDirectory; BackupDirectory=$backupDirectory
    RecoveryDirectory=$recoveryDirectory; ConfigurationDirectory=$configurationDirectory
    Transport=$Transport; Port=$Port; Listen="0.0.0.0:$Port"; ServiceName=$ServiceName
    BackupSeconds=$BackupSeconds; Firewall=(!$SkipFirewall)
}
if($Preview) { [pscustomobject]$plan; return }

$principal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if(!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw '安装 Windows 服务和设置私密目录权限需要管理员 PowerShell。请以管理员身份重新运行此脚本。'
}
if(Get-Service -Name $ServiceName -ErrorAction SilentlyContinue) { throw "服务 $ServiceName 已存在。首次配置脚本不会覆盖、停止或卸载已有服务。" }
if((Test-Path -LiteralPath $DataDirectory) -and @(Get-ChildItem -LiteralPath $DataDirectory -Force).Count) {
    throw "数据根目录非空，未初始化或修改：$DataDirectory。已有部署沿用原数据；中断配置的步骤记录在 config\setup-state.json。"
}
if(!$SkipFirewall) {
    if(!(Get-Command New-NetFirewallRule -ErrorAction SilentlyContinue)) { throw '防火墙管理模块不可用，可指定 -SkipFirewall 并手动配置局域网入站规则。' }
    if(Get-NetFirewallRule -Name "Drop-$ServiceName" -ErrorAction SilentlyContinue) { throw '同名防火墙规则已存在，未覆盖。指定 -SkipFirewall 可以沿用现有规则。' }
}
$listener=[Net.Sockets.TcpListener]::new([Net.IPAddress]::Any,$Port)
try { $listener.Server.ExclusiveAddressUse=$true; $listener.Start() } catch { throw "端口 $Port 已占用或不可用，请指定其他端口。" } finally { $listener.Stop() }

$certificateText=''; $leaf=$null
if($Transport -eq 'Https') {
    if(!$CertificateFile) { $CertificateFile=Read-DropValue '完整 PEM，或证书链文件的绝对路径（.pem / .crt）' }
    $CertificateFile=Resolve-DropPath $CertificateFile
    $certificateText=[IO.File]::ReadAllText($CertificateFile)
    if($certificateText -notmatch '-----BEGIN CERTIFICATE-----([\s\S]+?)-----END CERTIFICATE-----') { throw '证书文件须包含 PEM 格式的证书链。' }
    $leaf=[Security.Cryptography.X509Certificates.X509Certificate2]::new([Convert]::FromBase64String(($Matches[1] -replace '\s','')))
    if($leaf.NotAfter -le (Get-Date) -or $leaf.NotBefore -gt (Get-Date)) { $leaf.Dispose(); throw '证书已经过期或尚未生效。' }
    if($certificateText -notmatch '-----BEGIN (RSA |EC )?PRIVATE KEY-----') {
        if(!$PrivateKeyFile) { $PrivateKeyFile=Read-DropValue '匹配的未加密 PEM 私钥文件绝对路径（.key）' }
        $PrivateKeyFile=Resolve-DropPath $PrivateKeyFile
        $certificateText=$certificateText.TrimEnd()+"`r`n"+[IO.File]::ReadAllText($PrivateKeyFile)
    } elseif($PrivateKeyFile) { throw '完整 PEM 已含私钥，不需要再指定 PrivateKeyFile。' }
    if($certificateText -notmatch '-----BEGIN (RSA |EC )?PRIVATE KEY-----' -or $certificateText -match 'ENCRYPTED|Proc-Type:') {
        throw '服务需要匹配的未加密 PEM 私钥；完整 PEM 将只保存到受管理员/SYSTEM 保护的证书目录。'
    }
    $hostDefault=$leaf.GetNameInfo([Security.Cryptography.X509Certificates.X509NameType]::DnsName,$false)
    $leaf.Dispose()
} else {
    if($CertificateFile -or $PrivateKeyFile) { throw 'HttpPake 模式不接收证书参数；HTTPS 模式为默认值。' }
    $hostDefault=$env:COMPUTERNAME
}
if(!$ServerHost) { $ServerHost=Read-DropValue '客户端访问的服务器域名或 IPv4（HTTPS 必须符合证书）' $hostDefault }
$scheme=if($Transport -eq 'Https') {'https'} else {'http'}
$serverUrl=Get-DropServerUrl $ServerHost $Port $scheme
if(!$Login) { $Login=Read-DropValue '创建第一个 Drop 账号（例如 Jsnowly）' }
if($Login -cnotmatch '^[A-Za-z0-9_.-]{3,64}$') { throw '账号应为 3–64 个英文字母、数字、下划线、点或短横线，保留大小写。' }
if(!$DisplayName) { $DisplayName=$Login }
if([Text.Encoding]::UTF8.GetByteCount($DisplayName) -gt 128 -or $DisplayName -match '[\r\n"]') { throw '显示名称应不超过 128 个 UTF-8 字节且不包含换行或双引号。' }
if($SkipGoogle -and $OAuthClientFile) { throw 'SkipGoogle 和 OAuthClientFile 不能同时指定。' }
if(!$SkipGoogle -and !$OAuthClientFile) { $OAuthClientFile=Read-DropValue 'Google 桌面应用 OAuth JSON 完整路径（暂不授权可直接回车）' }
if($OAuthClientFile) {
    $OAuthClientFile=Resolve-DropPath $OAuthClientFile
    $oauth=Get-Content -LiteralPath $OAuthClientFile -Raw -Encoding UTF8 | ConvertFrom-Json
    if(!$oauth.PSObject.Properties['installed'] -or !$oauth.installed.PSObject.Properties['client_id'] -or
       !$oauth.installed.PSObject.Properties['client_secret'] -or !$oauth.installed.client_id -or !$oauth.installed.client_secret) {
        throw '需要 Google “桌面应用”的 OAuth JSON（installed），不能使用 Web 应用配置。'
    }
    $oauth=$null
}
# Catch missing runtime DLLs before creating any data.
Invoke-DropNative $executable @('--version') '检查程序运行依赖'
Write-Host "`n数据根目录：$DataDirectory`n服务地址：$serverUrl`n服务名称：$ServiceName"

$state=[ordered]@{Version=1;Plan=$plan;ServerUrl=$serverUrl;Login=$Login;CompletedSteps=@();Status='InProgress';FailedStep=''}
$step='创建受保护目录'
try {
    if(Test-Path -LiteralPath $DataDirectory) {
        # Existing empty root: protect only after all input/preflight checks pass.
        $security=New-Object Security.AccessControl.DirectorySecurity
        $security.SetSecurityDescriptorSddlForm('O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)')
        Set-Acl -LiteralPath $DataDirectory -AclObject $security
    } else { New-DropPrivateDirectory $DataDirectory -Machine }
    foreach($directory in @($serverDirectory,$certificateDirectory,$backupDirectory,$recoveryDirectory,$configurationDirectory)) {
        New-DropPrivateDirectory $directory -Machine
    }
    Write-DropJson $stateFile $state
    if($Transport -eq 'Https') { [IO.File]::WriteAllText($certificateTarget,$certificateText,[Text.UTF8Encoding]::new($false)) }
    $certificateText=$null
    if($OAuthClientFile) { Copy-Item -LiteralPath $OAuthClientFile -Destination $oauthTarget }
    $state.CompletedSteps+=@('目录与配置文件'); Write-DropJson $stateFile $state

    $step='初始化加密存储'; Invoke-DropNative $executable @('init','--data',$serverDirectory) $step
    $state.CompletedSteps+=@($step); Write-DropJson $stateFile $state
    $step='创建账号（密码至少 12 个 UTF-8 字节，并输入两次）'
    Invoke-DropNative $executable @('account-create','--data',$serverDirectory,'--login',$Login,'--display',$DisplayName) $step
    $state.CompletedSteps+=@('创建账号'); Write-DropJson $stateFile $state
    $step='导出恢复包（独立密码至少 16 个 UTF-8 字节，并输入两次）'
    Invoke-DropNative $executable @('recovery-export','--data',$serverDirectory,'--target',$recoveryTarget) $step
    $state.CompletedSteps+=@('导出恢复包'); Write-DropJson $stateFile $state
    if($OAuthClientFile) {
        $step='浏览器 Google 授权'
        Invoke-DropNative $executable @('drive-authorize','--data',$serverDirectory,'--oauth-client',$oauthTarget) $step
        $state.CompletedSteps+=@($step); Write-DropJson $stateFile $state
    }
    $step='转换服务运行身份'; Invoke-DropNative $executable @('runtime-machine','--data',$serverDirectory) $step
    $state.CompletedSteps+=@($step); Write-DropJson $stateFile $state
    $serviceArguments=@('service-install','--name',$ServiceName,'--data',$serverDirectory,'--listen',"0.0.0.0:$Port",'--backup-directory',$backupDirectory,'--backup-seconds',"$BackupSeconds")
    if($Transport -eq 'Https') { $serviceArguments+=@('--certificate',$certificateTarget) }
    $step='安装服务'; Invoke-DropNative $executable $serviceArguments $step
    $state.CompletedSteps+=@($step); Write-DropJson $stateFile $state
    if(!$SkipFirewall) {
        $step='添加局域网防火墙规则'
        New-NetFirewallRule -Name "Drop-$ServiceName" -DisplayName "DropServer ($ServiceName) TCP $Port" -Direction Inbound -Action Allow -Protocol TCP -LocalPort $Port -Program $executable -RemoteAddress LocalSubnet -Profile Domain,Private | Out-Null
        $state.CompletedSteps+=@($step); Write-DropJson $stateFile $state
    }
    $step='启动服务'; Invoke-DropNative $executable @('service-start','--name',$ServiceName) $step
    $state.CompletedSteps+=@($step); Write-DropJson $stateFile $state
    $step='验证服务接口'
    $info=Test-DropLocalEndpoint $ServerHost $Port $Transport
    $state.CompletedSteps+=@($step); $state.Status='Complete'; Write-DropJson $stateFile $state
    Write-Host "`n配置完成。服务接口已验证：$($info.version) / $($info.transport)" -ForegroundColor Green
    Write-Host "客户端地址：$serverUrl`n账号：$Login`n服务器实际数据目录：$serverDirectory`n恢复包：$recoveryTarget"
    Write-Host '请将恢复包另外备份到安全存储，并分别保管恢复密码。客户端首次登录不需要复制服务器密钥。'
    if(!$OAuthClientFile) { Write-Host 'Google 授权已跳过，可按 README 的已有服务器授权步骤稍后添加。' }
    if(!$SkipFirewall) { Write-Host '防火墙规则仅允许域/专用网络的本地子网；客户端电脑须能解析服务器地址并直连。' }
} catch {
    $state.Status='Failed'; $state.FailedStep=$step
    if(Test-Path -LiteralPath $configurationDirectory) { Write-DropJson $stateFile $state }
    Write-Warning "配置停在：$step。已有数据和已完成步骤保留；查看 $stateFile。不要重新初始化或删除已有服务器数据。"
    throw
} finally { $certificateText=$null }
