# Google Drive 密文备份

实现已完成，模拟 HTTPS 故障及完整恢复已验收；根据用户本轮选择，真实 Google 授权和真实 Drive 演练稍后处理，不能将模拟 PASS 称为真实云盘成功。

## 授权与秘密

在 Google Cloud 项目启用 Drive API，创建桌面应用 OAuth client，配置所需测试用户/发布状态，下载 installed 类型的 JSON。停止服务器，在本机执行：

```powershell
.\dropserver.exe drive-authorize --data C:\DropData\server --oauth-client C:\Private\desktop-oauth.json
```

系统浏览器打开 Google 授权，程序仅在随机 127.0.0.1 端口等待回调，校验高熵 state 并使用 PKCE S256，十分钟过期。只申请 drive.appdata 权限。客户端无需 Google 登录。配置中的 client_id、client_secret 和返回 refresh_token 复制到服务器 DPAPI 文件，访问 token 只保留在进程内，不在参数/日志输出。原 Google 下载文件由所有者自行保护；不要把授权码、密码或令牌发送到聊天或粘贴进普通配置。

前台使用 drive-user.dpapi；已有机器运行密钥的服务器使用 drive-machine.dpapi。runtime-machine 同时转换已有云盘授权，移除用户包并限制整个目录。更换主机需重新授权，备份和独立恢复包不包含 Google refresh token。Google 的桌面 client secret 不能提供机密客户端的保证；安全依赖系统浏览器、state、PKCE、DPAPI 和目录访问权限。

## 上传和进度

```powershell
.\dropserver.exe drive-upload --data C:\DropData\server --backup C:\DropData\backups\BACKUP_ID
.\dropserver.exe drive-status --data C:\DropData\server
.\dropserver.exe drive-list --data C:\DropData\server
```

命令中的 BACKUP_ID 使用实际 32 位随机编号。只接收带有效认证 complete.bin 的本服务器完整备份；逐文件核实本机密文 SHA-256。上传对象包括 SQLCipher 快照、附件认证头及密文块，最后上传加密 complete.bin；不上传原文件名、明文正文、证书、运行密钥或恢复密码。Drive 名称为内部 ID，appProperties 保存内部备份/对象路径、密文 SHA-256 和完成标记；Drive 可观察对象大小、内部编号和时间。

存储在 appDataFolder，普通 Drive UI 无法浏览，需本工具列表/下载；授权范围仅限本应用。预分配文件 ID、1 MiB 可恢复上传（符合 256 KiB 倍数）、Location 会话、进度和失败状态写入 SQLCipher schema 9。重启先查询 Range，依据服务器已接受位置续传；会话过期重新建立，同一个 ID 避免最终响应丢失产生重复文件。完成后检查 Drive SHA-256；若未提供该可选字段，分块读回密文进行完整 SHA-256 验证。

限流和 5xx 保留重试，配额不足显示 space_unavailable，授权失效显示 unauthorized。drive-status 显示完整/总文件数、任务状态及完成时间，不输出会话 URL 或 token。状态和手动管理命令在服务器停止时执行以遵守独占根锁；运行中可从服务器云盘日志的任务编号/错误及备份文件检查结果。损坏的密文或冲突对象拒绝，不通过覆盖已有备份消除错误；需修复来源或生成新的备份任务。

已授权且 serve 配置 --backup-directory 时，独立后台云盘线程每分钟检查完整本地备份并镜像；本地手动/定时备份仍由备份线程执行。上传失败不回滚消息，也不删除主存储。备份线程先完成本地清单，云盘线程再上传；服务器已接收、完整本地备份和云盘完成分开记录。服务停止会取消云盘网络等待并等待线程释放。

## 下载、恢复和明确删除

```powershell
.\dropserver.exe drive-download --data C:\DropData\server --backup-id BACKUP_ID --target C:\DropData\downloaded
.\dropserver.exe restore --data C:\DropData\restored --backup C:\DropData\downloaded --package C:\Offline\recovery.bin
.\dropserver.exe drive-delete --data C:\DropData\server --backup-id BACKUP_ID
```

下载通过内部固定路径白名单，拒绝路径穿越和重复条目，写 .part 并按已保存偏移续传；每文件 SHA-256 验证，最后保存完整标记。失败保留临时文件和 download.incomplete，不能当成完成。下载只是传输校验；restore 使用独立恢复密码验证清单、SQLCipher、每块 AEAD、原文及有序密文摘要，成功后才创建可运行的密钥包。坏文件、缺块、截断或错误密码不启动服务器。

灾难恢复时可先 init 一个独立管理目录，在新主机用同一 OAuth 应用重新授权，下载旧备份，再用旧恢复包恢复到另一个全新目录；原机 DPAPI 不必可用。恢复包和恢复密码单独保管。恢复保留服务器 ID、更新同步代次，较新客户端副本及不确定队列按 BACKUP_RESTORE.md 核对。云盘授权不是 Drop 账号登录。

drive-delete 是明确的永久远端删除，先删除完整标记再删除该备份文件，持久标为 deleted 避免后台重新上传。中断可重试，不删除本地备份或历史。程序不自动清理云端历史；不执行真实远端删除验收，除非部署者明确指定隔离测试备份。

## 验收与限制

运行 `python tests/drive.py --configuration Release`（也可 Debug）。最终 Release 完整 PASS 为 `build/tests/drive-Release-2c0f9bfd51164393b52d6777e5610a8c/results.json`。隔离私有 CA、模拟 Drive/OAuth 服务与真正 C 程序验证授权失效、403 配额、429、接受数据后实际终止上传进程、会话过期、最终响应丢失、56 条两页目录、路径穿越拒绝、精确密文一致性、下载篡改拒绝、全新目录恢复、原生托盘完整接收及后台配额失败时消息仍提交。真实浏览器 OAuth 回调、Google 账号/App 发布策略、真实 WAN 断网及真实 Drive 限流尚未验证。

最终 Debug 同一完整模拟也全部通过：`build/tests/drive-Debug-d815f8f1315849dc9e6539b976b98dc6/results.json`。

依据官方 [可恢复上传](https://developers.google.com/workspace/drive/api/guides/manage-uploads)、[预分配 ID](https://developers.google.com/workspace/drive/api/reference/rest/v3/files/generateIds)、[桌面 OAuth](https://developers.google.com/identity/protocols/oauth2/native-app)、[appDataFolder](https://developers.google.com/workspace/drive/api/guides/appdata) 和 [文件校验值](https://developers.google.com/workspace/drive/api/reference/rest/v3/files)。
