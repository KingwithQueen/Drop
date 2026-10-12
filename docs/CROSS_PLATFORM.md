# Drop 1.0.3 Linux / macOS 客户端

本次实现原生图形客户端：Linux 使用 GTK4、Wayland 和 KDE StatusNotifierItem；macOS 使用 AppKit 和菜单栏图标。业务、数据库、附件和消息删除继续复用 Windows 的 C17 核心，版本保持 `1.0.3`。中央服务器继续使用既有 Windows 版本，本次不迁移服务器或生产数据。

## 已接入的功能

- 真实账号列表，自己置顶，查找已有账号；发送对象跟随当前会话。
- 按所有者确认的高密度布局连续显示消息，标题、正文、附件各占一行；支持正文搜索、日期过滤、分页、独立窗口完整正文查看及复制。
- 文件选择、复制文件和拖入生成带文件图标、名称、大小的草稿预览。固定附件区域左侧显示待发送文件，右侧显示所选消息附件，两个列表独立滚动；添加文件不改变输入框高度。添加最多 32 个、每个最多 10 GiB 的普通文件，后台先保存独立副本和持久队列，再清空草稿。保存失败保留草稿。
- 消息内附件归属各自消息，点击只选择；明确导出后才写入所选位置。原文件名只用于显示和导出，不参与内部缓存路径。
- 本机历史、分块上传下载、续传、实时 WS/WSS 通知、心跳、重连和兜底同步，后台工作可取消。
- 删除按发送账号授权，允许同账号其他设备删除，拒绝接收账号删除。离线意图持久排队，服务器确认后双方设备同步删除并清理受管理缓存；原文件、导出文件和旧备份保留。
- 独立配置与账号窗口、系统通知和未读图标提示开关，提醒设置持久保存。关闭消息窗口后保留后台同步，真正退出停止同步。

Linux 的主窗口在托盘服务不可用时保持可访问，不隐藏成无法恢复的后台进程。Mac 菜单栏菜单提供打开消息、同步、配置和真正退出。恢复后的不确定发送任务须通过「会话」菜单明确复核再提交。

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

已在用户指定的 Linux Wayland 和 Intel macOS 13.7.8 上原生构建、链接、运行。核心检查覆盖 Unicode、容量、URL、JSON、原子提交、目录锁、符号链接拒绝及附件摘要；实际系统秘密存储验证加解密和篡改拒绝。

隔离 Windows 服务器与两平台多个原生客户端完成 HTTP/PAKE、HTTPS/TLS、WS/WSS、中文正文、多块附件、完整保存、导出摘要、接收方越权拒绝、同账号其他设备删除和双方设备补齐。实际 GTK/AppKit 图形会话通过登录、8 条消息、消息内附件选择和复制失败保留草稿。测试不使用生产账号或改动用户剪贴板。

当前仍需现场观察长时间运行、睡眠/网络恢复、跨物理屏幕及不同缩放组合；系统最终是否展示通知由桌面设置决定。Apple Silicon、其他 Linux 发行版、内置登录后自动启动设置、Apple 签名与公证、Linux/macOS 服务器移植不属于本次已验证交付。可先通过系统的登录项机制启动客户端，不宣称内置自启动已完成。

当前结果和包校验记录集中在 `build/verification/ui-optimization-1.0.3.json`；失败尝试与随后修正保留，最终 PASS 记录及具体来源由汇总文件列出。本机包位于 `build/packages/Drop-1.0.3-linux-x86_64.tar.gz` 和 `build/packages/Drop-1.0.3-macos-x86_64.tar.gz`。
