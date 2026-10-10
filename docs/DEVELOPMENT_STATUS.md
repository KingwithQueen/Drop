# 开发状态

更新日期：2026-10-10。产品范围以 ../DEVELOPMENT_PLAN.md 为准。

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
| 八：Google Drive | 实现及模拟验收完成；真实验收待后续 | 原生 DPAPI、持久续传、故障重试、云端密文下载/新目录恢复/托盘接收及后台失败隔离通过。用户明确本轮先完成实现/模拟，真实授权稍后。 |

所有测试使用 build 下隔离目录。实际安装了随机 DropTest 隔离服务，验收后卸载；未改变防火墙及系统证书信任。临时系统信任测试因 Windows 确认阻塞已取消，核查未新增测试根证书；改用应用内明确 CA 文件。

## 当前可运行入口

`scripts/Build.ps1 -Platform x64 -Configuration Debug -Test` 构建原解决方案并运行公共、客户端和加密服务器自测；Release 同样可执行。`scripts/Verify-Integration.ps1` 启动隔离 HTTPS 服务器，逐项结果保存于 `build/tests/integration-*/results.json`。

`drop.exe` 无参数启动原生托盘界面，提供服务器/账号设置、连接测试、正文和多附件发送、历史查询、未读及提醒、自动完整下载和显式导出；关闭窗口后保留托盘。原有诊断入口保留。`dropserver.exe` 提供加密初始化、本机遮蔽密码输入的账号创建和前台 HTTPS/WSS。本地在线备份、恢复包、账号管理和服务入口已接入，实际服务身份及生命周期已验收。

## 故障与修复

- Tcl 9.0.3 Windows port 删除 bundled SQLite 源码后仍构建不需要的扩展，产生 C1083；项目 overlay 排除该扩展，Tclsh / SQLCipher 重建通过。
- CivetWeb 1.16 Windows 主动关闭 HTTPS 短连接时丢失响应，100 次请求中出现 14–23 次失败；进程仍运行且写入返回成功。项目 overlay 在 TCP 关闭前执行最多 1 秒等待的完整 TLS 关闭，同时允许连接复用；100 次客户端主动关闭请求不重试全部通过。C++ wrapper 和未使用的 WebSocket 压缩关闭。
- 受限构建环境中 MSBuild 节点复用阻塞；脚本限制两个节点并关闭复用，终止了本项目阻塞构建进程，没有结束用户会话。
- 过期证书测试已使用固定过去的 notBefore/notAfter，适配 OpenSSL 3.6，实际过期拒绝通过。

阶段零至七的本机实现与发布候选验收已完成。不同物理主机、实际休眠/锁屏、Windows 重登录、通知显示、多显示器 DPI 和 24 小时以上运行仍需现场演练；当前仅有密钥版本 1，未实现换钥。具体边界和证据见 VERIFICATION.md。

阶段五首轮完整 Debug 回归记录：`build/tests/integration-x64-Debug-中文-35efa21b4e0b4fb2a515087e9f50c4ca/results.json`。实际验证丢失正文确认后的幂等重试、跨设备账号已读、越权确认拒绝、完整附件确认到发送端、异步 WinHTTP/WSS 及原生 UI。前两轮续传网络错误保留失败目录，随后完整 Debug/Release 故障回归通过。真实 Windows 休眠及系统卷耗尽未演练。

阶段五完整 Debug：`build/tests/integration-x64-Debug-中文-5467ddc97f944760a12e2b2b09cde2fb/results.json`，包括实际 SQLite 页容量耗尽的同步整页回滚、解除限制后补齐。Release 回归发现验收脚本按目录枚举顺序选错旧迁移快照，已改为按写入时间选最新快照；失败记录保留，最终 Release 完整 PASS：build/tests/integration-x64-Release-中文-51f9cd3cfe5548e0a1285337deedfea2/results.json。另一次早期 Release 用例误要求分批同步尚未完成的附件立即导出，已改为等待该附件确实完整保存。

阶段六在线备份恢复完整 PASS：Debug `build/tests/backup-Debug-中文-8893042637be470db82f1e88c9bea9ec/results.json`，Release `build/tests/backup-Release-中文-13bedf8514f946d0bd4631351ac08f34/results.json`。仅使用独立恢复包和加密备份在新目录恢复，不使用原机 DPAPI 包。不同物理 Windows 主机仍未演练；真实服务身份与生命周期另有下方管理员验收证据。

阶段七故障与修复：4 GiB + 17 字节全部上传后，完整校验超过 HTTP 超时。增加 schema 8、两条后台校验线程和可查询 validating/finalize_error；校验前不发布消息。保留目录重用分块的 Debug 复验最终完整 PASS：`build/tests/backup-Release-中文-324df4d89e5e4bb7a910b5533e03fbae/results.json`（目录名保留首次 Release 失败名称，结果记录实际 Debug 配置）。完成收发、备份实际进程中断及同任务续作、全新目录恢复、保留较新客户端副本/暂停不确定发送、明确重试及备份密文篡改拒绝；首次失败另存 initial-failure-results.json。

后台校验专项完整 PASS：`build/tests/finalize-Release-f8fad50026824d70847d10769ce24751/results.json`。真实 64 MiB 上传校验时仍可提交正文；强制终止后全部分块保留，重启重新验证；磁盘块篡改持久报告 crypto_error，使用原块明确修复后才能完成。自测脚本每次使用独立 UUID 目录；用于稳定触发中断的延时钩子仅在诊断构建存在。

阶段八最终 Release 模拟完整 PASS：`build/tests/drive-Release-2c0f9bfd51164393b52d6777e5610a8c/results.json`。服务器原生 C 后端使用独立 HTTPS 模拟服务；包含 56 条目录两页读取、路径穿越拒绝、密文续传/下载/认证恢复和真正托盘接收、后台配额失败仍提交正文，未访问真实 Google 账号。完整命令、授权和限制见 GOOGLE_DRIVE.md。

最终 Debug 重新构建与原生自测通过；阶段八 Debug 同一完整模拟也全部通过：`build/tests/drive-Debug-d815f8f1315849dc9e6539b976b98dc6/results.json`。服务器当前 schema 9、客户端 schema 4，x64 和 Win32 的依赖安装目录独立，切换架构不会移除另一架构依赖。

阶段七 Win32 Release 完整 PASS：`build/tests/integration-Win32-Release-中文-d4cdb17e23ef42769a4547cff723a069/results.json`。最终生产包原生验收完整 PASS：`build/tests/production-x64-808ae4f0b00417ffb521fb92baf71fc8/results.json`、`build/tests/production-Win32-84535d990ef3c1810f5861a1b4f141cf/results.json`。两架构分别测量登录后空闲 30 秒及连续 500 条消息时的 CPU/工作集；详细数值和采样边界见验收记录。

当前候选包：`build/packages/Drop-0.1.0-x64-20261010-195926.zip`、`build/packages/Drop-0.1.0-Win32-20261010-195916.zip`，各有同名 .sha256 与包内 files.json。包含云盘后端、后台大文件校验及服务修正；排除测试入口/口令、证书和运行数据。文档更新后由 `scripts/Refresh-PackageDocs.ps1` 同步重算清单与 ZIP 校验值，`tests/package.py` 验证真实归档的每个文件。未公开发布；原 MIT 作者/年份占位符须所有者在公开再分发前确认。

管理员服务验收实际揭示 ChangeServiceConfig2 设置自动重启所需 SERVICE_START 访问权缺失，已补齐；安装失败回滚临时服务。目录验收脚本混用旧 PowerShell 模块环境也已改为当前 pwsh。失败记录保留于 service-Debug-9c5cc4bc974a46e9860b4bba6ade34a4、service-Release-46b589938cb04c91a097d656993971a2、service-Release-a4e9d7f8341a4e568755e4bbcb9ad8ad。修正后实际管理员服务完整 PASS：build/tests/service-Release-c7d79aa9427046d69fe00828128beb2b/results.json，临时服务最终卸载。

阶段六最终服务结果：Windows 管理员确认后，整个现有目录树只保留 SYSTEM/Administrators 权限、Administrators 所有者；LocalSystem 服务实际解锁 DPAPI/SQLCipher 并登录，SCM 停止重启保留身份/凭据，随机服务最终卸载。首次故障均保留，最终结果不是编译推断。
