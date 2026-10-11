# 加密备份与恢复

本地备份及新目录恢复已通过 x64 Debug/Release 原生验收。服务器真实 LocalSystem 生命周期、机器密钥解锁和目录权限已通过；Google Drive 已完成真实浏览器授权、密文上传/下载和新目录独立恢复，实际托盘完整接收原正文及附件。不同物理机器仍未验收。当前密钥版本为 1，未实现密钥轮换。证据见 VERIFICATION.md。

## 独立恢复包

以其运行身份在本机控制台执行；新版服务器可保持运行，LocalSystem 服务使用提升的管理员控制台：

```text
dropserver recovery-export --data DATA_DIRECTORY --target RECOVERY_PACKAGE_FILE
```

输入独立恢复密码，至少 16 UTF-8 字节，然后在 `Confirm password:` 再输入同一密码。两次均遮蔽输入；不一致时取消，不创建或覆盖恢复包。PowerShell 命令本身不保存或传递密码；程序拒绝重定向输入。恢复包由固定参数 Argon2id 和 XChaCha20-Poly1305 保护，包含数据库及附件主密钥和固定服务器身份。密码与包分开保管，不能写入命令行、日志、INI 或云盘。账号密码重置不改变存储密钥。原机 DPAPI 包不能替代可移植恢复包。

## 本地备份

在线或离线手动执行；运行时由原服务器创建快照，命令等待完整结果：

```text
dropserver backup --data DATA_DIRECTORY --target BACKUP_ROOT
```

在线服务器添加 `--backup-directory BACKUP_ROOT`。使用 `dropserver backup-trigger --data DATA_DIRECTORY` 发起后台备份；该命令只确认请求已送交，完整结果以服务器 `backup_complete task=编号` 和任务目录中的 `complete.bin` 为准。独立线程备份失败不撤销消息接收。

`--backup-seconds 3600` 每小时触发；允许 60–604800 秒，0 表示只手动。失败任务持久记录为 retry，在线一分钟后重试，重启后再次触发复用原加密快照和已验证副本。未完整发布的目录不能恢复；空间不足不会删除历史或成功备份。

BACKUP_ROOT/随机任务编号内只保存加密数据库一致性快照、附件密文及认证加密清单；不复制运行 DPAPI、TLS 私钥、日志或原文件名旁路清单。完成前预留对象，成功后解除预留；已被消息引用的附件不被宽限清理。当前快照创建期间持有数据库锁，巨大数据库的暂停成本尚待测量。

## 新目录恢复

将完整任务目录及恢复包复制到本机，执行：

```text
dropserver restore --data NEW_DIRECTORY --backup COMPLETE_BACKUP_DIRECTORY --package RECOVERY_PACKAGE_FILE
```

NEW_DIRECTORY 必须不存在。输入恢复密码；不需要原机 DPAPI。恢复检查快照密文摘要、SQLCipher 完整性、对象清单和块 AEAD、完整原文 SHA-256 及密文聚合摘要。失败目录保留 restore.incomplete，不发布可运行密钥；核查后使用另一个新目录重试，避免覆盖已有数据。

成功保留 server_id，生成新 generation，并在当前 Windows 用户下生成新的运行 DPAPI 包。重新部署 TLS 证书；服务运行需先 runtime-machine 迁移，再安装服务。备份只恢复到快照覆盖状态。

客户端发现新代次后清零游标重新核对，保留较新的本机正文和完整文件。快照之外的记录显示“保留本机副本”；已接受历史不自动重发。结果不确定的待发送任务显示“恢复后待核对”，核对正文和附件后通过界面明确重新提交，或执行诊断命令 client-retry-reviewed。旧快照中不存在的设备凭据需要重新登录，同账号本机数据继续保留。

## 验收与限制

真实客户端已验证新目录恢复正文、完整附件、代次核对和明确重试；实际修改备份块后拒绝恢复。不同 Windows 用户或另一台机器、各备份步骤的进程终止及实际定时触发还需扩大验证。

Google Drive 阶段八真实完整 PASS：`build/tests/google-real-3daea6187076497fada56f20e3d6e93a/results.json`。下载 7 个密文文件并逐个核对 SHA-256，使用独立恢复包及恢复密码在全新目录恢复；恢复目录未使用原运行 DPAPI，也不含 Google 凭据。实际 HTTPS 核对中文正文，新托盘完整接收 3145745 字节原附件并校验 SHA-256。云端保留这一份隔离演练备份，未执行远端删除。演练密码只保留在进程内；正式备份必须使用自行保管的恢复包和密码。

云盘完整操作与授权说明见 GOOGLE_DRIVE.md。云盘只接收带有效认证清单的完整本地备份，后台镜像失败不回滚消息；下载后仍须执行独立恢复包认证恢复。
