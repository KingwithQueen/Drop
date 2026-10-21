# 当前验收记录

更新时间：2026-10-21。以下为当前结果；后面的历史记录保留各阶段当时的状态，不代表当前未完成范围。

## 1.0.4 代码规范及开发准备（2026-10-21）

项目自有程序代码均为 C17，Mac 的 Objective-C 界面已替换为 `platform/ui_macos.c` 和显式类型的 C 运行时边界 `platform/macos_bridge.h`。代码按根目录 180 列格式检查，维护中的 C/H 注释通过中文扫描；第三方源码保持上游内容。源码同步、许可证打包及 Windows 文档打包明确排除本机测试和设计稿。

证据目录为 `build/verification/pre-android-104/`，其中 `results.json` 汇总最终版本、各平台构建、版本资源和原生回归，`source-manifest.json` 记录实际同步清单。Mac 原生控件使用隔离合成快照验证：8 条消息从旧到新、24 次原生单次选择、20 次快速切换、24 次迟到快照拒绝、稳定账号单元格、12 次无正文重写刷新、选区/滚动保留、32 个附件固定布局、完整正文、私有剪贴板文件解析及异步文件面板关闭。该项不覆盖真实账号登录、跨设备收发、系统通知实际展示或长时间运行；既有 C 后台未因界面语言转换而修改。Apple Silicon 仅通过 C17 语法检查，未链接或运行。

## 1.0.3 macOS 排序、切换与同步专项（2026-10-20）

此前 AppKit 同样使用最新在前的绘制顺序，普通后台快照会重载用户表及全部正文，未消费共享后台的视图序号。现改为反向绘制、保持原历史行与附件范围映射，用户表按账号 ID 增量插入、移动及移除，已有单元格只更新变化文字；无正文变化时保留文本存储、选区及滚动位置。账号、筛选、分页、登录任务携带序号，迟到快照不能回退选择，但仍消费发送完成及通知。新视图到达前禁止操作旧会话内容。Mac 使用共享后台的连接复用及同步等待修正，产品版本保持 1.0.3。

独立 Mac Debug、Release 构建及核心 CTest 通过。真实 AppKit Release 窗口记录 `build/verification/native-ui-ef040fd6d1b7/macos.json` 覆盖：8 条本轮唯一消息从旧到新、最新底部、24 次单次选择（最长 493 毫秒）、20 次快速切换、用户行与单元格身份稳定、24 次迟到快照拒绝、12 次统计变化不重写正文且保留选区及滚动位置、30 次直接送往非输入标签的原生滚轮事件不请求整窗重绘；32 个附件不改变输入区和消息区尺寸、完整正文、消息附件选择及复制失败保留草稿继续通过。截图已复核用户名和最新消息显示，没有移动用户鼠标、修改剪贴板或操作生产账号。截图复核曾发现新增单元格弱引用提前释放标签，已修正并补充用户名可见断言；先前结果不代替最终回归。

同一隔离 TLS 端点的六次请求，旧 Mac 后台六条连接、修正版一条；不同认证、空认证、GET/POST 互不残留，不可信 CA 在已有连接后仍拒绝。与 10 月 15 日后台对照，同一服务器的真实 16 MiB 附件同步为 20991 与 5150 毫秒，均验证落盘长度及 SHA-256 `341aacac661ccb210720bedaa9ead5d668fe5ea41a73532fc147c71e34040df1`；未注入延迟，属于本轮单次测量。初次跨两个测试可执行程序读取凭据被 Keychain 授权交互阻塞，记录保留于 `performance-attempt-1/`；最终由同一测试程序登录并读取，不修改产品钥匙串授权策略。证据集中于 `build/verification/macos-followup-20261020/`，包含构建、性能、发布包摘要/清单/签名/解压启动检查。此次未替换用户现有安装，长期运行、睡眠恢复和 Apple Silicon 不属于本轮验证范围。

## 1.0.3 Linux 实际安装核对与同步优化（2026-10-20）

现场 `/home/owl/程序/Drop/drop` 的运行中映像与磁盘文件均匹配 10 月 12 日旧包，SHA-256 为 `dad0d8bad632fa0a3a74b7a71d05707ab9a6a67097dba167d353baec6d40b75b`；与 10 月 15 日修正包中的 `491abd560eb0133c8a0d6e242384cb1811d99d930292ae3ac18c7ecf4e3a35bc` 不同。因此前次隔离回归不能代表现场客户端已更新。所有者明确授权备份、更新并重启此应用后，已在原路径替换并核对新进程，摘要为 `6a9f764c35c889f97ecc19231c7327522a18b8e25048e62bc0278510cc634a18`。数据目录及加密凭据文件保留，三个本机数据库完整性检查均为 `ok`，历史条数分别为 9、0、0；安装目录 `.update-backups/20261020/` 中的旧程序已按所有者后续要求清除，删除前后确认当前程序及运行进程摘要不变、数据目录保留。

新增的同步优化只复用同一线程内的 HTTP 连接、DNS 与 TLS 会话，每次请求仍独立创建并释放认证头和正文设置，WebSocket 独立拥有连接。调度等待改为实际的下次同步截止时间，并继续推进未完成的资料分页。Linux/macOS Debug、Release 和核心 CTest 通过；隔离 TLS 探针确认六次请求从六条连接降为一条，GET/POST、空认证和两个测试令牌互不残留，连接复用后更换为不可信 CA 仍拒绝，再使用正确 CA 可恢复。

同一隔离服务器的 16 MiB 附件与相关状态同步对照，10 月 15 日后台为 14978 毫秒，本轮为 5062 毫秒；这是本轮环境和负载下的单次结果，不代表所有网络的固定倍数。HTTP 对照额外模拟每次新连接 80 毫秒等待，与上述实际附件结果分开记录。实际 Wayland/AppKit 窗口回归见 `build/verification/native-ui-cada885d72b0/results.json`：Linux 消息从旧到新、最新位于底部、24 次单次选择、20 次快速选择、用户行稳定及原有附件/草稿功能通过，单次选择最长 214 毫秒；Mac 原生收发回归通过。总证据位于 `build/verification/linux-followup-20261020/`，包含原运行文件身份、性能对照及实际安装核验。产品版本保持 1.0.3；发布包增加构建说明文件，并提供带日期的同版本文件名，减少混淆。

## 1.0.3 Linux 消息排序与会话切换（2026-10-15）

Linux 绘制改为从旧到新，保留最新页查询及原有分页；切换会话后在 GTK 重新计算文字高度后定位到底部。用户列表按账号 ID 复用现有行，内容未变化时不清空消息文字。视图命令带本机序号，迟到快照不能覆盖最新选择，发送完成与通知仍独立消费。队尾连续的视图请求合并，但不跨越发送、登录、删除等任务；同步步骤之间优先处理交互，正文每轮预算与 Windows 一样为两页。

实际 Wayland Release 窗口回归记录为 `build/verification/native-ui-3c754c183da6/results.json`：8 条唯一消息从旧到新、最新在底部、24 次单次选择和 20 次快速来回切换通过；本轮单次切换最长 247 毫秒，账号行对象及选中目标保持稳定。32 个草稿附件不改变布局、完整正文、附件选择和复制失败保留草稿继续通过。截图已确认显示本轮 1 至 8 条消息及最新消息的底部位置；没有操作用户剪贴板。

Linux/macOS 均在独立源码目录通过 Debug、Release 构建及核心 CTest；共享后台的 Mac 原生发送、附件和草稿回归记录为 `build/verification/native-ui-12093ec0217c/macos.json`。最初两次 Linux 回归在快速切换后的滚动定位失败：GTK 尚未完成文字布局，使用旧滚动范围定位会被拉回上方；修正后严格检查通过，失败记录保留。汇总位于 `build/verification/linux-ui-20261015/results.json`。本轮不把单次网络请求自身的超时、其他桌面组合或长期运行列为已验证范围；没有替换正在运行的正式客户端或修改真实账号和数据。

## 1.0.3 三平台界面优化（2026-10-12）

Windows x64 / Win32 Release 各通过 30 项滚轮与蓝色栏居中检查，以及 364 项独立窗口、125% / 150% / 175% 和短屏布局检查。静态提示及窗口背景的无效滚轮不触发布局或背景擦除；到底部边界时也不重复重绘。两架构附件专项检查 0 / 2 / 32 个附件时输入区、消息区、对象条和两附件槽尺寸不变。真实 TLS 下载期间可打开独立完整正文窗口，关闭后正文缓存保留，已读优先、选择稳定、WSS 通知及离线导出回归通过。

Linux Wayland 与 macOS AppKit 在真实桌面运行独立测试目标，分别发送本轮平台唯一标记的 8 条消息、选择消息附件、添加 32 个草稿附件、明确打开完整正文窗口，并确认输入框和消息区没有位移或收缩；文件复制失败仍保留草稿。没有注入用户桌面的全局滚轮或改变桌面缩放；两平台的滚轮处理通过原生容器路径源码检查，未发现 Windows 的强制整窗布局路径。最终截图、版本检查、互通及解压启动记录集中在 `build/verification/ui-optimization-1.0.3.json`。

## Linux / macOS 原生客户端（2026-10-12）

Linux x86_64 Wayland 与 macOS 13.7.8 Intel x86_64 完成 C17 原生构建和核心检查，两个系统的实际秘密存储完成认证加密、解密及篡改拒绝。两平台各三个隔离设备连接 Windows 测试服务器，HTTP/PAKE 与 HTTPS/TLS、WS/WSS 通知、多块中文附件、完整保存、显式导出摘要、接收方删除拒绝、同账号其他设备删除、双方全部设备补齐及原文件/导出保留通过。

`checks/gui_native.py` 在实际 Wayland/AppKit 会话运行独立 `drop-gui-check`，复用产品控件及后台，验证图形登录、8 条发送消息、附件选择和源文件在排队前消失时保留草稿。截图仅含隔离测试数据；未改变用户剪贴板。早期 Mac 默认线程栈不足及 Linux 锁屏时的空白截图不算验收；所有者解锁 Linux 后重新运行，最终截图已确认连续显示十余条短消息及附件名称、保存状态。Windows 共享代码同时进行原有构建与自测回归。最终证据集中于 `build/verification/unix-port-1.0.2.json`，未把系统通知是否展示、其他机器缩放或 Apple Silicon 列为已验证。

## 1.0.2 按账号删除消息（2026-10-12）

原生界面、独立账号及多个实际客户端验证同账号跨设备删除，接收方删除入口禁用且服务器拒绝接收方和第三方的请求；取消确认保留消息，重复删除不重复生成事件。在线双方设备移除正文、详情、未读和附件缓存，离线设备恢复登录后补齐，新设备只接收删除身份而不下载正文或附件。离线删除在真正退出及服务器重启后继续提交，发给自己的消息也适用。

- 正式 x64 HTTPS 删除专项 16 项 PASS，包括旧请求重放不复活内容、晚到保存确认、安全保留原始及导出文件、被占用缓存释放后重试回收、损坏隔离副本回收、异常删除页事务回滚、目录穿越缓存名拒绝，以及超过 50 条删除的连续分页及原生客户端补齐。记录 message-deletion-x64-tls-10f7f732da9d26ccb60c8176/results.json。
- 正式 Win32 HTTP/PAKE 删除专项 10 项 PASS，message-deletion-Win32-pake-913d208d50156cbd4ea5df25/results.json；包含按账号跨设备权限、确认取消、在线删除、被占用附件及损坏副本回收、离线重连、离线删除与真正重启，以及删除后继续发送。
- 实际此前 schema-11 服务器与非空 schema-4 客户端升级专项 3 项 PASS，deletion-migration-x64-bfbfcedb808a17cd6981e6cb/results.json。新客户端连接旧服务器正常收发、禁用删除；服务器升级留加密快照，本机升级留完整快照，原消息身份、正文、附件和免密码恢复保留；升级后的旧消息通过原生界面在运行中的服务删除。
- x64 Debug 完整 HTTPS/WSS 网络回归 47 项 PASS，integration-x64-Debug-中文-5f424bcd6d224fb991ee7777ef2d6762/results.json。覆盖附件续传、配额、事务回滚、非空旧客户端迁移、认证及证书边界。
- 两架构 Release DPI 各 333 项 PASS，ui-1.0.2-x64-Release-6afb0efc24244b0cbeb5548f94b4e729/results.json、ui-1.0.2-Win32-Release-7f826174255c4d5885c3f0b92951f412/results.json。覆盖删除按钮边界、125%、150%、175% 原生字体、独立双窗口及短屏滚动；没有修改桌面缩放或移动真实多屏。
- 两架构正式阅读与刷新各 8 项 PASS，ui-responsiveness-x64-fa8d60b0ed15f07f7fa215ff/results.json、ui-responsiveness-Win32-952fe5f07d1da46c50e620d0/results.json；完整正文、已读优先、真实 WSS 通知、20 次选择和同步不销毁或重排用户行、文字选择稳定及服务器停止后的本机导出保持可用。
- 两架构正式登录与恢复各 6 项 PASS，production-x64-6790aac14959bdd7ac4b166e3c0a1fd7/results.json、production-Win32-a591e7afe4fe98d96ca0edbefb3aa20a/results.json；真正重启客户端及服务器后免密码同步，明确注销后保持未登录并保留本机历史。

原始结果均在 build/tests，源码、十二个程序的三段可见版本和哈希、57 个项目 C/H 文件格式结果、截图、C 自测及四个独立 RAR 的实际解压记录汇总于 build/verification/message-deletion-1.0.2.json。早期隔离测试的选择等待、重新登录判断和 TLS 代理关闭问题已修正并保留失败记录；未因此放宽认证、事务或缓存路径校验。逻辑删除不宣称擦除旧备份、用户导出或数据库介质旧字节。实际跨物理电脑、物理多屏移动和长期运行仍需部署观察；没有修改实际安装、账号、服务或云数据。

## 1.0.2 发送对象、附件预览与配色：历史记录（2026-10-12）

本轮移除客户端的「全部消息」入口，将发送对象绑定至所选真实账户，自己会话使用明确提示；新增文件图标、原始名称与大小的待发送附件预览、文件复制粘贴及逐个移除，并统一蓝灰配色。版本仍为三段 1.0.2，服务器六个构建的二进制与前次相同，协议及数据库 schema 保持原样。

- x64 Debug HTTPS/WSS 全流程 47 项 PASS，build/tests/integration-x64-Debug-中文-5ea37baf4c80402aac26a6c1610f53e4/results.json。原生发送已验证当前用户选择，对方与自己历史隔离，无「全部消息」行；分页按自己会话的实际消息数检查，登录后先等正文同步再检查下载进度。
- 两架构原生附件专项各 8 项 PASS：x64 Release 的 ui-compose-x64-Release-bf89258bcbb6c8c3b9f52f6e，以及 Win32 Release 的 ui-compose-Win32-Release-ce1a7688a99c117dda204272，均位于 build/tests。真实文件选择和 CF_HDROP 复制粘贴显示名称、大小和数量，保留正文，去重并拒绝目录；普通文字粘贴、逐个移除、32 个附件上限、20 次快速添加/清空与过期图标防护、125%/150%/175% 图标尺寸和名称保留通过。实际 native-compose-preview.png 已查看。后续复制测试只有在可完整恢复当前剪贴板格式时才操作，默认复验改用原生文件选择器。
- x64/Win32 Release DPI 各 328 项 PASS，分别记录 ui-1.0.2-x64-Release-b80edc988da54e738041a51b391703a2 和 ui-1.0.2-Win32-Release-74bc50149c7f407995fe403c9ab5bfa5。覆盖原生字体、RichEdit 格式、控件边界、独立配置窗口、设置持久保存与短屏操作。非整数 DPI 检查作用于隔离原生窗口，未修改桌面缩放；真实物理多屏移动继续属于现场验收范围。
- 正式登录与恢复各 6 项 PASS：production-x64-a7ec220fa21a8cc207fbed1abf9fadbd 与 production-Win32-b6a08538ea47e37680fb34f0bfe9e84b。x64 HTTPS 与 Win32 PAKE 实际收发、程序与服务器重启后的免密码恢复、明确注销删除凭据并保留历史通过。阅读与刷新专项各 8 项 PASS：ui-responsiveness-x64-168949647ebc85aa52a25904 与 ui-responsiveness-Win32-8fa5fe5a5c2fecabd008c9ae，保持已读优先、真实 WSS 通知、文字选择和用户行稳定，以及服务器停止后的本机附件导出。

本轮原始测试、实际截图、55 个项目 C/H 文件、源码与十二个程序哈希、三段版本及发布包真实解压记录集中于 build/verification/client-compose-1.0.2.json。早期失败来自旧测试假定汇总视图、登录成功即正文已同步，以及 Python OLE 剪贴板回调异常；修正测试后分项和完整网络回归通过。环境暂时无响应期间的超时及后续不能复制当前复杂剪贴板格式的拒绝记录保留，没有据此修改客户端同步逻辑。没有操作已安装客户端、实际账户、运行中的服务器服务或 Google 云数据。

## 1.0.2 版本更新与存储说明：历史记录（2026-10-11）

本轮仅更新版本和说明。两架构 Debug、Release、正式构建完成；十二个程序的版本查询、可见 FileVersion 和 ProductVersion 均为三段 1.0.2。Windows 固定资源及 manifest 的内部末尾零继续满足系统格式，不是显示用迭代号。53 个 C/H 文件格式检查通过；逐文件与前次 client-responsiveness-1.0.1.json 比较，只有 version.h 的产品及固定资源版本常量改变，业务源码相同。

四个 x64/Win32 客户端及服务器独立 RAR 通过真实解压、文件清单 SHA-256、与正式程序的匹配、部署文档和配置脚本一致性、测试入口排除检查。原始路径、源码和二进制哈希、版本结果与归档验收集中于 build/verification/version-1.0.2.json。当前版本继续使用协议 1、服务器 schema 11、客户端 schema 4，无新迁移。

附件随机 ID 的 .part/.file 缓存路径、原名导出的机制，以及服务器未提供消息删除，均按当前源码核对并写入部署说明。本轮没有更改这些行为，没有重复下方 1.0.1 的网络/交互/DPI 回归，也没有操作实际账户、运行中的服务、Google 云数据或线上网站。

## 1.0.1 客户端阅读与刷新优化（2026-10-11）

- 正式 x64/Win32 交互专项各 8 项 PASS。使用两个账户及三个原生客户端，阻塞接收方真实 TLS 附件块请求，检查完整正文立即显示、用户控件可用、已读先于下一块到达服务器、真实 WSS 通知发送方和同账号另一设备；随后续传并逐字节校验。外部 Windows 事件钩子确认 20 次稳定消息选择及同步没有用户行销毁或重排；状态标题和新增消息不破坏原正文的文字选择。服务器停止后，原生文件对话框仍能从保留缓存导出附件。记录 ui-responsiveness-x64-538decb74f4850203b6d80ce/results.json、ui-responsiveness-Win32-9e8185f47bb6cc9880abe10f/results.json，均位于 build/tests；实际截图 native-receipts-and-cache.png 已查看。
- 本机专项中正文显示分别约 0.016 秒及一个采样间隔以内；受控附件块放行后，中央收到已读分别约 0.703 秒及 1.031 秒，发送方与同账号另一设备更新约 0.828 秒及 1.234 秒。此记录只证明该隔离场景的顺序与响应，不代表广域网保证；当前块未结束时中央回执仍需等待，本地阅读无需等待。
- x64 Debug HTTPS/WSS 47 项 PASS，integration-x64-Debug-中文-d4e8a6ec7be143dca2c743becc56d56f/results.json；Win32 Release HTTP/PAKE 13 项 PASS，pake-Win32-Release-中文-2c2164d32c5fcc5b/results.json。压缩回归等待已进入本机队列的消息完成实际传输；发送后控件提前恢复可用，不再假定远端同步与按钮恢复同时完成。
- 两架构 Release DPI 各 323 项 PASS，ui-1.0.1-x64-Release-d83166fbf09a40cd8e1e7339e6fbf215/results.json、ui-1.0.1-Win32-Release-c113dc4fdbbe47efafe46ef13fbefc46/results.json。覆盖 125%、150%、175% 原生字体与 RichEdit 格式、控件边界、独立双窗口及短屏；仅改变隔离窗口，真实物理多屏移动仍待现场验证。
- 两架构正式登录与恢复各 6 项 PASS，production-x64-b213e0a83181fefb9ecd7be30cd0dc3e/results.json、production-Win32-583035b810494dcd7c6bc341cb982020/results.json。客户端与服务器真正重启后免密码收发；明确注销删除凭据而保留历史。
- x64/Win32 Debug/Release 构建及原有 C 自测 PASS，两个正式构建排除测试入口，53 个项目 C/H 文件格式检查通过。十二个程序的 --version、可见文件版本及产品版本均为三段 1.0.1。服务器二进制保持原账户管理版本，协议及数据库 schema 不变。本轮无需更新或重启已部署服务器。

本轮完整证据及独立发布包实际解压校验集中在 build/verification/client-responsiveness-1.0.1.json。慢附件代理的早期失败来自测试连接关闭和转发设置，修正隔离代理后复验通过；失败记录保留，未据此宣称网络长期稳定。没有操作真实账户、服务器服务或 Google 云数据。

## 1.0.1 账户与在线管理：历史结果（2026-10-11）

- 正式 x64 HTTPS 和 Win32 HTTP/PAKE 各 13 项 PASS：原生客户端自动补齐名称、WebSocket 改名同步、128 字节完整显示名、畸形输入无变更、停用/启用、设备撤销、密码重置、在线配额/备份/清理，以及删除后历史内容和附件原字节保留。同名新账户得到新 ID 且不能读取旧历史；真正重启客户端和服务器后免密码登录仍可用；删除自己账户后保留本机副本并禁止继续发送。记录 build/tests/account-management-x64-tls-ea3a3adcaf054ba4ee15aa8ee369a78d/results.json、build/tests/account-management-Win32-pake-c4941d3ecd69f3dc538426ac68fa194f/results.json。
- 独立本地管理通道验收 8 项 PASS，覆盖错误密钥、错误方向、重复 JSON、失配挑战、旧请求重放、超长帧及客户端早退后的恢复。账户目录和客户端资料各 105 个已知身份的分页通过；资料 API 校验授权及 ID 范围，不提供陌生账户枚举。记录 build/tests/admin-channel-x64-d2be8d54043f9c67682494597b152b07/results.json。
- x64/Win32 Debug/Release 四配置构建及 C 自测 PASS；迁移新增 schema 10→11，保留账户、PAKE 资料及消息，并继续覆盖旧 schema 1/2/5。两个生产构建排除诊断入口，53 个项目 C/H 文件格式检查通过。十二个 EXE 的 --version、可见文件版本和产品版本均为三段 1.0.1，逐个哈希见 build/verification/account-management-versions-1.0.1.json。
- 原功能回归：HTTPS/WSS 47 项、Win32 HTTP/PAKE 13 项、x64/Win32 DPI 各 323 项、正式登录状态各 6 项、密码确认与账户创建各架构各 7 项，均 PASS。GUI 测试先等待原生窗口初始化完成，再操作按钮；账户查找断言按服务端显示名核对标题，同时核对发送仍使用登录名。程序不因这两处测试修正改变已保存登录行为。DPI 检查覆盖 125%、150%、175% 原生字体与 RichEdit 格式、像素边界、独立双窗口及短屏可达性。
- Drive 模拟验收 14 项 PASS，记录 build/tests/drive-Release-f076bfa408d84406be351f3e7e5f7977/results.json。新增持续运行实例的 status/list/upload/download/delete，与后台镜像共享授权和云任务锁；前两次新增测试误选先前已删除的任务，修正为本轮新备份后通过，失败记录保留。没有对真实 Google 云数据操作。
- Windows LocalSystem 身份下的新在线管理专项因提权提示取消而未执行，本轮未创建测试服务或更改已有服务。账户在线行为已在真实普通用户服务器进程验证。DPI 仅改变隔离窗口，真实物理多屏移动、广域网现场及本轮真实 Google 管理仍不属于验收结论。

本轮二进制、C/H 源码、原始测试路径、发布包真实解压校验和哈希集中记录于 build/verification/account-management-1.0.1.json；使用方式见 [账户与在线管理](ACCOUNT_MANAGEMENT.md)。以下记录对应先前构建。

## 登录状态复验与缓存清理：历史记录（2026-10-11）

正式 x64 HTTPS 与 Win32 HTTP/PAKE 程序各 6 项 PASS，记录 production-x64-f167283e1c5845f7f8708034c1bb2742/results.json、production-Win32-d0335b8e58a933737b65b4e2432ccb69/results.json。除既有生产入口隔离、密码隐藏、实际原生收发和空闲观察外，新增客户端与服务器均重启后的免密码登录及真实新消息同步；主动注销删除 session.dpapi，再次启动未登录但保留历史。首轮新增断言误将 accepted 记录计入未完成发送，已改为仅统计 queued，失败结果保留供追溯，程序逻辑没有为此修改。此次没有实际重启 Windows 或变更真实账号，自动恢复使用同一 Windows 用户及同一数据目录。

已补充登录与升级说明：升级须继续使用原 Data 或 --ui-root，服务器凭据当前有效期 30 天。清理仅删除旧发布包及可重建缓存，结果、日志、截图、真实 Google 授权和已安装依赖保留；历史结果证明原验收范围，已删除的临时数据不能据此作为仍存在的现场数据。当前清理和发布包校验记录见 build/verification/login-state-cleanup-1.0.1.json。

## 1.0.1 原生主界面与消息内附件：历史记录（2026-10-11）

- 所有者确认布局后实现原生菜单、工具栏、用户/消息分栏、拖动分隔条与状态栏；消息显示秒级时间、全部原始附件名、大小和保存状态。点击文件名称关联到正确附件，由明确导出命令保存。长正文有完整查看区，定时同步时保留未变化文本的选择。
- x64 Release HTTPS/WSS 全流程 47 项 PASS：新增真正点击第二个附件名并导出零字节文件，检查两个文件名都显示于消息中；精确用户查找、名称缓存、自己置顶、用户/自己会话隔离和恢复全部消息均通过。原有业务、搜索、分页、托盘、自动启动、证书与恢复回归通过；原始记录 build/tests/integration-x64-Release-中文-83d48678e3464cdd8fb6d6080426b705/results.json，实际截图 tray-ui.png 已查看。
- x64/Win32 Release 配置与 DPI 回归各 323 项 PASS，检查 96→120→144→168→96 DPI 的实际原生字体与 RichEdit 默认格式、所有控件边界/重叠、两窗独立 DPI、设置保存/重启及短屏操作可达。修正高 DPI 下附件表格偏矮与状态栏的一像素舍入越界；测试不更改桌面缩放，真实物理多屏移动继续待现场验收。
- 四个诊断配置构建及原有 C 自测通过；生产构建、真实原生登录/收发、十二程序的三段 1.0.1 版本和新发布归档随本轮重新核对。源码、二进制、全部测试路径及归档哈希汇总于 build/verification/main-ui-attachments-1.0.1.json，具体边界与结果以该汇总及原始记录为准。

## 1.0.1 文件版本显示纠正与 Win32 设计：历史记录（2026-10-11）

- 产品版本仍为所有者指定的 1.0.1；FileVersion 显示字符串由四段纠正为 DROP_VERSION，与 ProductVersion 一致。系统固定数值资源和程序集身份的末尾零仅符合 Windows 内部格式。
- x64/Win32 Debug、Release、生产构建共十二个 EXE 重新核对：Windows 文件版本、产品版本与 --version 均为 1.0.1。实际 EXE 哈希和逐项结果见 build/verification/versions-three-part-1.0.1.json；修改的 version.h 通过 clang-format 检查，工作区 diff 无空白错误。
- 传统 Win32 提案 client-main-win32-1.0.1.png / .svg 已生成并查看，采用菜单、工具栏、用户列表、密集消息、附件表格与状态栏。字体和线条为直接生成的图稿；图中数据、控件及快捷键仍是提案，没有据此宣称原生主界面已经实现。
- 本轮未改变业务或 DPI 逻辑，不重复下节已有功能回归；只重新构建、检查版本资源及发布归档。当前包、源码、图稿与实际解压校验结果汇总在 build/verification/win32-design-version-1.0.1.json。正式主界面待所有者确认后编写。

## 1.0.1 配置与非整数 DPI：初次交付原始结果（2026-10-11）

- x64/Win32 Debug/Release 构建及原有 C 自测通过；新 C 文件 ui_windows.c 及修改文件符合根目录格式规范。
- x64 Release HTTPS/WSS 全流程 45 项 PASS，包含独立配置窗口的连接测试、账号登录和返回消息，以及原有收发、附件、搜索、分页、托盘、重连、恢复和证书拒绝。记录 build/tests/integration-x64-Release-中文-d98b46f67cad4976a00467b29f9e2ac3/results.json。
- x64 Release HTTP/PAKE 原有 13 项 PASS，记录 build/tests/pake-x64-Release-中文-643b869d7899053d/results.json。
- x64 Release 新配置/DPI 回归 288 项 PASS：公开字段保存、大小写账号保持、密码不保存/关闭清除、重启恢复、两个顶层窗口为 PerMonitorV2、96→120→144→168→96 DPI 后的真实字体和控件矩形、独立 DPI 状态、短屏滚动可达。截图与几何记录见 build/tests/ui-1.0.1-x64-Release-5a5d01b58565416ba53bf04222cf7367/results.json。

Win32 Release 的相同配置/DPI 回归也通过 288 项，记录 build/tests/ui-1.0.1-Win32-Release-8750b061e3e540f986a0251ded11a13e/results.json。x64 HTTPS 与 Win32 HTTP/PAKE 的生产构建分别完成真实遮蔽输入、账号创建、原生登录与中文正文同步、30 秒空闲资源观测和正常退出，记录 build/tests/production-x64-c5ba96814f07edc6369810016fe6194f/results.json、production-Win32-c6161152b3bf6a2c1ab373a88c3bdeea/results.json。

初次交付的十二个 EXE 当时通过 1.0.1.0 / 1.0.1 核对；四段文件版本显示已在上节纠正，不作为当前期望。初次四个 x64/Win32 客户端/服务器独立 RAR 通过格式、恢复记录、真实解压和全部文件 SHA-256 检查，程序与当时生产构建一致且不含测试入口。50 个项目 C 源文件与头文件格式检查通过。原始证据及归档哈希汇总在 build/verification/ui-1.0.1.json，版本单项记录在 versions-1.0.1.json。

测试只将 DPI 消息注入独立测试窗口，没有改变桌面设置；截图证明本轮窗口绘制与几何结果，不代表已经完成真实物理多屏移动验收。生产构建排除 DPI 测试消息入口。设计图为主界面提案，当前客户端仍使用临时布局，正式分栏待确认。

## 2026-10-11 C 风格整理验证

验证范围为项目维护的 49 个 C 源文件与头文件，包含本机 C 自测；规则见根目录 AGENTS.md 和 .clang-format。405 个函数定义均有中文注释，项目原有英文注释已转换；13 个内部共享变量使用中文名，公开接口、协议、数据库字段和产品版本保持原有定义。完整语句优先单行，超过 180 列时才分行；本轮格式检查无超长行，重复格式化无差异。

整理前副本与最终文件按代码符号逐项比较，忽略注释、空白、宏续行、相邻字符串拼接及明确记录的内部变量改名后完全一致。总记录 `build/verification/c-style-20261011.json` 包含全部源码 SHA-256、改名清单、四配置构建和自测日志哈希、原始集成结果。

| 验证 | 本轮实际结果 |
| --- | --- |
| x64 Debug/Release 与 Win32 Debug/Release | 四配置均零警告、零错误；公共、客户端和服务器 C 自测均通过。日志为 build/logs 下对应配置的 .log、-client-tests.log、-server-tests.log。 |
| x64 Release HTTP/PAKE | 13 项完整 PASS，pake-x64-Release-中文-8d725e0ab190b718/results.json；包含原生加密 WS、跨账号中文正文、多块附件、重放拒绝、重启凭据恢复和设备撤销。 |
| x64 Release HTTPS/WSS 首轮 | 完成 32 项后损坏缓存重下载出现 Windows 12030、phase=3，FAIL 保留于 integration-x64-Release-中文-e402a9d9e3bc48bd9075ea1a387aa6b8/results.json；服务端相关请求记为 200，根因未确定。 |
| 原脚本全新目录复验 | 未改超时、重试或业务代码，44 项完整 PASS，integration-x64-Release-中文-a48a74ab00e74e4a8b40b8994b58b2bc/results.json；包含缓存修复、事务回滚、托盘、正文及附件续传、WSS、证书拒绝和凭据生命周期。 |

上表集成结果均位于 build/tests。首轮受限构建停在多进程调度，确认本任务进程身份后结束等待构建；常规环境原脚本四配置通过。本次为源码风格整理，没有重新验证 Windows 服务安装或真实云盘，没有重打发布包；版本保持 1.0.0，下一版本需求待所有者提供。保留首轮网络失败，不能据复验通过宣称长期稳定性。

## 最新：首次配置脚本

`tests/setup_wizard.py` 普通用户记录 `build/tests/setup-wizard-d17fa450ac445d7d/results.json` 为 8 项 PASS，覆盖 Windows PowerShell 5.1 / PowerShell 7：8443 默认端口与自定义端口、两根目录派生、预览无写入、错误绝对路径/目录重叠/无效端口保护、中文空格客户端目录、快捷方式、重复配置保持设置，以及实际 Drop 窗口读取生成的服务器地址和 `Jsnowly` 账号，密码为空。

管理员记录 `build/tests/setup-wizard-d58e7d441d32b7ab/results.json` 为 14 项 PASS：x64/Win32 新根目录加密初始化、真实账号/恢复包密码各两次确认、无密码输出、机器 DPAPI、LocalSystem 自动服务、随机端口接口、派生备份目录、Administrators/SYSTEM ACL、Domain/Private + LocalSubnet + 程序/端口防火墙范围、已有服务/目录不覆盖。非空根、占用端口和 Web 类型 OAuth 均在初始化前拒绝，原文件不变。所有随机服务及规则在 finally 移除，并再次核实不存在。

x64 HTTPS 分项用自有短期隔离证书和匹配私钥，实际合并为 certificates/server.pem、启动 HTTPS 服务，再用明确的隔离 CA 连接获得 tls / HTTP 200；脚本自身的 Windows 信任检查按预期拒绝未受信任的证书，setup-state.json 记录 Failed / 验证服务接口，没有跳过证书验证或新增系统信任。正式公开受信任证书、部署机 DNS/代理及真实 Google 浏览器流程仍由现场运行脚本验证。本轮未改变真实 Google、正式服务器或系统代理，未因脚本更改重复全部消息/附件回归。

首轮管理员验收失败记录 7bb93a5e9ba37a5c、9c7a0981b6a1ebef、8dc4ce1c7a40fbb3 保留：配置流程已完成，额外 TCP 验收错误要求状态文本为“200 OK”或一直读取至对端关闭，导致断言/连接结束错误；改用标准 HTTP 客户端按响应长度读取后两架构完整通过。没有把早期失败记录标为 PASS。

四个新版独立 RAR5 包纳入生产配置脚本，允许的 .ps1 仅 Setup.Common.ps1 和对应组件的 Setup 脚本；不包含测试源、正式密钥、OAuth JSON 或数据。最终 RAR 检测、实际解压、全部清单/生产 EXE/脚本 SHA-256、压缩参数及重复引用记录为 `build/verification/setup-rar-release-20261011.json`。缓存整理保留历史结果与真实授权，后续清理记录见 `build/verification/setup-cleanup-20261011.json`。

本轮清理报告为 PARTIAL：旧发布包的 16 个路径实际删除，新版四包保留。管理员清理确认被 Windows 报为“操作已被用户取消”；普通全量清理的在用保护先后捕获开发 build/bin/x64/Release/dropserver.exe 活动进程并在删除前中止，未关闭进程。因而 4 个机器 ACL 保护的首次配置测试根及正在使用/尚未清理的开发缓存保留，不能把本轮缓存整理描述为全部完成。此前 cleanup-20261011.json 和 cleanup-accessible-20261011.json 的成功历史证据已保留原内容。

## 本轮交付：实际 RAR 包的密码确认、收发与文件校验

从 RAR 实际解压出的服务器执行 `tests/password_confirmation.py`，x64 和 Win32 共 14 项 PASS，见 `build/tests/password-confirmation-d938e052502f1cf8/results.json`。账号创建明确捕获首次与 Confirm password 两个提示，一致才创建，不一致数据库字节不变；重置、恢复导出不一致保护原数据，正确确认后真实备份恢复和拒绝重定向仍通过。用户现场只输入一次的程序未由本机检查；此前完整服务器 ZIP 早于确认修正，最新 EXE 的确认逻辑已在实际包中重新验证。

RAR 中的独立客户端、服务器运行依赖闭包分别使用，x64 的 `account-creation-9d90fb97f0d16a8a/results.json` 和 Win32 的 `account-creation-a3205434490b0228/results.json` 各 7 项 PASS，包含真正原生 HTTPS 和 HTTP/PAKE 登录、密码清空、中文正文提交/同步，以及不合法账号、重复创建保护和大小写身份分离。测试只使用随机隔离账号/目录，正式数据与 Google 未修改。

四个 RAR5 归档的 m5、固实、恢复记录、每文件 BLAKE2sp-256、相同文件引用、RAR 完整性、实际解压、所有 SHA-256 和生产 EXE 一致性均通过，初次证据为 `rar-packages-3061ffac6eac421e/results.json`。参数为 -cfg- -ma5 -m5 -s -md32m -rr3p -htb -oi1:0；指定字典 32 MiB，小包被 RAR 自动优化为 x64 实际 16 MiB、Win32 实际 8 MiB，附属 .packaging.json 同时记录指定值和实际值。服务器包的 README 和 docs/SERVER_DEPLOYMENT.md 为相同文件，实际以引用保存；客户端包没有相同文件时不要求出现引用。新文档同步后对最终归档重验，最终报告为 `build/verification/rar-release-20261011.json`。初次尝试用本机 7-Zip 24.09 解压服务器包遇到部分固实/引用文档 Data Error，而 RAR 自身检测、实际完整解压及逐文件哈希全部通过；交付使用 WinRAR/RAR 解压。

已跟踪测试源和脚本从 Git 索引移除，本机文件保留、忽略规则实际通过。缺少 self_test.c 的项目求值为 DROP_DIAGNOSTICS=0 且编译列表不含该文件，证据 `build/verification/source-without-tests-20261011.json`。普通缓存清理已实际完成 225 个目标，逻辑大小 32.387 GiB，保留 103 个历史结果；记录 `build/verification/cleanup-accessible-20261011.json`。后续管理员清理已完成，`build/verification/cleanup-20261011.json` 为 PASS，skipped_targets 为空，删除 129244117 字节并保留 113 份历史结果，包含此前无法处理的 9 个保护目录。原始隔离测试数据库、附件、旧包与编译缓存按清单删除；正式数据及真实 Google 授权保留。

所有者随后明确报告故障电脑因代理导致连接失败，调整后已正常。该项为用户现场确认；此轮原生收发验收仍为本机隔离环境，不代替实际多机附件、长期运行和服务身份真实 Google 自动镜像验收。

## 现场连通性：正式服务器可从开发电脑直连，故障电脑待核对

2026-10-11 所有者说明失败发生在另一台电脑。开发电脑对正式域名 `dropserver.jsnowly.win` 实际解析到 `10.10.10.62`；`curl.exe --noproxy '*'` 请求 8443 的 `/api/v1/server-info`，未禁用证书校验，返回 HTTP 200 / protocol_version 1 / version 1.0.0 / transport tls。当前正式 x64 `drop.exe --probe` 通过开发电脑现有自动系统代理时返回 network_error，Windows 12029 / phase 1；通过相同本机代理的 curl 连接也在 TLS 握手阶段被重置。现有代理例外不包含该域名。本机代理路径失败、直接路径正常已复现。

证据为 `build/tests/deployment-connectivity-20261011.json`。这是开发电脑的只读现场检查，不是所有者所说的故障电脑测试，另一台电脑仍须单独检查 TCP、直接 HTTPS 和系统代理。未登录正式账号或执行消息/附件/WSS 测试，未修改系统或正式服务器。因此不将跨物理电脑完整收发、长期运行或服务身份云盘验收标为完成。

## 本轮新增：密码二次确认、Drop 独立客户端包

`tests/password_confirmation.py` 使用各自随机隔离数据，x64 的 `password-confirmation-77e9a83ac6675741/results.json` 和 Win32 的 `password-confirmation-9c3a814d697916e1/results.json` 各 7 项 PASS。验证 recovery-export 两次输入一致才创建包；不一致不创建目标且数据库字节不变；已有恢复包保持字节不变；账号创建和重置不一致时数据库字节不变、正确确认后才能提交；使用原恢复密码完成真实加密备份恢复；重定向输入仍拒绝。捕获自有控制台输出检查无随机测试密码。未操作正式服务器或真实 Google 对象。

实际 x64 独立包为 `build/packages/Drop-Client-1.0.0-x64-20261011-032758.zip`。包内仅有 drop.exe、5 个实际运行 DLL、README、许可与校验清单；不含 dropserver.exe、私钥、DPAPI、数据库、调试符号。`client-package-20261011.json` 确认全部清单长度/哈希、ZIP 哈希及生产客户端字节一致。用该包内的客户端对隔离生产服务器实际完成 HTTPS 和 HTTP/PAKE 的 `Jsnowly` 登录、密码清空和中文正文提交/同步，账号创建/不合法名称/重复账号保护也通过，7 项为 `account-creation-c1b7a6edcf6681e5/results.json`。独立包启动验证了运行依赖闭包；未据此标记跨物理电脑验收完成。

## 本轮修复：大写账号名

用户现场报告 `Jsnowly` 输入 12 字符以上密码后创建失败；根据当前源码确认旧规则拒绝大写 J，未读取用户密码或访问部署电脑。放开 A–Z 并保留原大小写后，x64/Win32 1.0.0 生产构建通过。账号创建与 HTTPS 登录使用同一规则；HTTP/PAKE 原始身份不作大小写转换，数据库保留精确匹配，既有小写账号不合并。

完整 PASS 为 `build/tests/account-creation-c33ab839b4e2507f/results.json`，共 14 项：两架构各验证不合法名称在密码输入前拒绝、短密码不建立账号并可用合法密码创建 `Jsnowly`、重复创建不泄露密码且不覆盖原凭据、`Jsnowly`/`jsnowly` 为独立账号、原密码的实际 HTTPS 登录、原生 HTTPS 客户端登录并提交/同步中文正文、原生 HTTP/PAKE 客户端登录并提交/同步中文正文。密码仅在随机隔离目录的程序自有隐藏控制台输入，检查捕获输出和日志无测试密码；正式服务器数据及真实 Google 对象未操作。不同物理电脑现场验收仍由部署者执行。

前两次脚本失败保留在 `account-creation-1e32d77e2747761d`、`account-creation-fb080303260141b3`：前者 stdout 重定向连带继承了重定向 stdin，被原生密码读取正确拒绝；改为读取程序自有控制台输出后，后者因 Python 脱离测试控制台留下无效标准输入句柄，后续 OpenSSL 子进程无法启动。补齐非交互子进程的 DEVNULL 输入后完整通过，未放宽生产程序拒绝重定向密码的行为。

## 本轮新增：双通信模式与程序旁 Data

| 项目 | 本轮结果 | 证据（build/tests 下 results.json） |
| --- | --- | --- |
| HTTP/PAKE x64 | Debug/Release PASS | pake-x64-Debug-中文-efe2e83120436833；pake-x64-Release-中文-95ccd23a5f40c496。Release 13 项，包含原生伪造服务器证明、公值拒绝、独立 SRP 数学互操作、方向密钥、标签/路由/重放、乱序、中文多块附件、加密通知、重连和撤销。 |
| HTTP/PAKE Win32 | Release 13 项 PASS | pake-Win32-Release-中文-14240e0922c09ccf；独立计算由 64 位 Python 执行，被验收客户端/服务器为实际 Win32。 |
| 保留 HTTPS/WSS | x64 Debug/Release 完整 PASS | integration-x64-Debug-中文-854f1706cd7947fda09037581dced8f7；integration-x64-Release-中文-28987b3f793744368e048935633c6272。证书校验、100 次短连接、原生 WSS、附件、托盘、断线、恢复与错误证书拒绝；HTTPS 不调用 PAKE 传输包装，无自动降级。 |
| 显式证书配置错误 | 四配置及两架构实际包 12 项 PASS | certificate-mode-60501dcd47de5de1。最终修正 --certificate 空参数后重新构建四配置和生产包；空证书或不存在的证书均失败，不启动 HTTP。其他完整功能结果保留作为此前基础回归，本项针对最后的参数校验改动。 |
| 四配置和迁移 | 原生构建/自测 PASS | build/logs/<平台>-<配置>*.log；加密 schema 1/2/5 旧数据保留、本机 reset 补建 PAKE、密钥/备份/事务/容量失败回滚。服务器从 9 到 10 保存加密快照，不能直接转换旧密码哈希。 |
| 真实 Windows 服务 | HTTP/PAKE 与 HTTPS 各 5 项 PASS | service-pake-Release-5cbe05c34e14449ead02aea15ba77287；service-tls-Release-34bd00c990464b66846c93d52797ae1c。管理员目录树 ACL/所有者、LocalSystem 机器 DPAPI/SQLCipher、登录、SCM 停启重启及卸载；随机测试服务最终卸载。 |
| 实际 x64 发布包 | 两种模式生产界面 PASS | production-x64-da1df55415d2aaa70d10d06b10caeb28（PAKE）；production-x64-00770a447cf9837f2e3e8cda3cc0d2f3（TLS）。无测试入口/密码，本机真实遮蔽账号创建、密码清空、中文持久收发及 30 秒资源采样。 |
| 实际 Win32 发布包 | 两种模式生产界面 PASS | production-Win32-eaf2dadd59d150daa124f8efb75b5348（PAKE）；production-Win32-9e11bef2fa22fb9d1a3f9d015e263dbd（TLS）。 |
| 程序旁默认 Data | 两架构实际包各 8 项 PASS | server-directory-x64-Release-0e67662ab07229e0755d3696c95bd5f5；server-directory-Win32-Release-61d5dcb1eb2f357b5eb51e4f8355976e。EXE 移动、中文空格、查询不创建、手动目录/身份保留、无 AppData 回退、不可用目录明确失败、服务固定绝对路径。 |
| Google 后端 schema 10 | Release 模拟完整 PASS | drive-Release-388e2cdbc1d44f06aacda2002add68ba；仍使用 HTTPS，续传/刷新失败、目录分页、密文下载与独立恢复、原生托盘附件接收、后台失败隔离。未重用旧目录授权到新 Data。 |

实现与上线边界：HTTP 账号名、路径和流量特征仍可见；两种模式都允许中央服务器处理明文，存储仍加密。PAKE 使用固定 csrp 上游并加包装，已经本机互操作验收，未经过独立安全审计。跨物理电脑、24 小时运行、服务身份真实 Google 自动镜像仍是现场待验收项。旧 C:\Users\JLYis\AppData\Local\DropServer 及其授权未移动或修改。

最终机器可读汇总为 build/tests/dual-transport-20261011.json，包含各原始结果文件校验值、当前关键源码 SHA-256、实际 ZIP 校验与包内程序等于最终生产输出的检查。两包已同步最新状态、部署及 PAKE 说明；原始成功/失败记录和旧包均保留。

修复记录：最初自测旧 schema fixture 未移除新 account_pake 表，已修正 fixture 后四配置通过；服务首次普通权限运行无法转换机器密钥，随后按此前授权提升；第一版服务验收适配器缺少 raw 方法，脚本修复后两模式完整通过，失败目录保留。没有将这些失败记录作为 PASS。

## 既有功能与更新前的历史基线

| 项目 | 当前结果 | 证据与边界 |
| --- | --- | --- |
| 原工程和四配置 | 通过 | 保留解决方案/主项目 GUID；C17、UTF-8、/W4 /WX，x64 与 Win32 Debug/Release 原生构建/自测。 |
| x64 完整集成 | Debug/Release PASS | Debug 5467ddc97f944760a12e2b2b09cde2fb；默认目录修改后 Release c6d52c3d1c2f444c9a0ee2b9e6dc7ce2，44 项完整 PASS。旧 Release 0a6748e6f74e46cfb91f4a86a3efdd8b / 51f9cd3cfe5548e0a1285337deedfea2 保留，完整 results.json 在 build/tests/integration-<架构>-<配置>-中文-<ID>。 |
| Win32 完整集成 | Release PASS | d4cdb17e23ef42769a4547cff723a069，包括原生托盘、中文多附件、续传、确认、心跳退避、压缩、配额、TLS 拒绝。Debug 当前完整原生自测通过。 |
| 加密和事务 | PASS | SQLCipher 错密钥/实际页篡改、非空迁移、附件块/清单篡改截断、SQLite_FULL 整页同步回滚、解除限制后补齐。未填满 Windows 系统卷。 |
| 本地备份恢复 | Debug/Release PASS | backup-Debug-中文-8893042637be470db82f1e88c9bea9ec；backup-Release-中文-13bedf8514f946d0bd4631351ac08f34。独立包/新目录恢复、较新本机副本保留、不盲重发及明确重试。 |
| 大于 4 GiB | 完整 PASS | 4294967313 字节，4097 块；backup-Release-中文-324df4d89e5e4bb7a910b5533e03fbae/results.json。修正后实际 Debug 重用原数据，收发、备份进程中断后同任务续作、完整恢复、较新客户端历史和文件保留、不盲重发、明确重试及备份密文篡改拒绝均通过。目录保留首次 Release 名称，首次失败另存 initial-failure-results.json。 |
| 后台完整校验 | 实际故障 PASS | finalize-Release-f8fad50026824d70847d10769ce24751/results.json：64 MiB 真正后台校验期间正文提交成功、未完成附件不可引用；实际终止进程保留 64 块，重启再校验；篡改失败持久化，原块明确修复后才能完成。诊断延时钩子不进入生产。 |
| Windows 服务 | 实际 PASS | 本轮 service-Release-1d7fbc58ec3e4584804890d80303636b：管理员目录树 ACL/所有者、LocalSystem DPAPI/SQLCipher/HTTPS 登录、SCM 停启重启及卸载 5 项通过，最终无本次 DropTest 服务；原 c7d79aa9427046d69fe00828128beb2b 保留。 |
| 分支设置 | GitHub 实际 PASS | branch-primary-20261010.json：主要为默认且唯一分支；master 历史已包含在主要内，删除使用旧 SHA 租约保护，本地 origin/HEAD 同步。 |
| 指定程序图标与 1.0.0 版本 | 四配置及两架构包内程序 PASS | version-icons-1.0.0-da793e5397cf4843a91d63eb9ac2f172/results.json：实际读取当前 12 个 EXE 的 Windows 文件/产品版本、原始文件名、Debug 标记和 --version 输出；各九种图标尺寸与由最新 PNG 编码的 ICO 图像字节一致。旧 dda24a4ed48e49a7a2392653bd715000 / icons-39d95a8b20954422bd4db1e8b83d4f12 证据保留。 |
| 1.0.0 长期试用包 | x64/Win32 当前生产运行 PASS | production-x64-00b7921c700dc299890bb7f8e53f44c7/results.json、production-Win32-482740fc9fb42c8b603752960cb5f11f/results.json：各 4 项，测试入口排除、真正遮蔽控制台、包内原生 UI 中文收发、30 秒空闲采样；实际窗口截图已查看，历史刷新后的标题保留 Drop 1.0.0。先前 500 条消息与负载资源证据保留在 production-x64-808ae4f0b00417ffb521fb92baf71fc8、production-Win32-84535d990ef3c1810f5861a1b4f141cf。真实 ZIP 校验记录为 build/tests/packages-*.json。 |
| 通知与闪烁 | x64 Debug/Release、Win32 Release 各 9 项 PASS | notifications-x64-Debug-789a6a22e3ce4066b58e8bdf20c02d70、notifications-x64-Release-66d477cec79c4fb0a9aeafacf410bf31、notifications-Win32-Release-7061151e029f47308a9b780c053e5ece 的 results.json：真实消息、窗口显示只请求任务栏、实际最小化到托盘后请求通知/交替图标、托盘点击停止、独立开关、关闭仍同步、重启保留不重放、原生已读后停止。Windows 弹窗是否展示与任务栏颜色尚未人工视觉确认。 |
| Google Drive | 真实 x64 生产候选 PASS；Debug/Release 模拟故障 PASS | google-real-3daea6187076497fada56f20e3d6e93a/results.json：实际浏览器 OAuth/DPAPI、刷新、7 文件密文上传/下载及逐文件 SHA-256、独立恢复包新目录恢复、原中文正文与完整附件托盘接收通过。模拟记录 drive-Release-2c0f9bfd51164393b52d6777e5610a8c/results.json、drive-Debug-d815f8f1315849dc9e6539b976b98dc6/results.json 覆盖授权/配额/429、实际进程终止、会话过期/丢最终响应、分页、路径穿越、篡改拒绝及后台失败隔离；这些故障未在真实 Google 服务触发。 |

## 发布候选与资源

当前归档：`build/packages/Drop-1.0.0-x64-20261011-004210.zip`、`build/packages/Drop-1.0.0-Win32-20261011-004210.zip`。各含文件清单 files.json，有外部 .sha256；同步最终文档时重算两层校验值。保留此前候选和失败记录。生产程序新增默认服务器目录和手动覆盖，包含最新图标、统一版本、此前通知策略和独立开关、schema 9 云盘后端、服务权限修正和大文件后台校验，诊断计数/密码、测试入口/环境钩子在编译时排除。本轮四配置构建/自测、12 个 EXE 资源、两个实际新包目录专项、x64 完整集成、隔离服务生命周期以及真实默认目录 Google 基线校验通过，Drive Release 模拟 13 项 PASS 为 drive-Release-9b1be17a317a464a815ba857b684f808/results.json。新包客户端和八个 DLL 与上轮生产 UI 验收字节相同；通知专项、30 秒空闲及 500 条消息负载保留原范围，没有重新测量长期稳定性。

当前 1.0.0 生产窗口截图：`build/tests/production-x64-00b7921c700dc299890bb7f8e53f44c7/production-ui.png`（x64）、`build/tests/production-Win32-482740fc9fb42c8b603752960cb5f11f/production-ui.png`（Win32），已实际查看，最新客户端图标、1.0.0 标题和两个通知选项可见且布局正常。通知专项截图在各最终 notifications-* 目录的 notification-settings.png。首轮脚本复用过期 HTTPS 连接及第二轮同步鼠标按下造成等待的失败记录保留在 notifications-x64-Debug-cd7039b16f844aae9c6034bcc03e1a9b 和 notifications-x64-Debug-0c8054c8fb114cae9cbdb3ecc39ba0f6；修正脚本后全部通过。

以下保留前一生产候选登录后各 30.047 秒的空闲/500 条消息采样。CPU 百分比相对一个逻辑核；内存是该时间窗内采样最大工作集，MiB=1048576 字节。本轮新包另测各 30 秒空闲，结果在新的 production-* 记录中，不将两轮测量混为同一候选的负载结果。

| 架构/场景 | 服务 CPU | 客户端 CPU | 服务 MiB | 客户端 MiB |
| --- | ---: | ---: | ---: | ---: |
| x64 空闲 | 0.052% | 0.260% | 11.10 | 25.17 |
| x64 连续消息 | 4.264% | 31.721% | 13.66 | 30.92 |
| Win32 空闲 | 0.052% | 0.208% | 10.93 | 22.54 |
| Win32 连续消息 | 4.784% | 31.877% | 13.28 | 27.52 |

各提交 500 条真实 HTTPS 消息，x64 20.766 秒、Win32 20.844 秒，提交错误均为空；真正托盘客户端随后保存全部消息。测试运行于共享开发机，大文件演练可能同时运行，不是隔离硬件基准或 24 小时稳定性证明。当前生产窗口实际截图及视觉核对：`build/tests/visual-production-d8f00649966b4bdcb350dc57bde51340/production-window.png`（x64）、`build/tests/visual-production-43db862acef54674ae2df161176f4479/production-window.png`（Win32）。截图脚本在捕获前同步重绘并拒绝空黑客户区，修正的是初次隐藏窗口捕获时序。

4 GiB 演练重用首次失败已上传的分块，Debug 续作的 client-send + client-sync 耗时 926.828 秒；不包含首次全部上传/稳定副本准备，也不包含后面的备份和恢复。过程包含原生完整文件验证及下载，不能将这个时间直接当成首次网络上传吞吐。每次客户端启动会重新 SHA-256 校验已完成文件，大规模历史的启动成本仍是优化范围。全部 64 位长度、分块认证、整文件摘要与恢复验证实际通过。

真实 Google Drive 10 项完整 PASS：`build/tests/google-real-3daea6187076497fada56f20e3d6e93a/results.json`。原附件 3145745 字节，备份 7 个密文文件共 3351381 字节；上传 33.359 秒、下载 22.907 秒，全部密文路径/长度/SHA-256 一致，并完成真实新目录恢复及全新托盘接收。云端保留备份 87008bdff9d0dcc23cf05a850c360b69，未删除或修改原有备份。系统浏览器授权成功证据在前一隔离目录 google-real-4c49237fcc3743d5bf3597b420454f84；添加测试用户后通过，首轮验收脚本按钮编号错误及原始失败均保留，脚本修正后新目录完整通过。截图在最终目录的 original-native-client.png / restored-native-client.png，已实际查看。

尚未人工验证：不同物理 Windows 主机、普通另一用户读取权限演练、真实睡眠/锁屏/Windows 重登录、网络适配器实际切换、Windows 通知实际展示、多显示器 DPI、24 小时以上稳定性、真实 WAN 断网及 Google 授权到期/限流/配额耗尽。真实云盘演练在 x64 生产程序运行，未在 Win32 独立重复；Windows 服务身份下的真实 Google OAuth/后台镜像尚未演练，服务及后台云盘失败隔离已有各自实际/模拟证据。实际新目录恢复不依赖原机 DPAPI，但不能代替另一主机演练。当前密钥版本 1，无换钥历史，未实现轮换。公开 MIT 作者/年份占位符待所有者确认。

## 历史阶段记录

# 验收记录

日期：2026-10-10。环境：实际 Windows x64 开发机，系统 API 10.0.26300，注册表 Build 26300.9457 / DisplayVersion 26H2（ProductName 残留为 Windows 10 Pro for Workstations，不据商品名推断兼容性）；Visual Studio Community 2026 18.10.3，MSVC 14.51.36231，v145。未据本机结果承诺其它 Windows 版本。

| 项目 | 当前结果 | 证据或限制 |
| --- | --- | --- |
| 保留工程 | 已核实 | drop.slnx、原项目 GUID 及四种配置保留；增加 dropcommon、dropserver。 |
| 固定依赖 | 通过 | vcpkg baseline 和源码哈希固定，Tcl / CivetWeb overlay 重建通过；依赖真实构建日志位于 third_party/vcpkg/buildtrees。 |
| x64 Debug/Release | 通过 | C17、零警告、零错误；build/logs/x64-Debug.log、x64-Release.log；公共及服务器原生测试均通过。 |
| Win32 构建及原生运行 | 待执行 | 未承诺兼容。 |
| 公共格式和秘密保护 | Debug/Release 通过 | u64 最大值、溢出和超过 4 GiB 的长度；中文/emoji UTF-8；URL；重复 JSON 键/NUL；zstd 损坏；用户 DPAPI 损坏；AEAD 篡改。未实际传输大文件。 |
| 加密数据库 | Debug/Release 通过 | 首次建表前设密钥；错误密钥及实际密文页篡改拒绝；cipher_integrity_check、integrity_check、重开。预期的 HMAC 错误属于拒绝证据。 |
| 账号与设备 | Debug/Release 通过 | Argon2id、错误密码、禁用账号、到期凭据、登录、token 摘要、退出撤销。 |
| HTTPS 及权限集成 | Debug/Release 通过 | 进程内隔离 CA 正常连接；原生 WinHTTP 拒绝不可信证书；错误主机名和过期证书拒绝；静态文件/未授权请求拒绝；JSON 错误、账号隔离、重启身份/凭据、轮换及退出。 |
| HTTPS 关闭回归 | 通过 | 100 次客户端 Connection: close 的独立连接全部成功，无重试；修复前 14–23/100 丢失。 |
| 多设备文本与 WSS | Debug/Release 通过 | 两个原生客户端同账号、另一账号、稳定请求 ID、响应丢失后重试、WSS 即时通知、离线队列及进程/服务器重启补齐。 |
| schema-1 加密迁移 | Debug/Release 通过 | 非空账号保留；升级前独立加密快照用原密钥实际解锁及完整性检查；回退快照仍包含账号。 |
| 私有 CA | 原生 HTTPS/WSS 通过 | 应用内独占信任引擎，错误 CA 在 HTTP 内容发送前被拒绝，服务端未创建该设备；过期证书拒绝。未改变系统信任。 |
| 文件同步 | Debug/Release 通过 | 稳定副本、中文/同名/空文件、所有相关设备完整校验、上传与下载实际进程中断续传、部分上传不可引用、越权拒绝、磁盘密文/清单篡改及截断拒绝。 |
| 原生托盘与后台同步 | Debug/Release 通过 | 真实窗口操作：私有 CA/登录、密码清除、中文/emoji 正文及文件、多附件、收件人、查询与导出、隐藏继续 WSS、重复启动、真正退出/重启补齐。 |
| 自动启动 | 开关与命令通过 | 原生客户端写入/移除隔离实例自己的用户 Run 值；默认关闭，带 --background。未实际注销或重启 Windows。 |
| 心跳退避、账号已读和保存确认 | 未实现 | 阶段五。 |
| 服务、恢复包、备份、Drive | 未实现/未验证 | 后续阶段及真实授权。 |

构建日志不代表运行验收。测试数据不进入源码或发布包，口令及令牌不打印。错误密钥/普通 SQLite 无法读取、SQLCipher 认证检查以及实际文件检查组合使用；不单凭后缀或搜索不到正文断定加密。

## 阶段一证据与限制

完整 Debug PASS 记录：`build/tests/integration-x64-Debug-中文-6b3dd9198742484ba64522a2b1190ffc/results.json`。Release 最终记录由脚本保存于相同目录规则；每次运行使用新目录。

复现脚本：`scripts/Verify-Integration.ps1 -Platform x64 -Configuration Debug`。修复前 `integration-x64-Debug-中文-b77cc3594aad4e0f91a0ea2e7a1b969b/results.json` 记录 23/100 响应丢失。失败记录保留，不用新结果覆盖；进程仍在运行，诊断只记录状态和字节数，没有记录内容或凭据。

阶段一证书正向测试使用 Python TLS 的进程内 CA 文件。阶段二进一步验证原生 WinHTTP 的应用内 CA 配置，未信任任意证书；默认无 CA 配置时仍使用 Windows 校验。公开 CA 外网连接、不同 Windows 用户 ACL 演练和本机遮蔽输入的人工操作尚未验收。

阶段二完整 Debug PASS：`build/tests/integration-x64-Debug-中文-c18564c74fcd42d68b7778caf56c2eca/results.json`。完整 Release PASS（含新增错误 CA/过期原生路径）：`build/tests/integration-x64-Release-中文-e87994cedb194c6790786dc00a52c2cc/results.json`。Release 和 Debug 构建零警告、零错误，测试脚本不打印内容或凭据。

所有测试进程结束后退出，仅保留项目内隔离记录；未安装服务、修改防火墙或重启电脑。尚无可移植恢复包，不把用户 DPAPI 当成灾难恢复。

## 阶段三证据与限制

完整 Debug PASS：`build/tests/integration-x64-Debug-中文-3be0bd6257c44ebe9ec7afd44588f6d5/results.json`。原生客户端使用 9 MiB + 77 字节和空文件，同名中文附件；复制后修改/删除源文件，各相关设备仍保存逐字一致的完整副本。终止下载客户端时正文和游标已入库，重新启动只传剩余块字节。上传终止服务器后位图保持，重复块不重写密文；不完整对象不能提交消息。认证失败测试直接修改实际块/清单文件并截短密文，不仅测试内存函数。

完整 Release PASS：`build/tests/integration-x64-Release-中文-1faf70616d6648269cc34d25abd5b445/results.json`。最终 Debug/Release 构建零警告、零错误；自测额外实际生成非空 schema-2 旧文本指纹，升级后同请求重试仍幂等、改正文冲突，保留加密回退副本。

超过 4 GiB 仅验证 64 位格式和计算，尚未进行实际大文件传输；Windows 磁盘已满及完整容量管理、孤立对象宽限清理、设备保存确认和连续后台恢复仍待后续阶段。文件路径采用随机 ID，原名不用于接收路径；显式导出界面待阶段四。

## 阶段四证据与限制

完整 Debug PASS：`build/tests/integration-x64-Debug-中文-aa69458ae6414197afd6e8f3babc6892/results.json`。窗口和对话框通过原生控件消息驱动，仅访问隔离实例，不使用全局按键或用户剪贴板；多文件选择和导出使用真实文件框，导出字节与稳定副本一致。实际 PrintWindow 截图经视觉检查，修正底部重叠及日期输入宽度；截图位于各测试目录的 tray-ui.png。

完整 Release PASS：`build/tests/integration-x64-Release-中文-45d6d49cf7bf4c84afd13f8e9dad436d/results.json`。Debug 增补 PASS：`build/tests/integration-x64-Debug-中文-ab12b22210c847e198123e5619b1e5e7/results.json`；额外实际写入 55 条消息，第一/第二历史页各显示正确条数，未来日期范围返回空结果。最终构建均零警告、零错误。

自启动设置写入移至后台，现有 Run 键通过最小写权限打开，缺失时才尝试创建。实际启用/关闭及命令核对通过，测试值最终删除；未更改用户其它启动项。关闭窗口继续接收 WSS，重复启动激活原实例，真正退出后不再保存新记录，再启动增量补齐均实际通过。

Shell 提醒注册已实现，但本轮不将 API 成功等同于 Windows 实际显示提醒；勿扰、通知关闭、实际 Windows 重新登录和跨显示器 DPI 尚未人工验收。账号级已读、可靠心跳、退避与完整容量管理留在阶段五。原生文件对话框测试必须等初始化结束后写控件，错误跨进程 HDROP 测试已改为真实选择框，失败结果保留。

## 阶段五进行中的实际验证

完整 Debug 记录：build/tests/integration-x64-Debug-中文-35efa21b4e0b4fb2a515087e9f50c4ca/results.json。正文确认响应丢失模拟、重复确认不增加状态、账号已读跨设备复制、越权确认拒绝、附件完整校验后确认、异步 HTTPS/WSS 和阶段四全部原生 UI 行为通过。早期两轮续传网络错误结果仍保留；不把后续单轮通过当成长时间网络稳定性证明。

新增原生自测实际通过 SQLite max_page_count 触发 SQLITE_FULL：客户端早先待发送记录仍在，服务器消息/事件事务回滚，明确返回 space_unavailable。此为数据库容量耗尽测试，未填满真实 Windows 卷。SQLCipher 大页分配测试曾报告 VirtualLock 错误 1453；没有据此宣称所有运行内存都已锁页，存储加密与认证检查仍实际通过。

WinHTTP 请求改用异步句柄，后台等待结果和退出事件，等待 HANDLE_CLOSING 后释放回调状态和读缓冲；WSS 发送/接收各自完成事件及引用计数保护并发取消。依据微软 [WinHttpCloseHandle](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpclosehandle)、[WinHttpWebSocketReceive](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpwebsocketreceive) 和 [WinHttpWebSocketSend](https://learn.microsoft.com/en-us/windows/win32/api/winhttp/nf-winhttp-winhttpwebsocketsend)。最新版压缩/配额/心跳故障回归仍在执行，实际休眠、联网变化及长期资源验收未完成。


## 1.0.0 长期部署现场记录

公开首页/隐私说明：Sites 原生部署 succeeded / public，build/tests/website-20261010.json；源码、内部链接、归档检查通过。2026-10-11 00:02（中国时间）drop.jsnowly.win 域名、提供商和 HTTPS 均 active，首页及 privacy.html 经本机现有网络代理返回 HTTP 200，TLS 校验开启，公共 DNS 两条 TXT 值正确；响应摘要为 build/tests/website-dns-20261011.json。本机浏览器布局未人工视觉确认，直接网络路径遇到 Cloudflare 403。Google 项目发布状态、正式目录 OAuth 不据此标记通过。

2026-10-11 新增服务条款：build/tests/website-terms-20261011.json。Sites 第 2 版 succeeded，八文件归档的静态资源字节与已推送提交一致，托管配置仅由官方打包器移动并格式化；三页内部链接、锚点和资源检查通过。正式域名首页/隐私说明/terms.html 均返回 HTTPS 200，三页含隐私/条款入口。程序未因此改变或重复执行已通过的运行测试。

2026-10-11 所有者反馈“显示正式版了”，Google 发布状态记录为 In production，来源为所有者确认；记录 build/tests/google-production-publishing-20261011.json。本机未独立读取 Google 控制台，也未据此标记品牌审核通过。当时本机只读检查未发现运行中的 Drop 程序或服务，E:\DropData\server、C:\DropData\server、E:\work\Drop\runtime\server 均不存在；这仅说明本次检查的候选目录，未扫描其他盘或认定所有机器均无部署。随后所有者选择应用默认目录并完成浏览器授权，当前结果见下表与补充记录；实际部署恢复与持续运行仍待完成。

| 项目 | 当前状态 | 验收记录要求 |
| --- | --- | --- |
| 自定义域名与 HTTPS | 已通过 | 域名/提供商/证书 active；首页与 privacy.html 实际 HTTPS 200，见 website-dns-20261011.json。 |
| Google 品牌资料及域名 | 所有者已填写资料，审核未独立确认 | 同域首页/隐私说明/条款已发布；必要域名验证及品牌审核按后续控制台要求处理。 |
| Google 正式发布状态 | 所有者确认 In production | 2026-10-11 用户反馈“显示正式版了”，见 google-production-publishing-20261011.json。 |
| 旧默认目录与手动覆盖（更新前） | 历史实际包 PASS | Windows 用户 LocalAppData\DropServer、与当前目录和伪造环境无关、中文空格自定义目录、原加密身份保留、参数拒绝和服务固定目录，各 6 项；见 server-directory-* 记录和下文。 |
| 旧 AppData 目录重新授权 | 真实 Google PASS | 所有者选择默认目录并完成新浏览器授权；用户 DPAPI，无测试凭据复制，新进程刷新及 drive-list；google-production-default-20261011.json。 |
| 旧 AppData 目录基线备份 | 真实 Google PASS，实际恢复待部署 | 空存储 2 个密文文件共 204976 字节，上传/下载逐文件长度及 SHA-256 一致，保留既有云端备份；自有账号/TLS/独立恢复包、该目录新目录恢复和自动镜像尚待部署。 |
| 第二台真实电脑与通知显示 | 待现场验收 | 消息、多附件、未读、窗口/托盘提醒及真正退出后补齐。 |
| 连续 24 小时及超过七天 | 待长期部署 | 起止时间、断线补齐、刷新令牌、定时本地/云盘备份完成任务。 |
| Windows 服务下 Google 镜像 | 采用服务时待验收 | 机器 DPAPI、真实服务启动和自动云盘任务完成。 |

版本/图标核验的首次脚本 planes 假设失败保留在 version-icons-1.0.0-a89cf144d209459e87959b1d8aaf547d；Windows RC 规范化 planes=1，实际图像字节没有变化。最终 12 程序检查全部通过。首次网站上传因全局代理连接重置失败，改为该网站仓库本地连接设置；首次打包误调用 WSL、其次 GNU tar 将 Windows 盘符当远端，改用仅当前进程的 Git Bash 路径与 TAR_OPTIONS=--force-local 后，完整官方 Sites 源码/归档/发布流程通过，不修改插件或全局代理。

2026-10-11 默认目录补充：所有者选择程序默认目录、部署时自行修改；本机 C:\Users\JLYis\AppData\Local\DropServer 已初始化并新授权，google-production-default-20261011.json 的 5 项通过。云端保留本次基线 ea42af015c35ecc44e524ee8d8974dbb 和原隔离备份，不删除任何真实远端对象。本次未建立测试账号或消息，也未为实际目录创建证书/独立恢复包、转换机器密钥、安装长期服务或启动长期守护进程。

默认目录专项各 6 项：build/tests/server-directory-x64-Release-6404404f6a5fcef488df6e9f1449136c/results.json，实际新 x64 包 server-directory-x64-Release-23f2899094e5daeee31d049b208cc17d/results.json，实际新 Win32 包 server-directory-Win32-Release-4721a99ca0d4a79a62a90e6c6b47a5fa/results.json。四配置重新构建/原生自测通过；新 x64 Release 集成 44 项为 integration-x64-Release-中文-c6d52c3d1c2f444c9a0ee2b9e6dc7ce2/results.json；本轮隔离真实服务 5 项为 service-Release-1d7fbc58ec3e4584804890d80303636b/results.json，最终已卸载。当前 12 个程序图标/版本资源为 version-icons-1.0.0-da793e5397cf4843a91d63eb9ac2f172/results.json。两个新包的客户端和八个 DLL 与之前 1.0.0 UI 验收逐文件 SHA-256 相同，client-runtime-default-20261011.json；服务路径相关行为已另外验收。

本轮 Drive 模拟 13 项完整 PASS：drive-Release-9b1be17a317a464a815ba857b684f808/results.json。前一轮 drive-Release-07d1289b9ef54f0ea7800bf38f7735b7/results.json 在恢复后附件读取发生 10 秒超时，原生服务日志未报告写入错误；未查明原因。相同脚本在集成结束后单独重跑通过，未改超时或加入重试。保留失败并在长期部署观察，不能据此宣称长期网络稳定性已通过。
