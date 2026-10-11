# Google Drive 密文备份

当前默认目录为 EXE 旁 Data\DropServer。下面在旧 AppData 目录进行的真实授权属于历史验收，未自动迁移到新目录；新部署需指定实际目录重新授权，Google 连接继续使用 HTTPS，与 Drop 的 HTTP/PAKE 模式独立。

2026-10-10 已完成真实 Google 浏览器授权、密文备份上传、下载和全新目录恢复验收；Debug/Release 模拟故障验收也已通过。真实演练使用此前 0.1.0 x64 生产候选程序，授权和全部测试数据位于 build/tests 下的隔离目录。1.0.0 保留同一 OAuth/Drive 实现，仅更新程序图标和产品版本标识；真实成功、模拟故障和长期部署结果分别记录。

## 授权与秘密

在 Google Cloud 项目启用 Drive API，创建桌面应用 OAuth client，配置所需测试用户/发布状态，下载 installed 类型的 JSON。停止服务器，在本机执行：

```powershell
.\dropserver.exe drive-authorize --data E:\DropData\server --oauth-client C:\Private\desktop-oauth.json
```

系统浏览器打开 Google 授权，程序仅在随机 127.0.0.1 端口等待回调，校验高熵 state 并使用 PKCE S256，十分钟过期。只申请 drive.appdata 权限。客户端无需 Google 登录。配置中的 client_id、client_secret 和返回 refresh_token 复制到服务器 DPAPI 文件，访问 token 只保留在进程内，不在参数/日志输出。原 Google 下载文件由所有者自行保护；不要把授权码、密码或令牌发送到聊天或粘贴进普通配置。

若 Google 显示“尚未完成验证流程”“此应用正在测试中，仅供已获开发者批准的测试人员使用”，在创建该 OAuth 配置的项目中进入 [Google Auth Platform → 受众](https://console.cloud.google.com/auth/audience)，在“测试用户”添加实际授权时选择的 Google 邮箱并保存，然后重新执行授权。即使账号是项目所有者，也应加入测试名单；个人演练可以保持测试状态。[Google 官方配置步骤](https://developers.google.com/workspace/guides/configure-oauth-consent)。现有授权等待超过十分钟时必须重启本机授权命令，以获得新的回调、state 和 PKCE。

外部应用处于 Testing 状态时，Google 签发的 refresh token 在七天后过期；仅请求基础身份资料的例外不适用于本程序的 drive.appdata。出现 unauthorized 时停止服务器并重新执行浏览器授权，已有本地消息及密文备份保留。长期部署需由项目所有者决定 OAuth 发布状态，本次未更改 Google 项目设置。[Google 刷新令牌有效期说明](https://developers.google.com/identity/protocols/oauth2#expiration)。

正式发布状态和通过 Google 应用验证是不同事项。本程序的 drive.appdata 属于非敏感权限；是否需要品牌验证等手续以 Google 控制台及目标使用范围为准。切换正式发布状态后，为实际服务器数据目录重新授权并复验备份；测试目录的授权和恢复包不能替代实际服务器的配置，正式令牌仍可能因撤销或账户策略失效。[Drive 权限分类](https://developers.google.com/workspace/drive/api/guides/api-specific-auth)、[OAuth 应用状态](https://developers.google.com/identity/protocols/oauth2/production-readiness/overview)。

### 1.0.1 长期部署：发布状态与重新授权

2026-10-11 项目所有者反馈控制台“显示正式版了”，据此记录 drop-511213 发布状态为 In production，证据来源是所有者反馈，不是本机对控制台的独立读取，也不代表 Google 品牌审核已通过。记录为 build/tests/google-production-publishing-20261011.json。所有者随后选择由程序提供默认数据目录，并在部署时自行修改。旧版默认目录 C:\Users\JLYis\AppData\Local\DropServer 曾用 1.0.0 x64 生产程序完成全新浏览器授权、刷新、真实上传和下载逐文件 SHA-256 校验，未复制隔离测试凭据；证据 build/tests/google-production-default-20261011.json。本机配置文件的项目标识为 drop-511213，不含程序版本信息，修改 Drop 的版本或图标不会修改 Google 控制台设置。

1. 项目所有者已确认 [该项目的 Google Auth Platform → 受众](https://console.cloud.google.com/auth/audience?project=drop-511213) 显示“正式版 / In production”。继续使用原 OAuth 应用：appDataFolder 按应用隔离，旧备份属于原应用。
2. 在“数据访问权限”核对只申请 https://www.googleapis.com/auth/drive.appdata；本次不扩大 Drive 权限。Google 品牌验证按控制台另行提示处理，不能将点击发布称为已通过 Google 验证。
3. 使用 1.0.1 包，停止实际服务器，在本机控制台执行下面的命令。未指定 --data 时使用程序旁 Data\DropServer（2026-10-11 更新）；默认目录已经初始化时 init 保留原身份。部署若选择其他目录，每条命令均添加同一 `--data "D:\DropData\server"`。保留原 OAuth client 配置，配置路径由部署者提供。程序会用 prompt=consent 再次请求离线授权，旧测试令牌不作为长期部署凭据。

```powershell
.\dropserver.exe --version
.\dropserver.exe data-directory
.\dropserver.exe init
.\dropserver.exe drive-authorize --oauth-client "E:\work\client_secret_213182483712-aj6q3sg53uvbj2th7fg9iutenj1bbg74.apps.googleusercontent.com.json"
.\dropserver.exe drive-list
```

4. 创建实际加密备份，上传并检查 drive-status，下载到新目录，再用自行保管的独立恢复包演练恢复。随后启动配置了 --backup-directory 和 --backup-seconds 的服务器，核对本地备份完成、云盘任务完成和客户端持续同步。Windows 服务部署遵循 DEPLOYMENT.md 的 runtime-machine/管理员步骤。
5. 记录连续运行超过 24 小时以及授权超过七天后的同步、令牌刷新和自动备份结果。正式发布可避开 Testing 的七天规则，但撤销授权、账号或组织策略、长时间未使用等仍可能导致刷新失败；以实际任务结果判断，不把“已发布”当成永久授权。

本节是长期部署操作说明，完成状态及现场观察写入 VERIFICATION.md 的现场记录。发布状态已有所有者确认；本机默认目录的重新授权和真实空存储基线上传/下载已通过。基线包含 2 个密文文件共 204976 字节，云端保留 ea42af015c35ecc44e524ee8d8974dbb，原隔离备份仍在，未删除远端数据。实际部署账号、TLS、自行保管的独立恢复包及该目录的新目录恢复、自动镜像和长期运行尚未验收，不能由空基线或发布状态推断为通过。

本轮用户尝试发布时报告控制台要求网站首页和隐私说明。2026-10-11 已确认 drop.jsnowly.win 域名与 HTTPS 为 active，首页 https://drop.jsnowly.win/、隐私说明 https://drop.jsnowly.win/privacy.html 和服务条款 https://drop.jsnowly.win/terms.html 均公开返回 HTTP 200。在 [品牌信息](https://console.cloud.google.com/auth/branding?project=drop-511213) 使用三个对应网址、授权域名 jsnowly.win，以及所有者自己的支持邮箱；如果 Google 后续要求域名所有权，用项目所有者账号在 Search Console 验证 jsnowly.win，添加 Google 实际提供的 TXT。托管 TXT 不能替代 Google 验证。所有者填写品牌资料并取得服务条款地址后，已反馈控制台显示正式版；品牌审核或域名所有权验证是否完成仍无独立证据。具体网站证据见 WEBSITE.md；本机默认目录已重新授权，部署者仍可选择其他目录。

前台使用 drive-user.dpapi；已有机器运行密钥的服务器使用 drive-machine.dpapi。runtime-machine 同时转换已有云盘授权，移除用户包并限制整个目录。更换主机需重新授权，备份和独立恢复包不包含 Google refresh token。Google 的桌面 client secret 不能提供机密客户端的保证；安全依赖系统浏览器、state、PKCE、DPAPI 和目录访问权限。

## 上传和进度

```powershell
.\dropserver.exe drive-upload --data E:\DropData\server --backup E:\DropData\backups\BACKUP_ID
.\dropserver.exe drive-status --data E:\DropData\server
.\dropserver.exe drive-list --data E:\DropData\server
```

命令中的 BACKUP_ID 使用实际 32 位随机编号。只接收带有效认证 complete.bin 的本服务器完整备份；逐文件核实本机密文 SHA-256。上传对象包括 SQLCipher 快照、附件认证头及密文块，最后上传加密 complete.bin；不上传原文件名、明文正文、证书、运行密钥或恢复密码。Drive 名称为内部 ID，appProperties 保存内部备份/对象路径、密文 SHA-256 和完成标记；Drive 可观察对象大小、内部编号和时间。

存储在 appDataFolder，普通 Drive UI 无法浏览，需本工具列表/下载；授权范围仅限本应用。预分配文件 ID、1 MiB 可恢复上传（符合 256 KiB 倍数）、Location 会话、进度和失败状态写入 SQLCipher schema 9。重启先查询 Range，依据服务器已接受位置续传；会话过期重新建立，同一个 ID 避免最终响应丢失产生重复文件。完成后检查 Drive SHA-256；若未提供该可选字段，分块读回密文进行完整 SHA-256 验证。

限流和 5xx 保留重试，配额不足显示 space_unavailable，授权失效显示 unauthorized。drive-status 显示完整/总文件数、任务状态及完成时间，不输出会话 URL 或 token。新版服务器允许已授权后的 drive-status/list/upload/download/delete 在线执行，由本机管理管道交给原实例操作；首次授权及服务身份转换仍按部署流程停止服务。运行中也可从云盘日志的任务编号/错误及备份文件检查结果。损坏的密文或冲突对象拒绝，不通过覆盖已有备份消除错误；需修复来源或生成新的备份任务。

已授权且 serve 配置 --backup-directory 时，独立后台云盘线程每分钟检查完整本地备份并镜像；本地手动/定时备份仍由备份线程执行。上传失败不回滚消息，也不删除主存储。备份线程先完成本地清单，云盘线程再上传；服务器已接收、完整本地备份和云盘完成分开记录。服务停止会取消云盘网络等待并等待线程释放。

## 下载、恢复和明确删除

```powershell
.\dropserver.exe drive-download --data E:\DropData\server --backup-id BACKUP_ID --target E:\DropData\downloaded
.\dropserver.exe restore --data E:\DropData\restored --backup E:\DropData\downloaded --package E:\Offline\recovery.bin
.\dropserver.exe drive-delete --data E:\DropData\server --backup-id BACKUP_ID
```

下载通过内部固定路径白名单，拒绝路径穿越和重复条目，写 .part 并按已保存偏移续传；每文件 SHA-256 验证，最后保存完整标记。失败保留临时文件和 download.incomplete，不能当成完成。下载只是传输校验；restore 使用独立恢复密码验证清单、SQLCipher、每块 AEAD、原文及有序密文摘要，成功后才创建可运行的密钥包。坏文件、缺块、截断或错误密码不启动服务器。

灾难恢复时可先 init 一个独立管理目录，在新主机用同一 OAuth 应用重新授权，下载旧备份，再用旧恢复包恢复到另一个全新目录；原机 DPAPI 不必可用。恢复包和恢复密码单独保管。恢复保留服务器 ID、更新同步代次，较新客户端副本及不确定队列按 BACKUP_RESTORE.md 核对。云盘授权不是 Drop 账号登录。

drive-delete 是明确的永久远端删除，先删除完整标记再删除该备份文件，持久标为 deleted 避免后台重新上传。中断可重试，不删除本地备份或历史。程序不自动清理云端历史；不执行真实远端删除验收，除非部署者明确指定隔离测试备份。

## 验收与限制

运行 `python tests/drive.py --configuration Release`（也可 Debug）。最终 Release 完整 PASS 为 `build/tests/drive-Release-2c0f9bfd51164393b52d6777e5610a8c/results.json`。隔离私有 CA、模拟 Drive/OAuth 服务与真正 C 程序验证授权失效、403 配额、429、接受数据后实际终止上传进程、会话过期、最终响应丢失、56 条两页目录、路径穿越拒绝、精确密文一致性、下载篡改拒绝、全新目录恢复、原生托盘完整接收及后台配额失败时消息仍提交。上述故障结果来自模拟服务；真实 WAN 断网、Google 配额耗尽/限流、授权到期及正式应用发布状态仍未演练。

最终 Debug 同一完整模拟也全部通过：`build/tests/drive-Debug-d815f8f1315849dc9e6539b976b98dc6/results.json`。

真实完整 PASS：`build/tests/google-real-3daea6187076497fada56f20e3d6e93a/results.json`，共 10 项检查。实际系统浏览器的 loopback/state/PKCE 授权在 `google-real-4c49237fcc3743d5bf3597b420454f84` 成功，再在同一 Windows 用户的全新隔离服务器目录复用其 DPAPI 授权；真实刷新和 Drive API 列表访问成功。生产托盘客户端发送中文/emoji 正文和 3145745 字节随机附件；生成并上传 7 个文件、3351381 字节的加密备份，上传约 33.359 秒、下载约 22.907 秒。上传检查远端校验值并最后发布 complete.bin；下载全部密文的路径、长度和 SHA-256 与原备份一致。使用独立恢复包和内存中的恢复密码在新目录恢复，无需原服务器 DPAPI；恢复目录不含 Google 凭据。真实 HTTPS 核对原正文，新托盘客户端自动完整接收附件并核实 SHA-256。时间仅对应本次小型演练，不作吞吐基准。

云端保留隔离备份 `87008bdff9d0dcc23cf05a850c360b69`，原有备份集合未改变，未执行远端删除。文件位于 appDataFolder，普通 Google Drive 页面不显示，无需先创建可见目录。截图为该目录的 `original-native-client.png` 和 `restored-native-client.png`。OAuth 配置、令牌、运行密钥、TLS 私钥、恢复包及恢复密码均未上传或进入发布包；演练密码只在内存中，未保存，正式使用必须为实际服务器单独设置账号和自行保管的恢复包/密码。

失败记录保留：首次 Google 测试名单拦截见 `google-real-4c49237fcc3743d5bf3597b420454f84/browser-access-denied.json` 与 `authorization-attempt-1.json`；用户添加测试账号后重新授权成功。首轮验收脚本把附件按钮 108 写成 111，导致文件对话框未打开，尚未上传数据；修正脚本并换全新隔离目录后完整通过。`tests/drive_real.py` 针对尚未使用且已经授权的隔离目录执行真实演练，不直接重复运行完成或失败目录；其状态文件须标为 authorized_ready_for_cloud_verification。对已有真实授权文件作复制仅适用于同一 Windows 用户，跨主机恢复仍需重新授权。

依据官方 [可恢复上传](https://developers.google.com/workspace/drive/api/guides/manage-uploads)、[预分配 ID](https://developers.google.com/workspace/drive/api/reference/rest/v3/files/generateIds)、[桌面 OAuth](https://developers.google.com/identity/protocols/oauth2/native-app)、[appDataFolder](https://developers.google.com/workspace/drive/api/guides/appdata) 和 [文件校验值](https://developers.google.com/workspace/drive/api/reference/rest/v3/files)。
