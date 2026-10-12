# Drop 1.0.3

新增 Linux Wayland（GTK4）和 Intel macOS（AppKit）原生客户端，沿用已确认的高密度布局和 Windows C17 同步核心。双传输、中文附件、WS/WSS 和账号级跨设备删除已进行原生互通验收；构建、运行范围及后续事项见 [Linux/macOS 客户端](docs/CROSS_PLATFORM.md)。Windows 服务器及现有部署继续使用原流程。

当前产品版本为 1.0.3。客户端可删除自己账号发送的消息，包括该账号其他电脑发送的消息；删除同步到中央服务器、双方账号的所有设备，离线操作持久排队。选中消息后使用「删除消息」、右键菜单或 Delete 键，并确认。客户端左侧只显示真实账户，输入区明确显示发送对象及待发送附件的图标、名字和大小，采用紧凑的蓝灰配色。升级须保留原数据目录，并更新服务器及全部客户端到本次构建。

1.0.1 已支持本机在线账户管理：删除保留历史及附件、独立修改显示名、客户端自动同步名称和账号状态。服务可保持运行；使用方式见 [账户与在线管理](docs/ACCOUNT_MANAGEMENT.md)。

C17 Windows 客户端与中央服务器，保留 Visual Studio 2026 的 `drop.slnx` 和主项目 `drop`。已确认需求及阶段顺序见 [DEVELOPMENT_PLAN.md](DEVELOPMENT_PLAN.md)。

1.0.1 将服务器地址、私有 CA、账号登录、通知、自启动和正文压缩移入独立「配置与账号」窗口。主界面已按所有者确认的传统 Win32 布局实现：菜单、窄工具栏、左侧用户、右侧消息、可拖动分隔条与状态栏；自己置顶，1.0.3 移除了「全部消息」入口。消息正文上方显示发送者和完整时间，下方直接逐个显示附件名、大小及保存状态；点击附件名选中对应文件，再明确导出。长正文可双击消息或通过右键“查看完整正文”打开独立阅读窗口，支持文字选择与复制。

主窗口、配置窗口和完整正文窗口分别按所在屏幕的 DPI 重建原生字体和控件位置，支持 125%、150%、175% 等非整数缩放及短屏滚动。主窗口左侧附件槽显示待发送文件，右侧显示所选消息附件；各自滚动，添加附件不改变正文输入区高度。当前确认布局见 [1.0.3 设计图](docs/design/client-main-win32-1.0.3.png)，原设计和历史验收记录保留。

登录状态默认以 Windows DPAPI 加密保存，正常退出后再次启动自动恢复；明确注销才清除。升级须保留原 Data 或继续使用原 --ui-root，见[客户端部署说明](docs/CLIENT_DEPLOYMENT.md)。

C 代码的换行、中文注释和可选中文变量命名遵循 [项目规范](AGENTS.md)，格式配置见 [.clang-format](.clang-format)。Python、PowerShell 测试脚本不受这些风格要求约束。

独立 RAR 包附带首次配置辅助脚本：服务器 [Setup-DropServer.ps1](scripts/Setup-DropServer.ps1) 只需选择程序目录与数据根目录，子目录自动创建，端口默认 8443、支持自定义；管理员运行后依次处理证书、账号、恢复包、可选 Google 授权和服务。客户端 [Setup-Drop.ps1](scripts/Setup-Drop.ps1) 指定自己的数据目录并生成快捷方式。使用方法见 [服务器部署](docs/SERVER_DEPLOYMENT.md) 和 [客户端部署](docs/CLIENT_DEPLOYMENT.md)；已有服务器沿用原数据，不重跑初始化。

阶段零至八的本机实现及记录的真实/模拟验收已完成，提供 x64 和 Win32 包；真实 Google Drive 演练使用 x64 生产候选。现场验证边界以 [开发状态](docs/DEVELOPMENT_STATUS.md) 及 [验收记录](docs/VERIFICATION.md) 为准。[构建与运行](docs/DEPLOYMENT.md)、[固定依赖](docs/DEPENDENCIES.md)、[协议](docs/PROTOCOL.md)、[存储格式](docs/STORAGE_FORMAT.md) 随实现同步维护。

本地加密备份、独立恢复包和服务器服务已接入；Google Drive 原生后端已完成真实浏览器授权、密文上传/下载、新目录恢复及完整附件托盘接收验收，Debug/Release 模拟故障也已通过。见 [备份恢复](docs/BACKUP_RESTORE.md) 与 [Google Drive](docs/GOOGLE_DRIVE.md)。发布候选包由 scripts/Package.ps1 生成，生产构建排除测试入口；实际验收限制以状态文档为准。

窗口关闭或最小化后收起到托盘。新消息在窗口打开时使用任务栏提示，隐藏到托盘时才显示 Windows 通知；“Windows 通知”和“托盘图标闪烁”可以独立开关，重启后保留。正式数据部署及尚待现场验证的项目见 [部署说明](docs/DEPLOYMENT.md)。

Drop 与 dropserver 产品版本和 Windows 文件属性中的文件版本均为 1.0.3。窗口标题、版本查询、服务器信息和发布包名称使用同一产品版本；图标来自所有者更新的 Dropico.png 和 DropServerico.png。版本来源为 dropcommon/version.h，业务协议版本 1、服务器 schema 12、客户端 schema 5，升级前自动建立迁移快照；显示名使用可增量补齐的本地资料缓存。此版本供长期部署试用，现场验证范围继续按验收记录追踪。

默认应用数据均在程序旁：客户端 `Data\Drop`，服务器 `Data\DropServer`，不向 AppData/系统盘回退。`drop.exe data-directory` 与 `dropserver.exe data-directory` 只查看路径，服务器 `init` 初始化目录。部署到非 C 盘，可用服务器 `--data "E:\DropData\server"`、客户端 `--ui-root "E:\DropData\client"` 覆盖；服务安装保存确定的绝对路径。旧目录与 Google 授权不会自动搬动；先前真实授权是在旧默认目录验收，改目录后须显式指定旧路径或为正式新目录重新授权。

通信保留两个模式：不带证书启动服务器时使用 `http://` + PAKE 会话加密（默认端口 8080），带 `--certificate` 时使用 `https://` + TLS（默认端口 8443），HTTPS 不叠加 PAKE，不自动降级。两种模式都保留服务器和云备份存储加密。旧账号启用 PAKE 前须在本机执行 account-reset 并重新登录；操作步骤与传输格式见 [PAKE 说明](docs/PAKE.md) 和 [部署说明](docs/DEPLOYMENT.md)。

Google OAuth 项目所需的产品首页、隐私说明和服务条款位于 website/dist，自定义域名 drop.jsnowly.win 已生效，首页和隐私说明通过 HTTPS 公开访问检查。页面说明实际数据存储、drive.appdata 用途及使用条件，不包含 OAuth 配置、令牌或服务器数据。Google 正式发布、域名验证及实际数据目录重新授权步骤见 docs/GOOGLE_DRIVE.md；网站和 DNS 状态另见 docs/WEBSITE.md。
