# 构建、运行与部署

独立发布包附带首次配置脚本：服务器 Setup-DropServer.ps1（管理员运行），客户端 Setup-Drop.ps1（普通用户运行）。只指定程序目录和数据根目录，子目录自动创建，服务端口默认 8443、可用 -Port 修改。具体步骤和已有部署的保护规则见 SERVER_DEPLOYMENT.md、CLIENT_DEPLOYMENT.md。脚本共用 Setup.Common.ps1，解压后保留在同一目录。

原 Visual Studio 2026 解决方案 drop.slnx 与主项目 drop 保留。业务代码按 C17、UTF-8、/W4 /WX 编译。当前 1.0.2 供长期部署试用，实际范围以 DEVELOPMENT_STATUS.md 和 VERIFICATION.md 为准；版本号不表示全部现场验收已完成。

## 可复现构建

在项目根目录运行 PowerShell：

```powershell
.\scripts\Bootstrap.ps1
.\scripts\Build.ps1 -Dependencies -Platform x64 -Configuration Debug -Test
.\scripts\Build.ps1 -Platform x64 -Configuration Release -Test
.\scripts\Build.ps1 -Dependencies -Platform Win32 -Configuration Release -Test
.\scripts\Package.ps1 -Platform x64
.\scripts\Package.ps1 -Platform x64 -Component Client
.\scripts\Package.ps1 -Platform x64 -Component Server
```

依赖固定于 vcpkg.json，分别安装在 third_party/installed-x64/x64-windows 和 third_party/installed-Win32/x86-windows，避免架构切换移除依赖。输出 build/bin/<平台>/<配置>/。每次自测创建独立目录，失败数据保留。Visual Studio 可直接打开原解决方案；先准备所选平台依赖。

-Production -Configuration Release 输出 build/production/<平台>/，排除测试密码和测试入口。Package.ps1 从实际导入表收集运行 DLL（另包含 CivetWeb 动态加载的 TLS DLL）、许可、配置和说明。默认使用本机 WinRAR/RAR，生成 RAR5 固实包：最高等级 m5、指定 32 MiB 字典、3% 恢复记录、BLAKE2sp-256 文件校验和相同文件引用（所有大小）；同时生成文件 SHA-256 清单、归档 SHA-256 与归档参数记录。RAR 对小包可能自动缩小实际字典；恢复记录占比含格式开销，接近指定值。可显式使用 -ArchiveFormat Zip 保留原 ZIP 交付方式，RAR 不可用时不会自动改用 ZIP。-Component Client 仅客户端，Server 仅服务器，默认 All 为完整包。需要对应架构的 Microsoft Visual C++ v145 运行时；不附带调试运行时、证书、密钥、运行数据或测试账号。原 MIT 作者/年份占位符须所有者在公开再分发前确认。

按所有者要求，tests/、scripts/Verify-*.ps1 和 dropcommon/self_test.c 为本机验收代码，不进入 Git 源码发布，已跟踪条目从索引移除但本机文件保留。没有本机 self_test.c 时普通构建自动关闭诊断代码；生产构建始终关闭，不编译 self_test.c。-Test 需要本机验收源文件存在，避免源码发布缺少测试文件造成链接失败。

图标源为根目录 Dropico.png（客户端）和 DropServerico.png（服务器）；已编码的 drop/drop.ico、dropserver/dropserver.ico 编入 EXE，客户端窗口与托盘也使用该资源。更换原图后运行 `python scripts/Generate-Icons.py`（需 Pillow）再构建；常规构建不依赖 Python。

Drop 和 dropserver 产品版本、Windows 文件属性中的文件版本均为三段 1.0.2。两个程序的 --version、客户端窗口标题、服务器 /api/v1/server-info 和 HTTP User-Agent 使用同一版本，包名由 scripts/Package.ps1 读取 dropcommon/version.h 生成。Windows 固定数值资源及程序集身份按系统要求保留内部四段格式，末尾零不作为产品迭代号；FileVersion 与 ProductVersion 显示字符串共用 DROP_VERSION。x64/Win32 使用相同产品版本；Debug 文件资源标记调试属性，Release/生产资源不标记。业务协议版本 1 和密钥版本 1 保留，当前服务器 schema 12、客户端 schema 5，升级前自动保存迁移快照。消息删除需要更新服务器和全部客户端到本次构建，保留原数据目录及服务参数；运行时删除不需停止服务。

## 服务器与传输模式

将发布包完整解压到非 C 盘的可写目录，例如 E:\Drop；保留运行 DLL。省略 --data 时，服务器使用 dropserver.exe 旁的 `Data\DropServer`，客户端使用 drop.exe 旁的 `Data\Drop`，与当前工作目录、AppData 环境值无关。目录不可写时明确失败，不向系统盘回退。查询目录只显示路径，不创建文件；init 创建受权限保护的加密存储，没有默认账号：

```powershell
.\dropserver.exe data-directory
.\dropserver.exe init
.\dropserver.exe account-create --login alice --display 小明
.\dropserver.exe serve --listen 0.0.0.0:8080
```

不提供 --certificate 即 HTTP + PAKE，客户端填 `http://服务器局域网地址:8080`，账号密码只在客户端输入，服务器以验证材料认证，不发送明文密码。提供 --certificate 即 HTTPS/WSS，客户端填 https 地址并校验证书，应用不叠加 PAKE 加密；失败不自动降级。两种模式是显式部署选择。HTTP 无需证书，HTTPS 可在以后配置符合主机名的证书；Google 后端始终使用 HTTPS。见 PAKE.md。

部署时可以手动选择任意合适的非 C 盘目录。在发布包目录的本机控制台运行以下示例，将路径替换为自己的目录；之后每条数据管理命令均使用同一 --data：

```powershell
.\dropserver.exe init --data E:\DropData\server
.\dropserver.exe account-create --data E:\DropData\server --login alice --display 小明
.\dropserver.exe serve --data E:\DropData\server --listen 127.0.0.1:8443 --certificate E:\DropData\server\server.pem --backup-directory E:\DropData\backups --backup-seconds 3600
```

登录名支持 3–64 个 ASCII 英文字母（大小写均可）、数字或 `_.-`，例如 `Jsnowly`；按输入大小写保存，登录时使用相同大小写。显示名可使用中文。密码仅在本机遮蔽输入，至少 12 个 UTF-8 字节；不支持参数、重定向或配置密码。没有默认账号。账号创建失败会区分名称/密码格式、重复账号、凭据生成与数据库写入错误；失败不覆盖已有账号。新版服务器支持在线执行 account-enable、account-disable、account-reset（--login）、account-display（另带 --display）、account-delete 及 device-revoke（另带 --device）；禁用、重设和撤销使凭据失效，保留历史。数据管理命令可以省略 --data 使用默认目录，也可显式指定，在线命令通过受认证的本机管道交由原服务器执行；离线命令仍使用独占目录锁。quota-set --login --bytes 设置逻辑容量；不足明确失败，保留历史和客户端副本。

`data-directory --data "D:\DropData\server"` 可确认指定目录的绝对路径。更换 --data 选择的是另一个服务器存储，不会自动搬迁消息、密钥或 Google 凭据；新主机或新数据目录需自行初始化、授权及保管独立恢复包。恢复已有备份时将 restore 的 --data 指向全新目标目录，按 BACKUP_RESTORE.md 操作。默认目录中的本次 Google 校验只有空存储基线，尚未创建实际部署账号、TLS 证书或独立恢复包。

PEM 包含证书链及私钥，放在受保护目录，SAN 与客户端域名/IP 匹配。默认只监听本机；公网监听、证书和必要防火墙规则由部署者设置，程序不改变防火墙或系统信任。合法私有 CA 可指定绝对 PEM 路径，发送凭据前验证链、名称、有效期和用途。见 TRUST.md。

serve --config FILE 可读取示例正文压缩和心跳设置。服务器和各客户端本地压缩独立，默认 none，新数据支持 zstd 1–19，历史不自动重压。server_url 仅用于配置校验，监听仍由 --listen 指定。

## Windows 服务

创建账号、重置账号密码及导出独立恢复包均要求两次密码输入，在 `Confirm password:` 再输入一次；不一致则取消，保留原账号和恢复包。账号设置至少 12 UTF-8 字节，独立恢复密码至少 16 UTF-8 字节。用户登录和使用已有恢复包时输入已有密码一次。

新账号只能在中央服务器的本机管理控制台创建；新版服务运行时可直接执行 account-create，保持消息服务运行；其他在线管理命令见 ACCOUNT_MANAGEMENT.md。使用该服务器的数据目录和运行密钥身份；机器 DPAPI 服务目录需管理员权限。在另一台电脑安装独立客户端包后，填写相同服务器地址及新账号密码，同一账号可在多台设备登录，每台设备保留自己的本地数据。

先导出并单独保管恢复包（见 BACKUP_RESTORE.md），停止前台服务器，在管理员控制台中执行：

```powershell
.\dropserver.exe runtime-machine --data E:\DropData\server
.\dropserver.exe service-install --name DropServer --data E:\DropData\server --certificate E:\DropData\server\server.pem --listen 127.0.0.1:8443 --backup-directory E:\DropData\backups --backup-seconds 3600
.\dropserver.exe service-start --name DropServer
.\dropserver.exe service-status --name DropServer
.\dropserver.exe service-stop --name DropServer
.\dropserver.exe service-uninstall --name DropServer
```

LocalSystem 自动启动，失败重启，可用独立合法服务名。runtime-machine 要求管理员，将密钥改为机器 DPAPI，目录树移除原用户显式权限并设置 Administrators 所有者和 SYSTEM/Administrators 访问权限；遇重解析点拒绝。服务命令核查属于本包，卸载不删除数据。证书和备份目录同样限制为管理员/SYSTEM 可访问。必须运行 scripts/Verify-Service.ps1 完成随机隔离服务的生命周期、运行身份解锁、HTTPS 登录和重启；编译成功不代表服务验收完成。

service-install 未指定 --data 时，使用程序旁 Data\DropServer，并将解析后的绝对路径写入服务启动参数；service-run 必须收到该显式路径，不使用 LocalSystem 的个人目录。安装前必须对同一目录完成 runtime-machine。service-install 不带 --certificate 时安装 HTTP + PAKE 服务；带证书则安装原 HTTPS 服务。

无证书局域网服务完整顺序（先在本机控制台完成初始化、自己的账号和可选 Google 浏览器授权，再用管理员控制台转换机器凭据）：

```powershell
.\dropserver.exe data-directory
.\dropserver.exe init
.\dropserver.exe account-create --login alice --display 小明
# 如需 Google 备份，在 runtime-machine 之前完成 drive-authorize。
.\dropserver.exe runtime-machine
.\dropserver.exe service-install --name DropServer --listen 0.0.0.0:8080 --backup-directory "E:\DropBackups" --backup-seconds 3600
.\dropserver.exe service-start --name DropServer
.\dropserver.exe service-status --name DropServer
```

仅允许防火墙的局域网所需入站端口。客户端填 http 地址并登录，跨电脑的数据及通知由会话加密。升级旧 schema 9 服务器，先停服务并备份，在本机执行 `account-reset --login alice`（可输入原密码），建立 PAKE 材料并撤销旧设备凭据，再启动和重新登录。切换 HTTPS 时停服务、卸载服务注册（保留数据）、用同一 --data 和 --certificate 重新安装，客户端改 https 地址并登录。旧数据与 OAuth 文件不会自动移动；变更目录应通过备份/独立恢复流程保留身份，不能把空目录当作原服务器。

## 托盘客户端

无参数运行 drop.exe，数据默认在程序旁 Data\Drop；通过右上角「配置与账号…」填入完整 http 或 https 地址和账号，HTTPS 可配置私有 CA，HTTP 无需 CA。密码遮蔽且提交后清空，令牌及 HTTP 派生登录凭据用用户 DPAPI 保护。收件人留空发给自己，附件先形成稳定副本，相关设备自动完整下载并校验，收到文件不会自动运行。可用 --ui-root 指定其他非 C 盘目录。

关闭或最小化窗口后保留托盘；真正退出停止同步，重启补齐。自动启动只设置本用户独立 Run 值，默认关闭。历史每页 50 条，正文逐字搜索和日期过滤在本机执行；显式导出须选目的地，不能覆盖内部文件。

“Windows 通知”和“托盘图标闪烁”是两个独立选项，默认开启，重启后保留。窗口显示时只请求任务栏原生闪烁，不显示 Windows 通知；切回 Drop 停止任务栏提示，提示颜色由 Windows 主题决定。隐藏到托盘后，新消息可显示 Windows 通知，并在有未读消息时闪烁托盘图标。打开窗口或未读清零后停止托盘闪烁，图标恢复正常；关闭 Windows 通知仍可保留托盘提示，两个选项都关闭时消息照常同步并计入未读。旧配置中的 notifications 开关继续控制 Windows 通知，新增 tray_flash 开关控制托盘。系统勿扰和通知权限可能抑制 Windows 通知，不能据此判断是否收到了消息。

正文保存、文件保存、已读和服务器接收分开显示。容量不足不确认完成，断线保留任务。超过 32 MiB 的上传在后台校验，完成后才提交消息，其他正文继续同步。恢复旧快照后保留较新本机副本并暂停不确定发送，核对后明确重试。

开发隔离入口：drop.exe --ui-root ABSOLUTE_DIRECTORY [--background]，同根只有一个实例。client-login、client-queue、client-attach、client-send、client-sync 为诊断入口，参数不带密码。client-send 等待大文件校验，client-sync 完整下载；托盘分批下载尽快显示正文。客户端不注册服务。验收证据位于 build/tests，环境和未运行范围见 VERIFICATION.md。

## 正式数据部署与试运行

当前已完成阶段零至八记录的本机实现、真实或模拟验收，可以准备小范围部署试用。真实 Google 云盘演练、服务演练和开发账号位于各自的 build/tests 隔离目录，不能当成实际服务器已部署。为实际数据目录初始化服务器、创建自己的 Drop 账号、选择 HTTP/PAKE 或配置符合客户端主机名的 HTTPS 证书，导出并单独保管独立恢复包及密码，再为该目录配置 Google 授权和备份计划。部署为 Windows 服务时，先完成用户授权，再以管理员执行 runtime-machine 迁移，让服务身份读取机器 DPAPI；随后核对服务启动、实际自动备份、云盘完成记录和下载恢复。前台部署可以继续使用用户 DPAPI。

Google OAuth 的 Testing/正式发布状态与 Google 的应用验证是不同设置。当前只申请非敏感 drive.appdata；Testing 状态签发的刷新令牌七天后过期。项目所有者决定切换到正式发布状态后，应在实际服务器数据目录重新完成浏览器授权并验证备份；这不代表已经完成 Google 品牌验证，也不保证令牌永远有效。见 GOOGLE_DRIVE.md 及 [Google 状态说明](https://developers.google.com/identity/protocols/oauth2/production-readiness/overview)。

小范围试用仍应在实际部署环境完成第二台电脑登录/完整文件接收、服务器实际定时备份及独立恢复，并观察持续运行。尚未完成的跨物理主机、真实休眠/锁屏/重登录、24 小时稳定性、服务身份真实 Google 后台镜像和真实限流/配额/授权到期测试见 VERIFICATION.md；已实现功能的本机 PASS 不等于这些现场项目已全部通过。
