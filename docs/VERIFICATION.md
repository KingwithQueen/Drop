# 当前验收记录

更新时间：2026-10-10。以下为当前结果；后面的历史记录保留各阶段当时的状态，不代表当前未完成范围。

| 项目 | 当前结果 | 证据与边界 |
| --- | --- | --- |
| 原工程和四配置 | 通过 | 保留解决方案/主项目 GUID；C17、UTF-8、/W4 /WX，x64 与 Win32 Debug/Release 原生构建/自测。 |
| x64 完整集成 | Debug/Release PASS | Debug 5467ddc97f944760a12e2b2b09cde2fb；Release 51f9cd3cfe5548e0a1285337deedfea2，完整 results.json 在 build/tests/integration-<架构>-<配置>-中文-<ID>。 |
| Win32 完整集成 | Release PASS | d4cdb17e23ef42769a4547cff723a069，包括原生托盘、中文多附件、续传、确认、心跳退避、压缩、配额、TLS 拒绝。Debug 当前完整原生自测通过。 |
| 加密和事务 | PASS | SQLCipher 错密钥/实际页篡改、非空迁移、附件块/清单篡改截断、SQLite_FULL 整页同步回滚、解除限制后补齐。未填满 Windows 系统卷。 |
| 本地备份恢复 | Debug/Release PASS | backup-Debug-中文-8893042637be470db82f1e88c9bea9ec；backup-Release-中文-13bedf8514f946d0bd4631351ac08f34。独立包/新目录恢复、较新本机副本保留、不盲重发及明确重试。 |
| 大于 4 GiB | 完整 PASS | 4294967313 字节，4097 块；backup-Release-中文-324df4d89e5e4bb7a910b5533e03fbae/results.json。修正后实际 Debug 重用原数据，收发、备份进程中断后同任务续作、完整恢复、较新客户端历史和文件保留、不盲重发、明确重试及备份密文篡改拒绝均通过。目录保留首次 Release 名称，首次失败另存 initial-failure-results.json。 |
| 后台完整校验 | 实际故障 PASS | finalize-Release-f8fad50026824d70847d10769ce24751/results.json：64 MiB 真正后台校验期间正文提交成功、未完成附件不可引用；实际终止进程保留 64 块，重启再校验；篡改失败持久化，原块明确修复后才能完成。诊断延时钩子不进入生产。 |
| Windows 服务 | 实际 PASS | service-Release-c7d79aa9427046d69fe00828128beb2b：管理员目录树 ACL/所有者、LocalSystem DPAPI/SQLCipher/HTTPS 登录、SCM 停启重启及卸载通过，最终无 DropTest 服务。 |
| 生产候选包 | x64/Win32 完整 PASS | production-x64-808ae4f0b00417ffb521fb92baf71fc8/results.json；production-Win32-84535d990ef3c1810f5861a1b4f141cf/results.json：测试密码/入口排除、真正遮蔽控制台、包内原生 UI 中文收发、30 秒空闲及连续 500 条消息。真实 ZIP 每个文件/大小/SHA-256、许可和秘密排除验证记录为 build/tests/packages-*.json。 |
| Google Drive | Debug/Release 实现和模拟 PASS | drive-Release-2c0f9bfd51164393b52d6777e5610a8c/results.json、drive-Debug-d815f8f1315849dc9e6539b976b98dc6/results.json：真实 C 后端、TLS 模拟、DPAPI、授权/配额/429、实际进程终止、会话过期/丢最终响应、56 条两页目录、路径穿越拒绝、精确密文、下载篡改拒绝、认证恢复及真正托盘接收、后台云盘失败仍接收消息。真实 Google 授权按用户要求稍后。 |

## 发布候选与资源

当前归档：`build/packages/Drop-0.1.0-x64-20261010-195926.zip`、`build/packages/Drop-0.1.0-Win32-20261010-195916.zip`。各含文件清单 files.json，有外部 .sha256；同步最终文档时重算两层校验值。保留此前候选和失败记录。生产程序包含 schema 9 云盘后端、服务权限修正和大文件后台校验，诊断密码、测试入口/环境钩子在编译时排除。

以下为实际生产包登录后各 30.047 秒的采样。CPU 百分比相对一个逻辑核；内存是该时间窗内采样最大工作集，MiB=1048576 字节。

| 架构/场景 | 服务 CPU | 客户端 CPU | 服务 MiB | 客户端 MiB |
| --- | ---: | ---: | ---: | ---: |
| x64 空闲 | 0.052% | 0.260% | 11.10 | 25.17 |
| x64 连续消息 | 4.264% | 31.721% | 13.66 | 30.92 |
| Win32 空闲 | 0.052% | 0.208% | 10.93 | 22.54 |
| Win32 连续消息 | 4.784% | 31.877% | 13.28 | 27.52 |

各提交 500 条真实 HTTPS 消息，x64 20.766 秒、Win32 20.844 秒，提交错误均为空；真正托盘客户端随后保存全部消息。测试运行于共享开发机，大文件演练可能同时运行，不是隔离硬件基准或 24 小时稳定性证明。当前生产窗口实际截图及视觉核对：`build/tests/visual-production-d8f00649966b4bdcb350dc57bde51340/production-window.png`（x64）、`build/tests/visual-production-43db862acef54674ae2df161176f4479/production-window.png`（Win32）。截图脚本在捕获前同步重绘并拒绝空黑客户区，修正的是初次隐藏窗口捕获时序。

4 GiB 演练重用首次失败已上传的分块，Debug 续作的 client-send + client-sync 耗时 926.828 秒；不包含首次全部上传/稳定副本准备，也不包含后面的备份和恢复。过程包含原生完整文件验证及下载，不能将这个时间直接当成首次网络上传吞吐。每次客户端启动会重新 SHA-256 校验已完成文件，大规模历史的启动成本仍是优化范围。全部 64 位长度、分块认证、整文件摘要与恢复验证实际通过。

尚未人工验证：不同物理 Windows 主机、普通另一用户读取权限演练、真实睡眠/锁屏/Windows 重登录、网络适配器实际切换、Windows 通知实际展示、多显示器 DPI、24 小时以上稳定性、真实 WAN/Google 授权限流。实际新目录恢复不依赖原机 DPAPI，但不能代替另一主机演练。当前密钥版本 1，无换钥历史，未实现轮换。公开 MIT 作者/年份占位符待所有者确认。

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
