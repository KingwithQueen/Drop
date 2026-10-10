# 构建、运行与部署

原 Visual Studio 2026 解决方案 drop.slnx 与主项目 drop 保留。业务代码按 C17、UTF-8、/W4 /WX 编译。当前为发布候选，实际范围以 DEVELOPMENT_STATUS.md 和 VERIFICATION.md 为准。

## 可复现构建

在项目根目录运行 PowerShell：

```powershell
.\scripts\Bootstrap.ps1
.\scripts\Build.ps1 -Dependencies -Platform x64 -Configuration Debug -Test
.\scripts\Build.ps1 -Platform x64 -Configuration Release -Test
.\scripts\Build.ps1 -Dependencies -Platform Win32 -Configuration Release -Test
.\scripts\Package.ps1 -Platform x64
```

依赖固定于 vcpkg.json，分别安装在 third_party/installed-x64/x64-windows 和 third_party/installed-Win32/x86-windows，避免架构切换移除依赖。输出 build/bin/<平台>/<配置>/。每次自测创建独立目录，失败数据保留。Visual Studio 可直接打开原解决方案；先准备所选平台依赖。

-Production -Configuration Release 输出 build/production/<平台>/，排除测试密码和测试入口。Package.ps1 从实际导入表收集运行 DLL（另包含 CivetWeb 动态加载的 TLS DLL）、许可、配置和说明，生成 ZIP、文件 SHA-256 清单及 ZIP 校验值。需要对应架构的 Microsoft Visual C++ v145 运行时；不附带调试运行时、证书、密钥、运行数据或测试账号。原 MIT 作者/年份占位符须所有者在公开再分发前确认。

## 服务器与证书

发布包目录的本机控制台运行，先创建独立父目录，数据目录由程序限制权限：

```powershell
.\dropserver.exe init --data C:\DropData\server
.\dropserver.exe account-create --data C:\DropData\server --login alice --display 小明
.\dropserver.exe serve --data C:\DropData\server --listen 127.0.0.1:8443 --certificate C:\DropData\server\server.pem --backup-directory C:\DropData\backups --backup-seconds 3600
```

密码仅在本机遮蔽输入，至少 12 个 UTF-8 字节；不支持参数、重定向或配置密码。没有默认账号。停止服务器后执行 account-enable、account-disable、account-reset（--login），device-revoke（另带 --device）；禁用、重设和撤销使凭据失效，保留历史。所有管理命令带 --data，目录锁拒绝并发管理。quota-set --login --bytes 设置逻辑容量；不足明确失败，保留历史和客户端副本。

PEM 包含证书链及私钥，放在受保护目录，SAN 与客户端域名/IP 匹配。默认只监听本机；公网监听、证书和必要防火墙规则由部署者设置，程序不改变防火墙或系统信任。合法私有 CA 可指定绝对 PEM 路径，发送凭据前验证链、名称、有效期和用途。见 TRUST.md。

serve --config FILE 可读取示例正文压缩和心跳设置。服务器和各客户端本地压缩独立，默认 none，新数据支持 zstd 1–19，历史不自动重压。server_url 仅用于配置校验，监听仍由 --listen 指定。

## Windows 服务

先导出并单独保管恢复包（见 BACKUP_RESTORE.md），停止前台服务器，在管理员控制台中执行：

```powershell
.\dropserver.exe runtime-machine --data C:\DropData\server
.\dropserver.exe service-install --name DropServer --data C:\DropData\server --certificate C:\DropData\server\server.pem --listen 127.0.0.1:8443 --backup-directory C:\DropData\backups --backup-seconds 3600
.\dropserver.exe service-start --name DropServer
.\dropserver.exe service-status --name DropServer
.\dropserver.exe service-stop --name DropServer
.\dropserver.exe service-uninstall --name DropServer
```

LocalSystem 自动启动，失败重启，可用独立合法服务名。runtime-machine 要求管理员，将密钥改为机器 DPAPI，目录树移除原用户显式权限并设置 Administrators 所有者和 SYSTEM/Administrators 访问权限；遇重解析点拒绝。服务命令核查属于本包，卸载不删除数据。证书和备份目录同样限制为管理员/SYSTEM 可访问。必须运行 scripts/Verify-Service.ps1 完成随机隔离服务的生命周期、运行身份解锁、HTTPS 登录和重启；编译成功不代表服务验收完成。

## 托盘客户端

无参数运行 drop.exe，数据默认在本用户 LOCALAPPDATA/Drop。填入完整 HTTPS 地址、账号和可选私有 CA；密码遮蔽且提交后清空，凭据用用户 DPAPI 保护。收件人留空发给自己，附件先形成稳定副本，相关设备自动完整下载并校验，收到文件不会自动运行。

关闭窗口保留托盘；真正退出停止同步，重启补齐。自动启动只设置本用户独立 Run 值，默认关闭。勿扰模式可能抑制提醒，历史/未读独立保存。历史每页 50 条，正文逐字搜索和日期过滤在本机执行；显式导出须选目的地，不能覆盖内部文件。

正文保存、文件保存、已读和服务器接收分开显示。容量不足不确认完成，断线保留任务。超过 32 MiB 的上传在后台校验，完成后才提交消息，其他正文继续同步。恢复旧快照后保留较新本机副本并暂停不确定发送，核对后明确重试。

开发隔离入口：drop.exe --ui-root ABSOLUTE_DIRECTORY [--background]，同根只有一个实例。client-login、client-queue、client-attach、client-send、client-sync 为诊断入口，参数不带密码。client-send 等待大文件校验，client-sync 完整下载；托盘分批下载尽快显示正文。客户端不注册服务。验收证据位于 build/tests，环境和未运行范围见 VERIFICATION.md。
