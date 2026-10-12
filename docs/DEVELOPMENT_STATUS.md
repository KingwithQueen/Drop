# 开发状态

## 2026-10-12 当前交付：1.0.3 三平台界面优化

程序、Windows 文件属性与程序集身份、本地网站版本标识及新包统一采用 1.0.3。保留原有协议 1、服务器 schema 12、客户端 schema 5 和已有数据，无新的格式迁移。历史设计、旧包与验收记录保留原版本标记；没有修改已部署服务或发布网站。

三平台按所有者确认的 1.0.3 设计图，将重复正文区替换为固定高度的待发送附件槽，右侧保留所选消息附件。两个区域独立滚动，0 到 32 个附件不改变输入框或消息区尺寸；长正文通过双击或右键命令打开独立原生窗口。Windows 无效滚轮及滚动边界不再触发布局或背景擦除，背景绘制裁剪子控件；会话条和发送对象条原生文字垂直居中。Linux / AppKit 没有 Windows 的强制整窗滚轮重绘路径，真实会话已检查固定附件区域及正文窗口。

Windows 两架构 Release 各 30 项滚轮和居中检查、364 项布局 / DPI 检查及固定附件专项通过；Linux / Mac Debug、Release 原生构建和真实桌面检查通过。互通复核暴露大响应提前结束，服务器响应现按有界片段推进实际写入，临时加密帧保留到发送结束；随后重新验收。具体互通、包与最终检查记录见 `build/verification/ui-optimization-1.0.3.json`。

## 2026-10-12 Linux / macOS 原生客户端

新增 CMake 构建、POSIX 存储与锁、libcurl 双传输、Secret Service / Keychain 凭据保护、共享后台调度，以及 GTK4/Wayland、AppKit 两套原生界面。所有者已确认按 Windows 当前密度优化的设计图，连续文本消息、各自附件、待发送预览、明确导出和账号级删除已接入。可见版本仍为 1.0.2；协议及服务器 schema 不变。本机缓存新增已认证服务器删除能力，断网重启仍可记录离线删除意图，服务器再次核查权限。

两平台已原生编译并执行核心检查；真实图形会话验证界面消息密度、附件归属和保存失败保留草稿。隔离 Windows 服务器的 HTTP/PAKE、HTTPS/TLS、WS/WSS、多块附件、显式导出、接收方拒绝和双方多设备删除同步通过。实现与边界见 [跨平台说明](CROSS_PLATFORM.md)。服务器迁移、内置自启动、Apple Silicon、其他发行版、Apple 签名公证及长时间现场观察仍列为后续事项。

## 2026-10-12 当前交付：1.0.2 按账号删除消息并同步全部设备

客户端工具栏「删除消息」、消息右键菜单和消息区 Delete 键提供确认后删除。权限按发送账号，电脑 B 可以删除同账号电脑 A 发出的消息，不能删除其他账号消息。服务器在事务边界再次校验令牌、清除活动正文和附件引用、保存双方账号独立删除事件；自发和重复删除不产生重复事件。在线客户端同步移除，离线设备重连补齐；离线删除先隐藏本机记录并持久排队，真正退出及服务器重启后继续提交。操作不需停止服务。

客户端确认删除后清除正文、下载记录及受管理缓存；文件占用时持久排队重试，同时回收该附件的损坏隔离副本。原始文件、用户已经导出的文件及旧备份保留，服务器未引用密文遵循原有宽限期与备份引用规则。账户删除仍保留历史。协议保持 1，服务器升级到 schema 12、客户端升级到 schema 5，均先保存回退快照。需要更新服务器及全部客户端到本次 1.0.2 构建；旧客户端不能同步移除已有缓存，新客户端连接旧服务器仍能收发但禁用删除。

实际原生多客户端删除、接收方越权拒绝、离线退出重试、重连补齐、幂等和旧请求重放、分页、异常页事务回滚与缓存路径保护已验证。使用实际前次发行程序建立非空旧数据，升级后身份、消息和保存的登录状态保留，旧消息可以在线删除。两架构原有 C 自测、Release DPI 各 333 项、正式登录与恢复各 6 项、阅读与刷新各 8 项及 x64 Debug 完整网络回归 47 项通过；产品可见版本保持三段 1.0.2。当前证据及四个独立发布包集中于 build/verification/message-deletion-1.0.2.json，详见 [验收记录](VERIFICATION.md)。没有操作已安装客户端、实际账号、运行中的部署服务或 Google 云数据。

## 2026-10-12 历史交付：1.0.2 发送对象、附件预览与配色

左侧移除「全部消息」，只保留自己、已有消息参与者及明确验证的账户。启动和重新登录默认选中自己，输入区明确显示「发送到：我的设备（自己）」并使用「发给自己」按钮；选择对方时同时显示其名字与登录名。发送任务直接使用当前账户 ID，切换视图期间禁止发送，避免查看对方消息却默认发给自己。

正文上方新增紧凑的待发送附件列表，立即显示原始文件名、大小及数量，文件类型图标在独立后台线程取得。文件选择、Ctrl+V 复制文件和拖入共用路径校验、去重及最多 32 个附件的限制；普通文字粘贴继续正常使用。可逐个移除或全部清空，过期图标结果不会覆盖新的草稿；附件列表与图标随 DPI 重新建立。窗口采用蓝灰背景、白色内容区与深蓝发送按钮，保留原生控件、菜单、分栏、短屏滚动和系统高对比度适配。

两架构附件专项、DPI 各 328 项、正式登录与真正重启恢复各 6 项、阅读与刷新专项各 8 项及 x64 Debug HTTPS/WSS 47 项通过。产品版本继续使用三段 1.0.2。服务器六个构建的二进制与前次 1.0.2 完全相同，协议及数据库 schema 不变；已部署服务器无需为此次客户端界面更新替换或重启。升级客户端继续使用原数据目录。当前验收与发布包证据汇总于 build/verification/client-compose-1.0.2.json，详见 [验收记录](VERIFICATION.md)。

## 2026-10-11 历史交付：1.0.2 版本更新与存储说明

所有者指定产品版本为 1.0.2。客户端、服务器、可见文件版本与产品版本、版本查询、manifest 内部身份、vcpkg 项目版本、当前部署文档及本地网站版本标识同步更新；网站没有重新发布。保留原 1.0.1 阅读与刷新优化，协议、数据库 schema、附件命名与同步行为不变。

已核对缓存路径：客户端数据根目录\服务器ID\账号ID\files\32位随机附件ID.file，下载中使用 .part。原始名称存在数据库元信息中，搜索原文件名不会匹配该缓存；导出时使用原名作为默认名称。Setup-Drop.ps1 使用数据根目录下的 client 子目录作为实际 --ui-root。服务器目前没有消息删除接口或命令，账户删除保留历史，cleanup 只处理过期且未引用的上传/残留文件；本轮补充说明，没有实现消息删除。

x64/Win32 的 Debug、Release 和正式构建完成，十二个 EXE 的 --version、文件版本和产品版本逐项核对为三段 1.0.2，53 个 C/H 文件格式检查通过；四个独立发布包实际解压并核对全部文件 SHA-256、程序、部署文档与配置脚本。证据汇总为 build/verification/version-1.0.2.json。与上一交付比较，C/H 源码仅 version.h 的版本常量变化，因此保留此前业务回归记录，不重复网络、DPI 或服务/云操作验收。

## 2026-10-11 历史交付：1.0.1 客户端阅读与刷新优化

用户列表改为比较并更新变化的行、名称和未读计数，稳定消息选择不再清空重建列表。选中消息立即显示本机完整正文和附件，阅读请求独立排队，不禁用用户列表；已读标题变化或新消息加入后，文字选择仍对应原消息正文。登录窗口明确说明自动加密保存登录状态，不保存原始密码。

已读提交和状态拉取优先于普通同步、发送与附件任务。正在执行的附件请求完成当前块后让出线程，保存既有进度，处理阅读后继续传输。中央服务器仍按账号、会话保存权威已读位置，本机保存缓存与离线待提交意图。附件是独立本地文件，数据库保存元信息与下载状态；完整校验后显示「已缓存到本机」，另存为从缓存复制，服务器仍保留原件。具体路径和行为见 [客户端部署说明](CLIENT_DEPLOYMENT.md)。

x64 与 Win32 正式程序各通过 8 项交互专项：真实 TLS 附件请求阻塞期间立即显示正文、已读先于下一块提交、真实 WSS 通知发送方与同账号另一设备、下载续传后逐字节校验、正文选择保持、20 次稳定选择及同步没有用户行销毁或重排，以及服务器停止后的原生另存为。记录分别为 build/tests/ui-responsiveness-x64-538decb74f4850203b6d80ce/results.json 和 build/tests/ui-responsiveness-Win32-9e8185f47bb6cc9880abe10f/results.json，实际窗口截图已查看。

四个诊断配置构建与 C 自测、两个正式构建、53 个 C/H 文件格式检查通过。回归包含 x64 Debug HTTPS/WSS 47 项、Win32 Release HTTP/PAKE 13 项、两架构 DPI 各 323 项、正式登录与真正重启恢复各 6 项。版本继续为三段 1.0.1，服务器程序与数据库 schema 未改变；已部署服务器无需为本轮客户端优化替换或重启。源码、版本、原始记录与发布包校验集中于 build/verification/client-responsiveness-1.0.1.json。

专项时延来自隔离的本机网络与受控慢附件，不保证所有网络环境的响应时间；已经发出的请求仍需完成当前块或超时。非整数 DPI 回归作用于隔离原生窗口，未修改桌面缩放，真实物理多屏移动仍待现场验证。以下章节保留先前构建的历史结果。

## 2026-10-11 历史交付：1.0.1 账户与在线管理

已补齐 account-list、account-display、account-delete。显示名独立于登录名；删除撤销全部登录、清除认证材料并释放登录名，保留原身份、最后的显示名、历史消息及已引用附件。同名新账户使用新 ID，不继承旧历史或权限。服务器 schema 升级至 11，先保存加密迁移快照；客户端 schema 仍为 4，新增可重建的 ui_profiles 资料缓存。客户端自动补齐已有参与者资料并更新自己、用户列表、会话标题和历史消息署名，标记停用/删除状态。版本继续使用三段 1.0.1，C 代码遵循原项目规范。

本机管理命令优先通过经过权限检查及实例密钥认证的加密命名管道交给原服务器执行，不打开第二个写库实例。账户、设备撤销、配额、存储查询、清理、备份、恢复包导出及已经授权的 Drive 操作支持在线执行；Drive 操作与后台云备份串行访问共享授权和云任务状态。命令行和离线模式共用执行逻辑。替换程序须正常停止一次并保留原数据目录；数据恢复、运行身份转换、首次 Google 授权和启动参数调整仍按原部署流程操作。具体命令及限制见 [账户与在线管理](ACCOUNT_MANAGEMENT.md)。

正式 x64 HTTPS 和 Win32 HTTP/PAKE 构建各 13 项账户管理验收通过，覆盖原生客户端自动更新、128 字节完整显示名、即时撤销、配额、在线备份/清理、删除后的历史与附件校验、同名新身份隔离，以及真正重启后的免密码登录。记录分别为 build/tests/account-management-x64-tls-ea3a3adcaf054ba4ee15aa8ee369a78d/results.json 和 build/tests/account-management-Win32-pake-c4941d3ecd69f3dc538426ac68fa194f/results.json。本地管理通道 8 项攻击/异常与分页验收通过，包含独立加密客户端、挑战重放拒绝、早退恢复、105 个账号及客户端资料分页；记录 build/tests/admin-channel-x64-d2be8d54043f9c67682494597b152b07/results.json。

四个诊断配置构建和 C 自测、两个正式构建及 53 个 C/H 文件格式检查通过。回归包含 HTTPS/WSS 47 项、HTTP/PAKE 13 项、两架构 DPI 各 323 项、正式登录状态各 6 项、两架构密码确认及账户创建各 7 项。Drive 模拟验收 14 项通过，增加服务持续运行且后台镜像工作时的在线云操作；没有触碰真实 Google 授权。十二个程序的版本查询、文件版本和产品版本均为 1.0.1；证据汇总为 build/verification/account-management-1.0.1.json。

新账户管理已在真实进程与原生窗口中验证，Windows LocalSystem 服务身份下的本轮专项未执行：提权提示被取消，没有创建测试服务或操作已有服务。非整数 DPI 验收只改变隔离测试窗口，未改变桌面缩放，真实物理多屏移动仍待现场验证。以下章节保留其各轮构建的历史结果。

## 2026-10-11 历史复验：登录状态与生成缓存清理

现有 1.0.1 客户端默认以 DPAPI 保存登录状态。使用正式 x64 HTTPS 和 Win32 HTTP/PAKE 程序分别验证：客户端与服务器均真正重启后，无需输入密码即可恢复登录并提交、同步新消息；公开设置不含密码或令牌。明确注销后凭据删除，再次启动保持未登录，本机历史保留。两架构各 6 项通过，记录 build/tests/production-x64-f167283e1c5845f7f8708034c1bb2742/results.json 和 build/tests/production-Win32-d0335b8e58a933737b65b4e2432ccb69/results.json。本轮补充回归和使用说明，没有修改程序二进制及 1.0.1 版本。

清理脚本仅处理过时发布包、编译输出、隔离测试数据和临时设计缓存；保留当前客户端及服务器的 x64/Win32 发布包、已安装构建依赖、验收结果、日志、截图和真实 Google 授权。非隔离目录及含实际应用数据的目录自动保留，运行程序或服务使用的目标不清理。执行范围与结果以 build/verification/cleanup-accessible-20261011.json 及登录状态汇总记录为准；历史验收中的临时数据库和附件不再长期保留。

## 2026-10-11 历史实现：Win32 左右分栏与消息内附件

所有者确认此前提案的布局，并要求在消息中直接显示附件名字。现已实现菜单、窄工具栏、左侧原生用户列表、右侧只读消息文本、可拖动分隔条和分段状态栏。自己置顶，支持选择单个用户或全部消息；发送者、完整秒级时间和保存/已读状态位于正文上方，附件在各自消息下逐个显示原始名称、字节大小和已保存/待下载状态。点击附件名称选中对应文件，明确导出后才写入用户选择的路径；多附件及零字节文件均能准确关联。长正文有明确提示，并在下方完整查看区显示；原生控件支持选择和复制，没有变化的同步快照保留文字选择。

用户来自本机已有消息参与者及精确查找验证过的账号，账号名缓存 ui_contacts 不保存密码或令牌。当前服务器仅有按登录名查找接口，因此未知参与者先显示账号 ID，明确查找后缓存名称；没有增加服务器枚举接口。协议与消息 schema 均保留，产品和可见文件版本继续为 1.0.1。

x64 HTTPS/WSS 全流程 47 项 PASS，记录 build/tests/integration-x64-Release-中文-83d48678e3464cdd8fb6d6080426b705/results.json；包含消息内全部附件名、点击第二个名称后导出对应空文件、账号查找与名称缓存、自己置顶及会话隔离，原有搜索/分页/托盘/恢复/证书拒绝回归也通过。实际截图 tray-ui.png 已查看，附件表格在高 DPI 下显示两个文件行。x64/Win32 原生布局和非整数 DPI 各 323 项回归通过，包含 RichEdit 实际字体格式、状态栏像素边界、短屏滚动和两窗独立 DPI；模拟只作用于独立测试窗口，真实物理多屏移动仍待现场验证。

四个诊断配置的构建及 C 自测通过；两个架构生产构建排除测试入口，实际登录/收发及包校验随本轮一起记录。当前二进制、源码、DPI、生产及新包证据汇总在 build/verification/main-ui-attachments-1.0.1.json，旧提案与旧构建记录保留为历史，不作为当前未实现范围。

## 2026-10-11 历史调整：三段版本与传统 Win32 主界面提案

所有者明确当前开发版本为 1.0.1，不增加显示用第四段，不为普通构建升版。Drop 与 dropserver 的 FileVersion 显示字符串现与 ProductVersion 共用 DROP_VERSION；十二个 x64/Win32 Debug、Release、生产 EXE 的文件属性及 --version 已逐一核对，均为 1.0.1，记录 build/verification/versions-three-part-1.0.1.json。Windows 固定数值资源和程序集身份中系统规定的四段内部格式仍以零结尾，不作产品迭代编号。后续确需另开迭代时，按所有者授权采用 1.1.x。

所有者认为原提案密度偏低、视觉风格偏向浏览器应用，要求采用传统 Win32 工业风格。最新提案为 [client-main-win32-1.0.1.png](design/client-main-win32-1.0.1.png)，同名 SVG 是矢量原稿：菜单、窄工具栏、用户列表、可调整分隔条、连续消息、附件表格与分段状态栏；当前示意窗口显示九条消息，减少卡片留白。保留自己置顶、正文上方的发送者与完整时间、搜索日期分页以及独立配置窗口。所有用户、消息、任务及快捷键均为设计示意；客户端是原生 Win32 C 程序，正式主界面待所有者确认再实现，当前仍使用临时布局。

本轮代码变动仅纠正文件版本显示字符串；四个诊断配置及两个生产架构重新构建，版本核对与头文件格式检查通过。没有修改既有收发或 DPI 逻辑，因此没有重复上一轮业务集成及 DPI 回归；下面的原始验收记录对应其各自的当时构建。新的独立 RAR 与图稿哈希、版本检查和实际归档解压结果汇总于 build/verification/win32-design-version-1.0.1.json。

## 2026-10-11 初次交付（历史）：1.0.1 配置窗口与 DPI

初次交付时 Drop 与 dropserver 产品版本统一为 1.0.1，Windows 文件/程序集版本为 1.0.1.0；文件属性显示已在上面的最新调整中纠正为三段 1.0.1。vcpkg 项目版本、当前部署说明和本地网站页面同步为 1.0.1。协议、数据库 schema 和历史验收记录保持原始编号；过去 1.0.0 包名及记录代表当时实际程序。本轮没有更新正式服务器、真实 Google 凭据或线上网站。

客户端将地址、私有 CA、账号、登录/退出账号以及通知、自启动、正文压缩放在独立配置窗口。公开字段可明确保存，偏好修改自动保存；连接和账号变化在下次登录时使用。登录提交、返回消息和关闭配置时清除密码框，普通设置不写密码。主窗口保留历史、正文搜索/日期、详情/附件导出、消息发送、同步及恢复复核的临时布局。

manifest 声明 PerMonitorV2；两个顶层窗口各自读取 DPI，响应 WM_DPICHANGED，按目标 DPI 重新建立字体、图标和控件矩形，使用 Windows 建议的位置与尺寸。初始尺寸受屏幕工作区限制；短屏采用垂直滚动访问底部按钮。C 代码保持中文注释和完整语句优先单行。

初次提案 docs/design/client-main-1.0.1.png 及同名 HTML 已被所有者要求修改，保留为历史参考；后续实现使用上面的 Win32 紧凑布局提案并等待确认。

四配置的 x64/Win32 Debug/Release 构建与公共、客户端、加密服务器自测通过，零警告、零错误，50 个 C 源文件与头文件格式检查通过。x64 Release HTTPS/WSS 全流程 45 项 PASS（新增独立配置窗口的打开、登录和返回验证），HTTP/PAKE 13 项 PASS。x64 与 Win32 Release 配置/DPI 回归各 288 项 PASS，覆盖保存/重启、密码不持久化、125%/150%/175% 字体重建、控件无越界/重叠、两个窗口独立 DPI 和短屏滚动。证据见 build/tests/integration-x64-Release-中文-d98b46f67cad4976a00467b29f9e2ac3/results.json、pake-x64-Release-中文-643b869d7899053d/results.json、ui-1.0.1-x64-Release-5a5d01b58565416ba53bf04222cf7367/results.json、ui-1.0.1-Win32-Release-8750b061e3e540f986a0251ded11a13e/results.json。DPI 切换由测试构建在隔离窗口模拟，没有修改桌面缩放；真实不同 DPI 物理屏幕间移动尚未现场验收。

x64 HTTPS 与 Win32 HTTP/PAKE 无测试入口生产程序分别完成原生登录、密码清除、中文消息持久同步和正常退出，记录 build/tests/production-x64-c5ba96814f07edc6369810016fe6194f/results.json、production-Win32-c6161152b3bf6a2c1ab373a88c3bdeea/results.json。全部十二个诊断/生产 EXE 的 Windows 资源和 --version 均符合 1.0.1 / 1.0.1.0，记录 build/verification/versions-1.0.1.json。

初次交付的历史 RAR 为 Drop-Client-1.0.1-x64-20261011-142048.rar、Drop-Client-1.0.1-Win32-20261011-142153.rar、DropServer-1.0.1-x64-20261011-142140.rar、DropServer-1.0.1-Win32-20261011-142200.rar。已验证归档设置、真实解压、全部文件 SHA-256、生产二进制和测试入口排除，汇总为 build/verification/ui-1.0.1.json；这些包的文件版本显示纠正前为四段，当前交付以本页顶部新记录为准。

## 2026-10-11 C 代码风格整理

按所有者最新确认，本次只整理风格，产品版本保持 1.0.0，下一版本功能待另行提供。保留工作区已有实现和修改，整理 drop、dropserver、dropcommon 的 49 个 C 源文件与头文件（包含本机 C 自测），不调整既有 Python、PowerShell 验收脚本或第三方上游源码。

根目录 `.clang-format` 使用四空格缩进和 180 列行宽，完整声明、调用和表达式尽量单行，过长字符串按相邻 C 字符串拼接分行，独立语句各自占行。405 个函数定义均有中文用途说明；公开声明、共享结构、资源所有权、事务、认证、续传及跨线程状态也补充了中文约束。13 个文件内部共享变量改用中文，包括网络退出事件、凭据缓存与锁、当前线程认证请求以及服务状态和退出事件；公开符号、协议和数据库字段保持原名。后续开发规范写入 AGENTS.md、README.md 和开发计划。

静态检查确认没有缺少函数注释、没有纯英文项目注释、没有超过 180 列的行，格式重复执行无差异。与整理前逐项比较，只忽略空白、注释、宏续行、相邻字符串拼接以及明确记录的内部变量改名，其他代码符号和字符串内容一致。总记录为 `build/verification/c-style-20261011.json`，包含源文件哈希、改名清单、构建日志及原始运行证据。

x64/Win32 Debug/Release 四配置均零警告、零错误，公共、客户端和加密服务器原生自测通过。x64 Release 的 HTTP/PAKE 13 项完整通过：`build/tests/pake-x64-Release-中文-8d725e0ab190b718/results.json`。HTTPS/WSS 首轮完成 32 项后，在损坏缓存自动重下载步骤出现 Windows 12030、phase=3；服务端请求日志记录 200，连接中断根因未确定，失败保留于 `build/tests/integration-x64-Release-中文-e402a9d9e3bc48bd9075ea1a387aa6b8/results.json`。未调整超时或加入重试，原脚本在全新隔离目录复验 44 项完整通过：`build/tests/integration-x64-Release-中文-a48a74ab00e74e4a8b40b8994b58b2bc/results.json`。单轮复验不代表长期网络稳定性已验证。

受限构建第一次停在多进程调度阶段；核对命令及父子关系后只结束本任务的等待构建，在常规环境执行原有构建脚本通过。本次未重新打发布包，也未访问正式部署或启动下一版本功能开发。

## 2026-10-11 最新交付：首次配置脚本

新增 scripts/Setup-DropServer.ps1、Setup-Drop.ps1 及共用 Setup.Common.ps1，并随各自的独立发布包提供。服务器只指定程序目录、数据根目录，自动派生 server、certificates、backups、recovery、config；默认 HTTPS / 8443，可指定其他端口或 HTTP/PAKE。证书链和私钥合并保存、Google 桌面 OAuth 可选授权、账号和恢复密码两次本机遮蔽输入、机器 DPAPI、LocalSystem 服务、局域网防火墙规则和接口验证依次执行。已有服务、非空数据根目录、端口占用、链接路径均受保护，不自动重建存储。客户端辅助脚本保存公开连接设置并在数据根目录生成快捷方式，保留已有设置及凭据，不写 AppData/桌面。

Windows PowerShell 5.1 与 PowerShell 7 普通用户验收共 8 项 PASS：默认/自定义端口和无写入预览、错误路径与端口拒绝、中文空格目录、快捷方式/设置保持、实际 Drop 窗口加载。记录 `build/tests/setup-wizard-d17fa450ac445d7d/results.json`。管理员隔离验收 14 项 PASS，记录 `build/tests/setup-wizard-d58e7d441d32b7ab/results.json`：x64/Win32 实际 HTTP/PAKE 服务、x64 HTTPS 服务、独立恢复包、双密码确认、机器 DPAPI、权限、备份目录、局域网规则、错误配置保护及测试对象清理。HTTPS 用未安装到系统信任的隔离证书，服务真实启动且可用明确测试 CA 访问；脚本正常 Windows 验证按预期拒绝该证书，没有关闭校验。没有在正式部署机运行脚本或重新操作真实 Google 授权。

本轮最新独立 RAR 为 Drop-Client-1.0.0-x64-20261011-132814、Drop-Client-1.0.0-Win32-20261011-132814、DropServer-1.0.0-x64-20261011-132814、DropServer-1.0.0-Win32-20261011-132814（build/packages，扩展名 .rar）。沿用已验收的 1.0.0 生产 EXE，只修改配置脚本、文档和打包集成；原有工程和程序版本保持。本轮包校验记录为 build/verification/setup-rar-release-20261011.json。首次配置及日常更新步骤分别见 SERVER_DEPLOYMENT.md / CLIENT_DEPLOYMENT.md；已有服务器不运行初始化向导。

本轮整理状态为 PARTIAL，`build/verification/setup-cleanup-20261011.json`：四个旧包及附属文件共 16 个路径已删除，仅保留新版四个 RAR。管理员清理 UAC 被取消，4 个受保护的 setup-wizard 测试根保留；全量清理另检测到 build/bin/x64/Release/dropserver.exe 的活动进程，因此未继续删除开发缓存和测试数据，未结束未知进程。此前约 32 GiB 普通缓存和 9 个旧保护目录的清理仍已完成；本次新增测试数据的保留不改变此前记录。

## 2026-10-11 交付更新：两次密码确认、独立 RAR 发布包与缓存整理

所有者已确认另一台电脑的网络连接失败由代理引起，调整代理直连后正常。客户端说明补充系统代理域名例外。此前源码和单独更新的服务器 EXE 已含两次密码确认，但上次完整服务器 ZIP 未重打；本次以最新源码重新构建两个架构并提供独立客户端和服务器 RAR，避免继续部署较早的服务器程序。产品版本保持 1.0.0 / 文件版本 1.0.0.0。

实际解压出的 x64/Win32 服务器完成 14 项确认验收，`build/tests/password-confirmation-d938e052502f1cf8/results.json`：创建、重置和恢复导出两次一致才保存，不一致取消且数据库或既有恢复包不变；独立备份恢复和拒绝重定向输入均通过。两个实际客户端包分别完成 7 项 HTTPS/HTTP-PAKE 原生登录、密码清空、大小写账号保护和中文正文收发，证据为 `account-creation-9d90fb97f0d16a8a/results.json` 与 `account-creation-a3205434490b0228/results.json`。

上轮独立包（已由上方包含配置脚本的新版包替代）：

- Drop-Client-1.0.0-x64-20261011-035207.rar
- DropServer-1.0.0-x64-20261011-035222.rar
- Drop-Client-1.0.0-Win32-20261011-035224.rar
- DropServer-1.0.0-Win32-20261011-035227.rar

RAR5 使用 m5、固实、指定 32 MiB 字典、3% 恢复记录、BLAKE2sp 和相同文件引用（含小文件），每包附 SHA-256 与参数记录。本机 RAR 7.01 对这些小包自动优化字典，x64 实际为 16 MiB，Win32 为 8 MiB，参数记录如实区分指定值与实际值。四包已经通过 RAR 检测、RAR 实际解压及完整清单 SHA-256，参考 `build/tests/rar-packages-3061ffac6eac421e/results.json`；文档同步后再次校验最终归档，最终报告为 `build/verification/rar-release-20261011.json`。独立 Drop 包不含服务器；服务器包附停止现有服务、覆盖 EXE/DLL、沿用原数据启动的更新步骤。

`.gitignore` 排除 tests/、scripts/Verify-*.ps1、dropcommon/self_test.c；14 个已跟踪测试条目已从 Git 索引移除，本机源文件保留。生产构建不编译自测源；缺少本机 self_test.c 的源码发布自动关闭诊断代码，项目求值确认见 `build/verification/source-without-tests-20261011.json`。

普通权限可访问的 225 个生成缓存和旧包目标已清理，逻辑大小 32.387 GiB，保留 103 个历史 results.json，证据 `build/verification/cleanup-accessible-20261011.json`。E 盘此步可用空间增加约 34.6 GB。随后管理员清理完成，`build/verification/cleanup-20261011.json` 为 PASS，跳过列表为空；此前 9 个 Administrators/SYSTEM 保护的旧服务目录已处理，再清理约 123 MiB，并保留 113 个历史结果。仅保留当前四个 RAR 及参数/哈希、相应解压目录和最新生产程序。保留依赖安装树、vcpkg 源码、真实 Google 授权测试目录和本机验收源文件，不操作正式部署目录与外部授权。

## 2026-10-11 现场排查：另一台电脑连接失败

所有者报告另一台电脑测试 `https://dropserver.jsnowly.win:8443` 时显示网络连接失败，ping 正常。从开发电脑只读访问，域名解析为 `10.10.10.62`，绕过代理且保留正常证书校验的 HTTPS `/api/v1/server-info` 返回 200，协议 1、版本 1.0.0、tls。开发电脑的正式 x64 客户端使用现有系统代理时复现 network_error / Windows 12029；同机经现有代理请求也在 TLS 握手时被重置。证据 `build/tests/deployment-connectivity-20261011.json`。

这表明从开发电脑到正式服务器的直接 HTTPS 路径正常，不能据此确定另一台故障电脑的原因。下一步在故障电脑检查域名解析、8443 TCP、绕过代理的真实 HTTPS 接口和 Drop 测试结果。未修改任何电脑的代理、防火墙、证书、服务或正式数据；没有执行正式账号登录、收发或 WSS 验收。

## 2026-10-11 完成：设置密码二次确认与独立客户端包

创建/重置账号和导出独立恢复包现在在首次遮蔽输入后要求 `Confirm password:`，两次完全一致才保存；不一致取消并清除内存中的密码。账号密码至少 12 UTF-8 字节，恢复密码至少 16 UTF-8 字节。登录和使用已有恢复包输入已有密码一次，仍拒绝重定向密码。原独立恢复包格式与密钥身份保持兼容。

x64/Win32 生产构建通过；两架构密码确认各 7 项实际 PASS，包含不一致不建立/覆盖恢复包、不保存新账号、不重置原密码，以及确认后导出/实际备份恢复。证据分别为 `build/tests/password-confirmation-77e9a83ac6675741/results.json` 和 `password-confirmation-9c3a814d697916e1/results.json`。

按所有者要求提供单独的 `build/packages/Drop-Client-1.0.0-x64-20261011-032758.zip`：只有 drop.exe、5 个实际导入运行 DLL、客户端 README、许可及校验清单，不带服务器程序、CivetWeb/TLS 服务 DLL、运行数据或凭据。打包入口为 `scripts/Package.ps1 -Component Client`，默认完整包行为保留。实际包中客户端的 HTTPS 和 HTTP/PAKE 原生登录、密码框清空、中文正文发送/同步均通过，7 项证据为 `build/tests/account-creation-c1b7a6edcf6681e5/results.json`；ZIP/逐文件校验及生产程序一致性为 `client-package-20261011.json`。服务器密码确认修正需部署者另外替换对应架构的 build/production 下 dropserver.exe 后生效，未访问或改动正式部署电脑。

## 2026-10-11 修复：账号名支持大写英文字母

所有者在实际部署使用 `Jsnowly` 创建账号，原 ASCII 小写限制导致输入密码后只显示 Operation failed。现已放开 A–Z，创建及 HTTPS 登录共用账号规则；HTTP/PAKE 标识保留输入大小写，两种模式均可直接使用 `Jsnowly`。沿用现有数据库的大小写精确匹配，不改名、不合并现有小写账号，不需要数据库迁移。产品版本保持 1.0.0。账号创建另区分参数、重复账号、凭据生成和数据库写入错误，失败不会覆盖已有密码。

x64 与 Win32 生产构建通过；`tests/account_creation.py` 的 14 项实际验收全部 PASS：混合大小写账号创建、不同大小写身份分离、重复创建保留原密码、经证书校验的 HTTPS 登录，以及两架构原生客户端使用 `Jsnowly` 分别通过 HTTPS 和 HTTP/PAKE 登录、清空密码框、提交并同步中文正文。证据为 `build/tests/account-creation-c33ab839b4e2507f/results.json`。部署电脑尚未由本机访问，需要部署者替换更新程序并复验；实际 Google 授权与长期观察继续按原现场范围执行。

本次更新包为 `build/packages/Drop-1.0.0-x64-20261011-030849.zip` 和 `build/packages/Drop-1.0.0-Win32-20261011-030847.zip`，替代下面双通信模式记录中的先前包；含大写账号支持与本轮部署/验收说明，不包含真实 OAuth 配置、证书、账号或运行数据。

## 2026-10-11 完成：HTTP/PAKE、保留 HTTPS、程序旁 Data

所有者选择 HTTP + PAKE 会话加密，允许账号盐和会话随机数；随后要求同时保留 HTTPS/WSS 模式，HTTPS 只使用 TLS 加密传输，不叠加 PAKE。现已实现：不带 --certificate 时运行 HTTP/WS + SRP-6a/XChaCha20-Poly1305；带证书时运行原 HTTPS/WSS。地址显式选择，不自动降级。密码不通过 HTTP 发送，服务器和备份存储加密保留。默认应用数据为 EXE 旁 Data\Drop 与 Data\DropServer，手动路径保留，无 AppData 回退，不自动搬动旧目录和 Google 授权。

四配置构建及原生自测、x64 Debug/Release 的 HTTPS 完整回归、x64/Win32 Release 的 PAKE 独立互操作与故障验收、两种真实 LocalSystem 服务生命周期、实际两架构生产包的两种模式界面收发均已记录。PAKE 验收包含错误密码、伪造服务器证明、公值范围、标签篡改、路由替换、重放/乱序、跨账号中文及多块附件、原生加密 WS、缓存凭据重连和设备撤销。服务器 schema 10 建立 account_pake 和加密迁移快照；旧账号通过本机 account-reset（可输入原密码）补建材料并重新登录，消息/附件保留。目录不可用时明确失败，已在复制的实际发布程序上验证。

本轮包为 build/packages/Drop-1.0.0-x64-20261011-022927.zip 和 Drop-1.0.0-Win32-20261011-023057.zip；旧包未包含 PAKE，部署需同时更新客户端/服务器。详见 PAKE.md、DEPLOYMENT.md 和 VERIFICATION.md。先前 AppData 目录的 Google 真实授权属于历史验收，新 Data 目录未重新授权，部署时按选定目录完成。不同物理电脑、24 小时持续运行、真实 Google 服务身份自动备份等现场项目仍待部署；PAKE 集成没有独立安全审计。

更新日期：2026-10-11。产品范围以 ../DEVELOPMENT_PLAN.md 为准。

| 阶段 | 状态 | 验收 |
| --- | --- | --- |
| 零：工程与格式 | 完成 | 保留原解决方案、drop GUID 和四种配置；新增两个 C 项目。固定依赖、C17 x64 Debug/Release、公共自测及格式文档通过。Win32 Debug/Release 构建、自测及 Release 完整原生集成验收通过。 |
| 一：服务器基础 | 完成（前台开发验收） | SQLCipher、用户 DPAPI、Argon2id、设备凭据、HTTPS、隔离 CA/错误证书、权限、崩溃重启、令牌轮换/撤销及 100 次短连接回归通过。实际验证限制见验收记录。 |
| 二：文本同步 | 完成（诊断客户端验收） | 同账号两个原生客户端和另一账号收发、离线队列、重启补齐、幂等重试、分页/代次、证书校验 WSS 及非空 schema-1 加密迁移通过。连续后台循环和托盘留在后续阶段。 |
| 三：文件同步 | 完成（诊断客户端验收） | x64 Debug/Release：稳定副本、多附件、独立对象密钥、认证分块、上传/下载进程中断续传、自动完整下载、权限与实际密文篡改拒绝、非空格式迁移通过。 |
| 四：托盘界面 | 完成（本机原生验收） | x64 Debug/Release 原生窗口、托盘、后台 HTTP/WSS、账号选择、多附件、查询/导出、自启动开关、退出和补齐通过；Debug 额外 50 条分页与日期过滤通过。实际 Windows 登录/通知显示及多显示器 DPI 未验证。 |
| 五：可靠性与压缩 | 完成（本机故障验收） | Debug 完整验收及实际 SQLite-full 同步整页回滚通过；Release 完整回归通过。心跳超时、退避、压缩互读、配额、缓存修复、保存/已读及宽限清理已实现。 |
| 六：服务与备份恢复 | 完成（实际服务与隔离恢复验收） | Debug/Release 在线加密备份、独立恢复包、新目录正文/完整附件恢复、旧快照代次核对与明确重试、实际备份篡改拒绝通过；真实管理员 ACL/所有者、LocalSystem DPAPI/SQLCipher HTTPS 登录、SCM 启停重启/卸载通过。 |
| 七：基础版发布 | 完成（本机发布候选验收） | 四配置构建与原生自测、x64/Win32 生产包运行、连续 500 条消息、资源采样、实际 ZIP 全文件校验通过。4 GiB + 17 字节收发、备份中断续作、新目录恢复与校验篡改拒绝通过。 |
| 八：Google Drive | 完成（真实云盘与模拟故障验收） | x64 生产候选的真实浏览器授权、令牌刷新、7 个密文文件上传/下载、逐文件 SHA-256、独立恢复包新目录恢复、原正文及托盘完整附件接收均通过。Debug/Release 持久续传、故障重试及后台失败隔离模拟通过；真实限流/配额故障仍未演练。 |

故障与账号演练使用 build 下隔离目录；2026-10-11 经所有者授权，在应用默认目录另行初始化空存储并完成全新 Google 授权和密文基线备份校验，没有复制测试账号或凭据。实际安装了随机 DropTest 隔离服务，验收后卸载；未改变防火墙及系统证书信任。临时系统信任测试因 Windows 确认阻塞已取消，核查未新增测试根证书；改用应用内明确 CA 文件。

## 当前可运行入口

`scripts/Build.ps1 -Platform x64 -Configuration Debug -Test` 构建原解决方案并运行公共、客户端和加密服务器自测；Release 同样可执行。`scripts/Verify-Integration.ps1` 启动隔离 HTTPS 服务器，逐项结果保存于 `build/tests/integration-*/results.json`。

`drop.exe` 无参数启动原生托盘界面，提供服务器/账号设置、连接测试、正文和多附件发送、历史查询、未读及提醒、自动完整下载和显式导出；关闭窗口后保留托盘。原有诊断入口保留。`dropserver.exe` 提供加密初始化、本机遮蔽密码输入的账号创建和可选 HTTP/PAKE 或 HTTPS/WSS。本地在线备份、恢复包、账号管理和服务入口已接入，两种模式的实际服务身份及生命周期已验收。`python tests/pake.py --configuration Release` 与 `scripts/Verify-Service.ps1 -Transport pake` 是新增隔离验收入口。

## 故障与修复

- Tcl 9.0.3 Windows port 删除 bundled SQLite 源码后仍构建不需要的扩展，产生 C1083；项目 overlay 排除该扩展，Tclsh / SQLCipher 重建通过。
- CivetWeb 1.16 Windows 主动关闭 HTTPS 短连接时丢失响应，100 次请求中出现 14–23 次失败；进程仍运行且写入返回成功。项目 overlay 在 TCP 关闭前执行最多 1 秒等待的完整 TLS 关闭，同时允许连接复用；100 次客户端主动关闭请求不重试全部通过。C++ wrapper 和未使用的 WebSocket 压缩关闭。
- 受限构建环境中 MSBuild 节点复用阻塞；脚本限制两个节点并关闭复用，终止了本项目阻塞构建进程，没有结束用户会话。
- 过期证书测试已使用固定过去的 notBefore/notAfter，适配 OpenSSL 3.6，实际过期拒绝通过。

阶段零至八的本机实现及记录的真实/模拟验收已完成，阶段八真实云盘演练使用 x64 生产候选。不同物理主机、实际休眠/锁屏、Windows 重登录、通知显示、多显示器 DPI 和 24 小时以上运行仍需现场演练；当前仅有密钥版本 1，未实现换钥。具体边界和证据见 VERIFICATION.md。

阶段五首轮完整 Debug 回归记录：`build/tests/integration-x64-Debug-中文-35efa21b4e0b4fb2a515087e9f50c4ca/results.json`。实际验证丢失正文确认后的幂等重试、跨设备账号已读、越权确认拒绝、完整附件确认到发送端、异步 WinHTTP/WSS 及原生 UI。前两轮续传网络错误保留失败目录，随后完整 Debug/Release 故障回归通过。真实 Windows 休眠及系统卷耗尽未演练。

阶段五完整 Debug：`build/tests/integration-x64-Debug-中文-5467ddc97f944760a12e2b2b09cde2fb/results.json`，包括实际 SQLite 页容量耗尽的同步整页回滚、解除限制后补齐。Release 回归发现验收脚本按目录枚举顺序选错旧迁移快照，已改为按写入时间选最新快照；失败记录保留，最终 Release 完整 PASS：build/tests/integration-x64-Release-中文-51f9cd3cfe5548e0a1285337deedfea2/results.json。另一次早期 Release 用例误要求分批同步尚未完成的附件立即导出，已改为等待该附件确实完整保存。

阶段六在线备份恢复完整 PASS：Debug `build/tests/backup-Debug-中文-8893042637be470db82f1e88c9bea9ec/results.json`，Release `build/tests/backup-Release-中文-13bedf8514f946d0bd4631351ac08f34/results.json`。仅使用独立恢复包和加密备份在新目录恢复，不使用原机 DPAPI 包。不同物理 Windows 主机仍未演练；真实服务身份与生命周期另有下方管理员验收证据。

阶段七故障与修复：4 GiB + 17 字节全部上传后，完整校验超过 HTTP 超时。增加 schema 8、两条后台校验线程和可查询 validating/finalize_error；校验前不发布消息。保留目录重用分块的 Debug 复验最终完整 PASS：`build/tests/backup-Release-中文-324df4d89e5e4bb7a910b5533e03fbae/results.json`（目录名保留首次 Release 失败名称，结果记录实际 Debug 配置）。完成收发、备份实际进程中断及同任务续作、全新目录恢复、保留较新客户端副本/暂停不确定发送、明确重试及备份密文篡改拒绝；首次失败另存 initial-failure-results.json。

后台校验专项完整 PASS：`build/tests/finalize-Release-f8fad50026824d70847d10769ce24751/results.json`。真实 64 MiB 上传校验时仍可提交正文；强制终止后全部分块保留，重启重新验证；磁盘块篡改持久报告 crypto_error，使用原块明确修复后才能完成。自测脚本每次使用独立 UUID 目录；用于稳定触发中断的延时钩子仅在诊断构建存在。

阶段八最终 Release 模拟完整 PASS：`build/tests/drive-Release-2c0f9bfd51164393b52d6777e5610a8c/results.json`。服务器原生 C 后端使用独立 HTTPS 模拟服务；包含 56 条目录两页读取、路径穿越拒绝、密文续传/下载/认证恢复和真正托盘接收、后台配额失败仍提交正文，未访问真实 Google 账号。完整命令、授权和限制见 GOOGLE_DRIVE.md。

最终 Debug 重新构建与原生自测通过；阶段八 Debug 同一完整模拟也全部通过：`build/tests/drive-Debug-d815f8f1315849dc9e6539b976b98dc6/results.json`。服务器当前 schema 9、客户端 schema 4，x64 和 Win32 的依赖安装目录独立，切换架构不会移除另一架构依赖。

阶段七 Win32 Release 完整 PASS：`build/tests/integration-Win32-Release-中文-d4cdb17e23ef42769a4547cff723a069/results.json`。最终生产包原生验收完整 PASS：`build/tests/production-x64-808ae4f0b00417ffb521fb92baf71fc8/results.json`、`build/tests/production-Win32-84535d990ef3c1810f5861a1b4f141cf/results.json`。两架构分别测量登录后空闲 30 秒及连续 500 条消息时的 CPU/工作集；详细数值和采样边界见验收记录。

当前长期部署试用包：`build/packages/Drop-1.0.0-x64-20261011-004210.zip`、`build/packages/Drop-1.0.0-Win32-20261011-004210.zip`，各有同名 .sha256 与包内 files.json。Drop 和 dropserver 统一为产品版本 1.0.0、Windows 文件版本 1.0.0.0，使用所有者更新的两个图标；新增默认服务器数据目录和手动覆盖，通知策略和独立开关、云盘后端、后台大文件校验及服务修正保留，排除测试入口/口令、证书和运行数据。文档更新后由 `scripts/Refresh-PackageDocs.ps1` 同步重算清单与 ZIP 校验值，`tests/package.py` 验证真实归档的每个文件。保留先前候选，未向 GitHub 发布 Release；原 MIT 作者/年份占位符须所有者在公开再分发前确认。版本号不表示尚待现场验证的项目已通过。

管理员服务验收实际揭示 ChangeServiceConfig2 设置自动重启所需 SERVICE_START 访问权缺失，已补齐；安装失败回滚临时服务。目录验收脚本混用旧 PowerShell 模块环境也已改为当前 pwsh。失败记录保留于 service-Debug-9c5cc4bc974a46e9860b4bba6ade34a4、service-Release-46b589938cb04c91a097d656993971a2、service-Release-a4e9d7f8341a4e568755e4bbcb9ad8ad。修正后实际管理员服务完整 PASS：build/tests/service-Release-c7d79aa9427046d69fe00828128beb2b/results.json，临时服务最终卸载。

阶段六最终服务结果：Windows 管理员确认后，整个现有目录树只保留 SYSTEM/Administrators 权限、Administrators 所有者；LocalSystem 服务实际解锁 DPAPI/SQLCipher 并登录，SCM 停止重启保留身份/凭据，随机服务最终卸载。首次故障均保留，最终结果不是编译推断。

## 2026-10-10 分支与图标更新

GitHub KingwithQueen/Drop 的默认且唯一分支已设为 `主要`，删除 `master`；原 master 提交 9446542fdf89fba0a49be21ede8f381d6b563626 已包含在主要的 9b5e8bad20a94d215f6a751fb2e5ac6b20fbec37 历史内。删除使用旧提交 SHA 的租约保护，随后重新读取 GitHub API 验证唯一分支，本地 origin/HEAD 同步为 origin/主要。记录：`build/tests/branch-primary-20261010.json`。

客户端使用根目录 Dropico.png，服务器使用 DropServerico.png。`scripts/Generate-Icons.py` 将原图编码为 16/20/24/32/40/48/64/128/256 像素 ICO，保留透明通道和原 PNG；正常构建直接使用已保存的 ICO，无需 Python。两个 EXE 编入原生图标资源，客户端窗口、任务栏和托盘使用同一客户端图标。

x64/Win32 Debug/Release 及两个生产候选重新构建通过；实际读取 12 个 EXE 的 Windows 图标资源，全部九种尺寸与对应 ICO 一致：`build/tests/icons-39d95a8b20954422bd4db1e8b83d4f12/results.json`。当前包的真正窗口截图分别为 `build/tests/visual-production-ef2f7998e8824280a5da681be47aa55d/production-window.png` 与 `build/tests/visual-production-7a12400b1b824936b4597b6a9c209e3e/production-window.png`。本轮只改图标及文档，业务验收证据保留；图标代码和文档更新位于本地工作区，未提交推送。

阶段八的真实 Google 浏览器授权、上传、下载和完整恢复已完成；其余待现场验证的项目仍以 VERIFICATION.md 为准，未实现密钥轮换。

2026-10-10 真实 Google 完整 PASS：`build/tests/google-real-3daea6187076497fada56f20e3d6e93a/results.json`，10 项检查。使用当时的 x64 生产候选 20261010-211910 的原生 OAuth/DPAPI 和实际 Google Drive API；生产托盘发送中文/emoji 正文及 3145745 字节附件，上传 7 个密文文件共 3351381 字节，下载逐文件 SHA-256 完全一致；仅用独立恢复包在新目录恢复，实际 HTTPS 核对正文并由全新托盘完整保存附件。云端保留一份演练备份 `87008bdff9d0dcc23cf05a850c360b69`，原有备份集合未改变，未删除云端文件。正式服务器需要为实际数据目录配置账号及自行保管的恢复包，演练密码只在内存中。真实限流、配额耗尽、WAN 断网和跨物理主机仍未测试。

先前 Google 测试名单拦截、添加测试用户后的真实授权及首轮验收脚本失败保留在 `build/tests/google-real-4c49237fcc3743d5bf3597b420454f84`。首轮脚本误用附件按钮编号 111，修正为 108 并断言附件数量后，在全新隔离目录完成 `tests/drive_real.py`；产品代码未改变。OAuth 配置和凭据未进入源码或发布包。上述待授权描述属于先前状态，当前已由完整真实 PASS 取代。

## 2026-10-10 通知调整与部署边界

按新确认的要求，最小化和关闭都将窗口收起到托盘；窗口显示时不请求 Windows 通知，收到新消息时请求 Windows 原生任务栏提示，切回窗口停止。隐藏到托盘时才请求新消息通知；托盘在有未读时以 500 毫秒周期闪烁，打开窗口或未读清零后恢复正常。“Windows 通知”和“托盘图标闪烁”是独立且持久的开关，默认开启；兼容旧 notifications 设置，新增 tray_flash。关闭提醒不阻止同步或未读记录。系统通知是否展示和任务栏颜色仍由 Windows 设置决定。

四配置重新构建及原生自测通过。通知专项各 9 项完整 PASS：x64 Debug `notifications-x64-Debug-789a6a22e3ce4066b58e8bdf20c02d70`、x64 Release `notifications-x64-Release-66d477cec79c4fb0a9aeafacf410bf31`、Win32 Release `notifications-Win32-Release-7061151e029f47308a9b780c053e5ece`，均在 build/tests 下保存 results.json。实际 HTTPS 消息、原生窗口/最小化/托盘回调、成功的 Shell 调用、图标交替帧、关闭/重开选项、会话及设置重启和已读停止均通过；计数诊断在生产构建完全排除，不把 API 请求验收称为系统弹窗或橙色任务栏视觉确认。首两轮脚本因复用超时 HTTPS 连接和同步鼠标按下等待释放失败，记录保留；改为独立已验证连接及现有 HOME 历史选择方式后全部通过。

本轮 x64 Release 完整集成 44 项 PASS：`build/tests/integration-x64-Release-中文-0a6748e6f74e46cfb91f4a86a3efdd8b/results.json`，含正文/文件、压缩、自动启动、分页、断线恢复和真正退出。新的生产包运行 PASS：`build/tests/production-x64-3d8ed518b57a2f3826e5478caa2f6095/results.json`、`build/tests/production-Win32-76c1bb2cfab611ddfe39323add820186/results.json`，两架构各 4 项，含遮蔽账号管理、生产入口排除、原生中文收发和 30 秒空闲采样。两个新包的 dropserver 及各 8 个运行 DLL 与前一候选逐字一致；本轮改的是客户端界面/设置，原 Google 后端证据继续适用，未再上传新的真实云盘演练数据。

当前可以准备小范围部署试用，不能称全部现场测试完成，也不能仅靠 Google 改为正式发布状态就视为服务器已部署。实际数据目录仍需账号、所选传输模式、独立恢复包、Google 授权和备份配置；随后验证实际服务/自动备份、第二台电脑和持续运行。跨物理主机、真实休眠/重登录、Windows 提醒实际展示、24 小时稳定性、服务身份真实 Google 后台镜像等仍待现场验收，完整边界见 VERIFICATION.md 和 DEPLOYMENT.md。


## 2026-10-10 1.0.0 与公开网站

用户更新 Dropico.png 与 DropServerico.png 后，重新编码九种尺寸的原生 ICO，源 PNG 保持不变。Drop / dropserver 产品版本统一为 1.0.0，Windows 数值/文件版本 1.0.0.0；dropcommon/version.h 供 C、RC 与打包脚本共用。版本查询、服务器信息、HTTP User-Agent、客户端标题和托盘提示更新。修正历史刷新覆盖标题版本的问题，两个最终生产窗口截图已查看。协议 1、服务器 schema 9、客户端 schema 4 和密钥版本 1 未改变，不做数据迁移。四配置重新构建及原生自测通过；Debug 资源正确标记，Release/生产不标记。

实际 12 个 EXE 资源/版本/命令检查：build/tests/version-icons-1.0.0-dda24a4ed48e49a7a2392653bd715000/results.json，全部通过；每个程序的九种图标图像字节等于新 ICO。首轮资源脚本误要求 RC 保留 PNG 图标的 planes=0，而 Windows RC 规范化为 1；修正检查后图像字节一致通过，原失败 version-icons-1.0.0-a89cf144d209459e87959b1d8aaf547d 保留，不是程序图像损坏。

最终包内实际运行各 4 项 PASS：production-x64-00b7921c700dc299890bb7f8e53f44c7、production-Win32-482740fc9fb42c8b603752960cb5f11f。1.0.0 云盘 Release 模拟 13 项 PASS：drive-Release-7e14a1ad553c4dd6b81e8315c85dd334，保留此前真实 0.1.0 x64 云盘成功证据，本轮未增加真实 Google 上传或删除。新版程序包为上文 234432 / 234435；首版 233422 / 233429、先前 0.1.0 候选及其成功/失败记录保留。程序改动仍在本地，未向 GitHub 提交或推送。

用户已授权 Google 正式发布以开始长期部署测试，但用户报告控制台缺少首页和隐私说明。按用户的 drop.jsnowly.win 域名方案制作公开静态网站，Sites 部署 succeeded。所有者确认已添加三条 DNS 后，于 2026-10-11 00:02（中国时间）确认域名、提供商、HTTPS 均为 active，https://drop.jsnowly.win/ 和 https://drop.jsnowly.win/privacy.html 实际返回 HTTP 200；原始托管地址 https://drop-message-sync.owlwithleaves.chatgpt.site 也返回首页。发布记录为 build/tests/website-20261010.json，公共 DNS/HTTPS 响应摘要为 build/tests/website-dns-20261011.json。已向所有者提供 Google 品牌信息填写项，Google 必要域名验证、正式发布状态及实际目录重新授权等待反馈，不能报告 Google 已转正式。网页源码同时保存在 website/dist，网站自己的 Sites 仓库已推送，与主程序 Git 分支隔离。操作和隐私说明见 WEBSITE.md / GOOGLE_DRIVE.md。

2026-10-11 用户报告品牌资料已填写，但缺少应用服务条款链接。新增 website/dist/terms.html 并更新三页导航/页脚，使用同一 Sites 项目发布第 2 版，源码提交 27831d26a607b1b8b9be125e92f40b8a9921905d。三页静态链接/资源、八文件归档及实际 HTTPS 200 检查通过，记录 build/tests/website-terms-20261011.json；服务条款链接 https://drop.jsnowly.win/terms.html 已提供给用户。网页变更不扩大 Google 权限，也不改变程序。最新说明同步到上述 1.0.0 试用包并重验文件/ZIP 清单。

2026-10-11 所有者确认 Google 控制台“显示正式版了”，据此记录发布状态为 In production，证据 build/tests/google-production-publishing-20261011.json；不将所有者反馈称为本机独立控制台验证或品牌审核通过。当时待确定服务器目录，本机检查未发现运行中的 Drop 程序/服务，三个已检查的常用目录不存在。随后所有者选择由程序提供默认目录，实际授权和基线备份的完成结果见下节；此前隔离测试数据和授权保留，持续 24 小时及超过七天的现场结果尚未产生。

## 2026-10-11 默认服务器目录与正式状态下重新授权

按所有者要求，省略 --data 时使用 Windows 当前用户的 LocalAppData\DropServer，部署者可在每条数据命令用 --data 手动覆盖。data-directory 只显示有效绝对路径；不依赖程序所在位置、当前工作目录或伪造的 LOCALAPPDATA 环境变量。服务安装把解析后的绝对路径固定在启动参数中，service-run 缺少该参数时拒绝运行，避免使用服务账号个人目录。目录改变不自动迁移旧数据或授权。

四配置原生构建/自测通过。默认/中文空格自定义目录、重开保留加密身份、独立目录、错误参数和服务路径检查各 6 项 PASS：server-directory-x64-Release-6404404f6a5fcef488df6e9f1449136c，实际新 x64 包 23f2899094e5daeee31d049b208cc17d，实际新 Win32 包 4721a99ca0d4a79a62a90e6c6b47a5fa；完整记录均在 build/tests 对应目录。x64 Release 完整集成 c6d52c3d1c2f444c9a0ee2b9e6dc7ce2 的 44 项通过；随机服务 service-Release-1d7fbc58ec3e4584804890d80303636b 的 5 项实际管理员/LocalSystem 生命周期通过，验收后卸载。12 个程序的最新图标/版本资源检查通过：version-icons-1.0.0-da793e5397cf4843a91d63eb9ac2f172。两包客户端和八个运行 DLL 与上轮 UI 验收逐文件 SHA-256 一致，记录 client-runtime-default-20261011.json，未重复测量此前 500 条消息资源结果。

Drive 模拟首次 drive-Release-07d1289b9ef54f0ea7800bf38f7735b7 在恢复后附件读取处发生 10 秒超时，记录保留；完整集成结束后相同脚本单独重跑 drive-Release-9b1be17a317a464a815ba857b684f808 的 13 项全部通过，没有修改请求超时或增加隐式重试。首次超时原因尚无结论，单轮重跑通过不作为长期稳定性证明。

使用新 x64 生产包初始化本机默认 C:\Users\JLYis\AppData\Local\DropServer，并由所有者完成全新系统浏览器授权；授权 exit 0，凭据用用户 DPAPI 保存在该目录，未从隔离目录复制。新进程刷新及真实 Google API、空存储加密基线备份上传、下载全部路径/长度/SHA-256 检查共 5 项 PASS：build/tests/google-production-default-20261011.json。两文件共 204976 字节，云端保留 ea42af015c35ecc44e524ee8d8974dbb，原有备份仍在，未执行远端删除。该默认目录无测试账号或消息，尚未创建实际 TLS/独立恢复包，未常驻启动服务器或安装长期服务。自有部署配置、该目录独立恢复与自动镜像、第二台电脑及长期运行继续验收；部署时可自行改目录，新主机或新目录须重新授权。
