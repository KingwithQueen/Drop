# dropserver 1.0.1 服务器

本包包含中央服务器、运行 DLL、首次配置脚本、说明和许可。客户端使用独立 Drop 包。完整解压并保留 DLL 和 Setup.Common.ps1；需要对应架构的 Microsoft Visual C++ v145 运行时。

## 首次配置脚本

在中央服务器以管理员身份打开 PowerShell，完整解压程序后执行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File 'E:\Program Files\Drop\Setup-DropServer.ps1' -ProgramDirectory 'E:\Program Files\Drop' -DataDirectory 'E:\ProgramData\DropData'
```

也可直接运行包内 Setup-DropServer.ps1，按提示选择程序目录和数据根目录。数据根目录默认是程序目录下的 Data，端口提示默认 8443；指定 `-Port 9443` 可改为 9443。脚本默认 HTTPS，按提示提供已有完整 PEM，或证书链 .crt 和匹配的未加密私钥 .key 文件；例如从 lego 的 certificates 目录选择相应域名的文件。不支持直接导入 PFX。证书自动合并为数据目录下的 certificates\server.pem，不打印私钥。

脚本自动创建以下目录，无需逐个指定：

| 数据根目录内 | 用途 |
| --- | --- |
| server | 实际服务器数据、运行密钥、Google 授权 |
| certificates | 服务使用的证书链与私钥 |
| backups | 每小时本地加密备份，间隔可用 -BackupSeconds 调整 |
| recovery | 独立恢复包 recovery.bin |
| config | OAuth 配置副本及 setup-state.json 步骤记录 |

随后输入第一个 Drop 账号名；账号密码和独立恢复密码均由 dropserver 在本机遮蔽输入两次。Google OAuth JSON 路径可直接回车跳过，也可提供桌面应用配置后在浏览器完成授权。授权先于机器 DPAPI 转换，之后安装 LocalSystem 自动启动服务，监听 0.0.0.0:指定端口，并验证真实服务器接口。密码不进入脚本参数、配置文件或步骤记录。

默认添加“域/专用网络 + 本地子网”的程序/端口入站规则；已有规则或希望自行配置时加 `-SkipFirewall`。Windows 将网络归类为公用网络时，这条规则不会放行。指定 `-Transport HttpPake` 可部署原有 HTTP/PAKE 模式，脚本仍默认端口 8443，客户端使用脚本输出的 http 地址。指定 `-Preview` 只查看目录与端口计划，不创建文件、不安装服务。

数据根目录只供管理员和 SYSTEM 访问，不存放客户端缓存。必须使用全新或空的数据根目录，且同名服务不存在；已有部署应按下方更新/管理流程操作。脚本拒绝覆盖非空目录、已有服务、同名防火墙规则和链接路径。某一步失败会停止后续步骤并保留数据，config\setup-state.json 记录已完成步骤；不要为了重跑脚本删除或重新初始化已有存储。证书私钥不匹配会导致服务启动失败；HTTPS 接口验证使用 Windows 的正常证书链和域名检查，不忽略证书错误，也不经过系统代理。成功验证本机接口仍需在另一台电脑测试域名解析、代理例外和实际收发。

恢复包另行复制到安全存储，恢复密码分开保管。本脚本不写入 AppData 或桌面目录；所有应用数据都在选定的数据根目录。Windows 自身的服务/防火墙登记由系统管理。

## 稍后添加 Google 授权

对已经部署、之前跳过 Google 的服务，在管理员 PowerShell 执行；OAuth 源文件路径换成实际文件：

```powershell
$exe = 'E:\Program Files\Drop\dropserver.exe'
$data = 'E:\ProgramData\DropData\server'
& $exe service-stop --name DropServer
if ($LASTEXITCODE -ne 0) { throw '停止服务失败，后续未执行。' }
try {
    & $exe drive-authorize --data $data --oauth-client 'E:\OAuth\client_secret.json'
    if ($LASTEXITCODE -ne 0) { throw 'Google 授权失败，请核对浏览器提示。' }
    & $exe runtime-machine --data $data
    if ($LASTEXITCODE -ne 0) { throw '服务身份转换失败。' }
} finally {
    & $exe service-start --name DropServer
}
```

已有服务器不运行首次初始化脚本。按浏览器提示授权，授权码和令牌只保存在本机。

## 更新已部署的服务器

在中央服务器以管理员身份打开 PowerShell，用现有安装目录中的程序停止服务：

```powershell
$exe = 'E:\Program Files\Drop\dropserver.exe'
& $exe service-stop --name DropServer
```

停止后，将本包的 dropserver.exe 和全部 DLL 覆盖到 `E:\Program Files\Drop`。保留原数据、证书和服务启动参数，然后启动：

```powershell
& $exe service-start --name DropServer
& $exe service-status --name DropServer
```

本次账户管理更新仍为 1.0.1。替换程序需要正常停止一次，首次启动自动迁移到 schema 11；保留原数据，不重新初始化、不重新授权 Google、不重新安装服务。启动新版后账户与数据管理可在线执行，详见 [账户与在线管理](ACCOUNT_MANAGEMENT.md)。客户端也应更新以自动同步显示名。

## 创建新账号

账号只在中央服务器创建。管理员控制台直接对服务实际使用的数据目录操作，服务可以保持运行：

```powershell
$exe = 'E:\Program Files\Drop\dropserver.exe'
$data = 'E:\ProgramData\DropData\server'
& $exe account-create --data $data --login 'NewUser' --display '新用户'
```

在 `Account password (12+ UTF-8 bytes):` 输入密码，随后在 `Confirm password:` 再输入一次。两次一致才创建账号；不一致取消，不保存账号。账号允许大小写英文字母，其他设备登录时使用相同大小写。密码重置和独立恢复包导出也输入两遍；普通登录仍输入一次。

如果实际服务数据目录不是示例路径，必须使用服务已经配置的目录。同一账号可在多台电脑登录，每台客户端使用独立本地数据。

首次部署、证书、防火墙、服务身份与备份流程见包内 `docs/DEPLOYMENT.md`；恢复包管理见 `docs/BACKUP_RESTORE.md`。应用数据默认在程序旁 `Data\DropServer`，可通过 `--data` 自定义。
