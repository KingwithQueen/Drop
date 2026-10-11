#Requires -Version 5.1
param(
    [string]$ProgramDirectory='',
    [string]$DataDirectory='',
    [string]$ServerUrl='',
    [string]$Login='',
    [switch]$Launch,
    [switch]$Preview
)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot 'Setup.Common.ps1')
if(!$ProgramDirectory) { $ProgramDirectory=Read-DropValue '程序目录（包含 drop.exe）' $PSScriptRoot }
$ProgramDirectory=Resolve-DropPath $ProgramDirectory
$executable=Assert-DropExecutable $ProgramDirectory 'drop.exe'
if(!$DataDirectory) { $DataDirectory=Read-DropValue '客户端数据根目录（每台电脑独立、当前用户可写）' (Join-Path $ProgramDirectory 'Data') }
$DataDirectory=Resolve-DropPath $DataDirectory
Assert-DropDataLocation $ProgramDirectory $DataDirectory
$clientDirectory=Resolve-DropPath (Join-Path $DataDirectory 'client')
$settingsFile=Resolve-DropPath (Join-Path $clientDirectory 'ui.json')
$shortcutFile=Resolve-DropPath (Join-Path $DataDirectory 'Drop.lnk')
if($Preview) {
    [pscustomobject]@{ProgramDirectory=$ProgramDirectory;DataDirectory=$DataDirectory;ClientDirectory=$clientDirectory;Shortcut=$shortcutFile}
    return
}
$existingSettings=Test-Path -LiteralPath $settingsFile -PathType Leaf
if(!$existingSettings -and !$ServerUrl) { $ServerUrl=Read-DropValue '服务器完整地址（也可回车后在程序中填写）' }
if($ServerUrl) { $ServerUrl=Assert-DropServerUrl $ServerUrl }
if($Login -and $Login -cnotmatch '^[A-Za-z0-9_.-]{3,64}$') { throw '账号格式不正确，请保留创建时的大小写。' }
if($existingSettings) {
    $settings=Get-Content -LiteralPath $settingsFile -Raw -Encoding UTF8 | ConvertFrom-Json
    if(($ServerUrl -and (!$settings.PSObject.Properties['url'] -or $settings.url -ne $ServerUrl)) -or
       ($Login -and (!$settings.PSObject.Properties['login'] -or $settings.login -cne $Login))) {
        throw '已有客户端设置与参数不同，未覆盖。请在 Drop 窗口内修改连接设置。'
    }
}
$shell=New-Object -ComObject WScript.Shell
$shortcut=$shell.CreateShortcut($shortcutFile)
$shortcutArguments='--ui-root "'+$clientDirectory+'"'
if(Test-Path -LiteralPath $shortcutFile) {
    if($shortcut.TargetPath -ne $executable -or $shortcut.Arguments -ne $shortcutArguments) { throw '已有 Drop.lnk 指向其他程序或数据目录，未覆盖。' }
}
if(!(Test-Path -LiteralPath $DataDirectory)) { New-DropPrivateDirectory $DataDirectory }
if(!(Test-Path -LiteralPath $clientDirectory)) { New-DropPrivateDirectory $clientDirectory }
if(!$existingSettings) {
    Write-DropJson $settingsFile ([ordered]@{url=$ServerUrl;ca_file='';login=$Login;notifications=$true;tray_flash=$true;autostart=$false;text_compression='none';zstd_level=3})
}
$shortcut.TargetPath=$executable
$shortcut.Arguments=$shortcutArguments
$shortcut.WorkingDirectory=$ProgramDirectory
$shortcut.IconLocation=$executable+',0'
$shortcut.Save()
Write-Host "`n客户端配置完成。双击 $shortcutFile 启动。" -ForegroundColor Green
Write-Host "本地缓存：$clientDirectory`n密码在 Drop 窗口中输入；本脚本不创建服务器账号、不保存密码。"
if($existingSettings) { Write-Host '保留了已有客户端设置和登录数据。' }
if($Launch) { Start-Process -FilePath $executable -ArgumentList $shortcutArguments -WorkingDirectory $ProgramDirectory }
