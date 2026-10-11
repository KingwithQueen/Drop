# Shared deployment helpers. Distributed beside the two setup scripts.
Set-StrictMode -Version 2.0

function Read-DropValue([string]$Prompt, [string]$Default='') {
    $caption=if($Default) { "$Prompt [$Default]" } else { $Prompt }
    $value=Read-Host $caption
    if([string]::IsNullOrWhiteSpace($value)) { return $Default }
    return $value.Trim().Trim('"')
}

function Resolve-DropPath([string]$Path) {
    if(!$Path -or $Path -notmatch '^[A-Za-z]:[\\/]' -or $Path -match '["\r\n]' -or $Path.Substring(2).Contains(':')) {
        throw '请输入本机磁盘上的绝对路径，例如 E:\ProgramData\DropData。'
    }
    $absolute=[IO.Path]::GetFullPath($Path).TrimEnd('\')
    if($absolute.Length -le 3) { throw '不能直接使用磁盘根目录。' }
    $current=$absolute
    while($current) {
        if(Test-Path -LiteralPath $current) {
            if((Get-Item -LiteralPath $current -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "不支持链接或重解析路径：$current"
            }
        }
        $current=Split-Path -Parent $current
    }
    return $absolute
}

function Assert-DropExecutable([string]$ProgramDirectory, [string]$Name) {
    if(!(Test-Path -LiteralPath $ProgramDirectory -PathType Container)) { throw "程序目录不存在：$ProgramDirectory" }
    $file=Resolve-DropPath (Join-Path $ProgramDirectory $Name)
    if(!(Test-Path -LiteralPath $file -PathType Leaf)) { throw "缺少 $Name，请完整解压对应程序包：$ProgramDirectory" }
    return $file
}

function Assert-DropDataLocation([string]$ProgramDirectory, [string]$DataDirectory) {
    # Data may live below the program directory, but must never contain the program.
    if($ProgramDirectory.Equals($DataDirectory,[StringComparison]::OrdinalIgnoreCase) -or
       $ProgramDirectory.StartsWith($DataDirectory+'\',[StringComparison]::OrdinalIgnoreCase)) {
        throw '数据根目录不能等于程序目录，也不能是程序目录的上级目录。'
    }
    if((Test-Path -LiteralPath $DataDirectory) -and !(Test-Path -LiteralPath $DataDirectory -PathType Container)) {
        throw "数据目录的位置已经有文件：$DataDirectory"
    }
}

function New-DropPrivateDirectory([string]$Path, [switch]$Machine) {
    # Never rewrite permissions on an existing user directory.
    if(Test-Path -LiteralPath $Path) { throw "目录已经存在，未修改权限：$Path" }
    $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
    $owner=if($Machine) { 'BA' } else { $identity.User.Value }
    $extra=if($Machine) { '' } else { "(A;OICI;FA;;;$($identity.User.Value))" }
    $security=New-Object Security.AccessControl.DirectorySecurity
    $security.SetSecurityDescriptorSddlForm("O:${owner}G:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)$extra")
    # .NET Framework can apply the protected ACL atomically when creating the directory.
    # PowerShell 7 exposes the equivalent ACL extension API.
    if($PSVersionTable.PSEdition -eq 'Core') {
        [IO.FileSystemAclExtensions]::CreateDirectory($security,$Path) | Out-Null
    } else {
        [IO.Directory]::CreateDirectory($Path,$security) | Out-Null
    }
}

function Write-DropJson([string]$Path, $Value) {
    [IO.File]::WriteAllText($Path,($Value | ConvertTo-Json -Depth 8),[Text.UTF8Encoding]::new($false))
}

function Get-DropServerUrl([string]$HostName, [int]$Port, [string]$Scheme) {
    if([Uri]::CheckHostName($HostName) -notin @([UriHostNameType]::Dns,[UriHostNameType]::IPv4)) {
        throw '服务器名称必须是域名或 IPv4 地址，不包含协议、端口或路径。'
    }
    $builder=[UriBuilder]::new($Scheme,$HostName,$Port)
    return $builder.Uri.AbsoluteUri.TrimEnd('/')
}

function Assert-DropServerUrl([string]$Value) {
    $uri=$null
    if(![Uri]::TryCreate($Value,[UriKind]::Absolute,[ref]$uri) -or $uri.Scheme -notin @('http','https') -or
       !$uri.Host -or $uri.UserInfo -or $uri.Query -or $uri.Fragment -or $uri.AbsolutePath -ne '/') {
        throw '服务器地址应为 http://域名:端口 或 https://域名:端口，不包含账号、路径或查询参数。'
    }
    return $uri.AbsoluteUri.TrimEnd('/')
}

function Invoke-DropNative([string]$Executable, [string[]]$CliArguments, [string]$Step) {
    Write-Host "`n$Step" -ForegroundColor Cyan
    # Keep the real console attached: the executable masks and confirms passwords.
    & $Executable @CliArguments
    if($LASTEXITCODE -ne 0) { throw "$Step 失败（退出码 $LASTEXITCODE），后续步骤未执行。" }
}

function Test-DropLocalEndpoint([string]$HostName, [int]$Port, [string]$Transport) {
    $connection=[Net.Sockets.TcpClient]::new()
    $stream=$null
    try {
        $connection.Connect('127.0.0.1',$Port)
        $stream=$connection.GetStream()
        $stream.ReadTimeout=10000; $stream.WriteTimeout=10000
        if($Transport -eq 'Https') {
            # Direct loopback connection, normal Windows chain/hostname validation, no proxy.
            $stream=[Net.Security.SslStream]::new($stream,$false)
            $stream.AuthenticateAsClient($HostName)
        }
        $request=[Text.Encoding]::ASCII.GetBytes("GET /api/v1/server-info HTTP/1.0`r`nHost: ${HostName}:$Port`r`nConnection: close`r`n`r`n")
        $stream.Write($request,0,$request.Length)
        $reader=[IO.StreamReader]::new($stream,[Text.Encoding]::UTF8)
        $response=$reader.ReadToEnd()
        if($response -notmatch '^HTTP/1\.[01] 200 ') { throw '服务器接口没有返回 HTTP 200。' }
        $info=($response -split "`r`n`r`n",2)[1] | ConvertFrom-Json
        $expected=if($Transport -eq 'Https') {'tls'} else {'srp6a-sha256-4096-argon2id-xchacha20poly1305-v1'}
        if($info.protocol_version -ne 1 -or $info.transport -ne $expected) { throw '服务器返回的协议或传输模式与配置不一致。' }
        return $info
    } finally {
        if($stream) { $stream.Dispose() }
        $connection.Close()
    }
}
