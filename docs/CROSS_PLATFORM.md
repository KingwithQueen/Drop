# Drop 1.0.4 Linux / macOS 客户端

1.0.4 将原 Mac Objective-C 界面改为 C17，通过公开运行时使用同一套 AppKit 原生控件、系统菜单和异步文件面板；不再启用 Objective-C 编译语言。Intel Mac 已完成构建和实际原生窗口回归，Apple Silicon 目前仅通过 C17 语法检查，未链接或运行。Linux 保持既有 GTK4 C 界面，桌面布局沿用已确认设计。

本次实现原生图形客户端：Linux 使用 GTK4、Wayland 和 KDE StatusNotifierItem；macOS 使用 AppKit 和菜单栏图标。业务、数据库、附件和消息删除继续复用 Windows 的 C17 核心，版本统一为 `1.0.4`。中央服务器继续使用既有 Windows 版本，本次不迁移服务器或生产数据。

## 已接入的功能

- 真实账号列表，自己置顶，查找已有账号；发送对象跟随当前会话。
- 按所有者确认的高密度布局连续显示消息，标题、正文、附件各占一行；支持正文搜索、日期过滤、分页、独立窗口完整正文查看及复制。
- 文件选择、复制文件和拖入生成带文件图标、名称、大小的草稿预览。固定附件区域左侧显示待发送文件，右侧显示所选消息附件，两个列表独立滚动；添加文件不改变输入框高度。添加最多 32 个、每个最多 10 GiB 的普通文件，后台先保存独立副本和持久队列，再清空草稿。保存失败保留草稿。
- 消息内附件归属各自消息，点击只选择；明确导出后才写入所选位置。原文件名只用于显示和导出，不参与内部缓存路径。
- 本机历史、分块上传下载、续传、实时 WS/WSS 通知、心跳、重连和兜底同步，后台工作可取消。
- 删除按发送账号授权，允许同账号其他设备删除，拒绝接收账号删除。离线意图持久排队，服务器确认后双方设备同步删除并清理受管理缓存；原文件、导出文件和旧备份保留。
- 独立配置与账号窗口、系统通知和未读图标提示开关，提醒设置持久保存。关闭消息窗口后保留后台同步，真正退出停止同步。

Linux 的主窗口在托盘服务不可用时保持可访问，不隐藏成无法恢复的后台进程。Mac 菜单栏菜单提供打开消息、同步、配置和真正退出。恢复后的不确定发送任务须通过「会话」菜单明确复核再提交。

Linux 消息按从旧到新显示，最新消息位于底部；打开或切换会话时定位到底部，阅读上方历史时后台刷新保留滚动位置。账号行按稳定 ID 复用，连续切换只接受最后一次选择对应的结果；后台按两页预算补齐正文，并在同步步骤之间让出线程处理本机操作。

macOS 采用相同的消息顺序和视图序号约束；AppKit 用户行按账号 ID 增量维护，未变化的正文不重新写入文本存储，保留历史阅读位置和选区。添加附件继续使用固定高度列表，不挤占输入区。

后台按实际截止时间继续处理续传和补页，同一线程内复用 HTTP 连接及 TLS 会话，减少反复连接与握手；认证头和请求内容逐次释放。发布包的 `BUILD-INFO.json` 记录产品版本、构建说明及程序摘要；同一产品版本的不同构建须结合该文件或程序 SHA-256 区分。

## 数据与登录

客户端默认使用程序旁 `Data/Drop`。macOS `.app` 使用应用包外侧、与应用同目录的 `Data/Drop`，不向签名后的应用包内部写数据。可用 `--ui-root ABSOLUTE_DIRECTORY` 覆盖；目录不可写时明确失败，没有系统目录回退。

每个设备拥有自己的 SQLite，仍按服务器固定 ID、账号 ID 分目录。目录权限为当前用户 `0700`，普通文件为 `0600`；同一目录有独占锁，不穿越符号链接操作受管理文件。升级时继续使用同一数据目录；不要复制 Windows 的 DPAPI 文件作为 Unix 登录凭据。

Linux 随机主密钥存入 Secret Service；macOS 存入本机 Keychain、不启用 iCloud 同步。`session.secret` 保存 XChaCha20-Poly1305 认证加密后的凭据，绑定文件路径；明文密码仅用于登录，不落入公开配置。系统秘密存储未解锁或不可用时明确失败，不回退成普通密钥文件。数据目录改名或搬到另一台机器后需要重新登录，历史和附件可以保留。

macOS 非图形 SSH 会话可能返回 Keychain `-25308`；从实际图形会话启动 Drop 进行登录。Linux SSH 调试 Secret Service 时需使用当前桌面的用户 D-Bus 地址。本项目专用 SSH 配置与机器目录见 [环境交接](HANDOFF_LINUX_MACOS_20261012.md)，私钥不会进入源码同步或包中。

通信保持两种明确模式：`http://` 使用 PAKE 认证加密及 WS，`https://` 使用证书验证及 WSS、不叠加 PAKE。无自动降级，不发送 HTTP 明文密码或 Bearer 令牌。私有 CA 在配置窗口明确选择；Mac 发布包携带公开根证书集合，私有 CA 优先。

## 原生构建

在各自开发目录运行：

```sh
sh scripts/Bootstrap-Unix.sh
sh scripts/Build-Unix.sh Debug
sh scripts/Build-Unix.sh Release
python3 scripts/Package-Unix.py Release
```

Mac 使用 `/opt/local/bin/python3.13`。脚本仅在自身进程中设置 MacPorts PATH 和 UTF-8 环境，不修改全局配置。Windows 继续使用 `drop.slnx`、`scripts/Build.ps1`。

`checks/`、`tests/`、检查脚本和 `docs/design/` 设计稿只在本机保留，不进入 Git 源码发布。完整的本机检查文件存在时，Unix 构建默认编译并运行测试；下载的源码缺少这些文件时，构建脚本自动关闭测试目标，直接使用 CMake 也默认关闭测试。此时不要指定 `-DBUILD_TESTING=ON`。产品图标、原生界面样式和构建、打包、安装脚本继续发布。

Bootstrap 固定 cJSON 1.7.19、libsodium 1.0.22、SQLite 3.53.4，并核对现有 vcpkg baseline 的 SHA-512。OpenSSL、Zstandard、libcurl、GTK4/libsecret 使用已经配置的本机开发包，构建时记录实际版本；要求 libcurl 含 WS/WSS。生产 `drop` 排除独立的 `drop-client-fixture`、`drop-portable-check` 和 `drop-gui-check`。

Linux 包针对本次 openSUSE Tumbleweed x86_64 环境，运行依赖 GTK4、libsecret、libcurl、OpenSSL 和 Zstandard，不宣称所有发行版通用。Mac Intel 包包含 `.app`、非系统动态库、图标、公开 CA、许可证及随附 GNU 库的原始源码与 MacPorts 构建文件，核查没有 `/opt/local` 动态库依赖；使用本地 ad-hoc 签名，尚未使用 Apple Developer ID 签名及公证。

解压到当前用户可写目录。Linux 运行包中的 `./drop`；Mac 打开 `Drop.app`。首次使用在「配置与账号」填写服务器地址和账号，私有 CA 可在同一窗口选择。默认数据目录与程序并列，升级须保留 `Data/Drop`。Mac 未经公证，系统是否允许首次打开取决于本机应用安全设置。

## 验证范围与后续事项

2026-10-20 Mac 专项检查发现此前 AppKit 仍按最新在前绘制，每次快照会重建列表及正文，且未拦截迟到视图；现已修正。真实 Release 窗口通过 8 条消息顺序、最新消息底部、24 次单次选择、20 次快速切换、24 次迟到快照拒绝、12 次无正文重写刷新和 30 次非输入区滚轮检查，单次选择最长 493 毫秒；用户名可见、32 个附件固定布局及完整正文也通过。Mac 实际 16 MiB 附件同步对照为 20991 与 5150 毫秒，两份落盘摘要一致。这是单次隔离负载结果；证据见 `build/verification/macos-followup-20261020/`，窗口截图及结果见 `build/verification/native-ui-ef040fd6d1b7/`。

2026-10-20 已核对现场仍运行 10 月 12 日旧程序，并在所有者授权后完成原路径更新及重启，数据和登录凭据保留。Linux/macOS 构建、自测、原生窗口及 TLS 复用/证书拒绝检查通过；一次 16 MiB 附件同步对照从 14978 毫秒降为 5062 毫秒。现场安装和性能证据见 `build/verification/linux-followup-20261020/`，本轮 Linux 窗口回归见 `build/verification/native-ui-cada885d72b0/results.json`。

2026-10-15 Linux 排序与切换修复通过实际 Wayland Release 窗口回归：8 条唯一标记消息顺序、底部定位、24 次单次选择、20 次快速切换及用户行稳定性通过，本轮单次选择最长 247 毫秒。32 个附件的固定布局、完整正文和失败时保留草稿仍通过。Linux/macOS Debug、Release 构建与核心自测、Mac 原生窗口的发送及附件流程通过；记录见 `build/verification/linux-ui-20261015/results.json`。

已在用户指定的 Linux Wayland 和 Intel macOS 13.7.8 上原生构建、链接、运行。核心检查覆盖 Unicode、容量、URL、JSON、原子提交、目录锁、符号链接拒绝及附件摘要；实际系统秘密存储验证加解密和篡改拒绝。

隔离 Windows 服务器与两平台多个原生客户端完成 HTTP/PAKE、HTTPS/TLS、WS/WSS、中文正文、多块附件、完整保存、导出摘要、接收方越权拒绝、同账号其他设备删除和双方设备补齐。实际 GTK/AppKit 图形会话通过登录、8 条消息、消息内附件选择和复制失败保留草稿。测试不使用生产账号或改动用户剪贴板。

当前仍需现场观察长时间运行、睡眠/网络恢复、跨物理屏幕及不同缩放组合；系统最终是否展示通知由桌面设置决定。Apple Silicon、其他 Linux 发行版、内置登录后自动启动设置、Apple 签名与公证、Linux/macOS 服务器移植不属于本次已验证交付。可先通过系统的登录项机制启动客户端，不宣称内置自启动已完成。

当前 Linux 现场更新与同步优化记录位于 `build/verification/linux-followup-20261020/`，Mac 专项修正及包校验记录位于 `build/verification/macos-followup-20261020/`；此前三平台界面优化记录保留于 `build/verification/ui-optimization-1.0.3.json`。本机修正包为 `build/packages/Drop-1.0.3-linux-x86_64-20261020.tar.gz` 和 `build/packages/Drop-1.0.3-macos-x86_64-20261020.tar.gz`，各有省略日期的同内容副本。当时版本仍为 1.0.3，包内 `BUILD-INFO.json` 区分构建并记录实际程序摘要；Mac 专项未替换用户现有安装或启动真实账号会话。

所有者随后授权直接更新 Mac 安装。现场未找到独立安装的应用或真实登录数据，已将验证过的应用安装到 `/Users/owl/程序/Drop/Drop.app` 并在图形会话启动；程序旁数据使用 `/Users/owl/程序/Drop/Data/Drop`。已核对实际进程的可执行文件路径、映射文件身份、SHA-256 和签名与本轮发布包一致，记录见 `build/verification/macos-followup-20261020/installation.json`。首次使用需在「配置与账号」登录；开发源码及构建目录保留。
