# 存储格式与迁移

当前格式：协议 1，服务器数据库 schema 12，客户端数据库 schema 5，运行密钥包 1，附件格式 1，恢复包和备份格式 1。产品版本为 1.0.4，沿用持久消息删除及独立删除同步，没有新增格式迁移。以下描述已实现格式。业务内部 UTF-8，Win32 文件路径与界面边界为 UTF-16。

服务器 schema 12 新增 message_deletions、object_deletions、deletion_sequences、deletion_events，升级前保存加密 schema-11 回退快照。发送账号校验、令牌再校验、删除标记、清空活动正文、删除附件引用和双方删除事件在同一事务完成；保留原消息身份及幂等指纹，附件退役标记阻止重新绑定。未引用密文按原有宽限期与备份引用规则回收，已有备份保留。

客户端 schema 5 新增 message_deletions、pending_deletes、cache_cleanup 及 meta.deletion_cursor，升级前保存完整 schema-4 本机回退快照。离线删除先持久保存意图并隐藏消息，服务器确认后事务应用删除标记、清空正文、取消下载及阅读任务并排队内部缓存回收。缓存只接受随机 ID 及已知后缀，提交后才删除，文件占用保留队列重试；原始文件和明确导出文件不参与回收。删除标记可先于正文到达，整页校验失败则回滚且不推进游标。代次改变时重拉独立删除序列，保留删除意图及已知标记。这是逻辑删除与缓存回收，不宣称擦除历史备份或数据库介质中的旧字节。

本次 1.0.1 阅读响应优化不改变存储格式。中央 read_positions 按账号和会话存储权威已读位置，客户端 account_reads 缓存同步状态，pending_reads 保留断网待提交意图，ui_messages 记录本设备未读及通知状态。正文/附件保存与阅读独立：文件内容在本机 files 目录作为 .part/.file 存储，downloads 只记录元数据和进度；完成校验后显示「已缓存到本机」，另存为从该缓存明确导出。

1.0.1 的原生用户列表使用可重建的 ui_contacts(user_id,login) 登录名缓存，并增加 ui_profiles(user_id,login,display_name,enabled,deleted) 资料缓存。缓存位于当前账号自己的客户端数据库，按需建立，不保存密码或令牌，不提升消息 schema。通过认证接口刷新自己、已有参与者和精确查找过的联系人；资料分页独立于正文游标，不能据此跳过消息。

schema 11 在 accounts 增加 deleted_at（默认 0）和 former_login（默认空）。升级前保存加密 schema-10 回退快照；删除在一个事务中撤销设备、移除 account_pake、清除 password_hash，将登录名替换为正常登录规则禁止的内部占位名，同时记录原登录名。账户行及 user_id 留存，保持消息、会话和附件所有权等外键。former_login 仅用于验证此前已知身份的资料请求，不作为登录名或响应字段。同名新账号获得新 user_id。改显示名与删除均不改变正文、附件密文和存储密钥。

在线管理读取受保护的运行密钥包进行认证，数据库和目录独占锁仍由原服务器拥有；仅在线执行器在原数据库连接及相应锁下修改，不允许两个进程直接写库。

服务器目录受当前运行用户、SYSTEM、Administrators 的受保护 DACL 限制。迁移为 runtime-machine.dpapi 后仅允许 SYSTEM 和 Administrators；机器 DPAPI 保护与 DACL 必须同时存在。实际 LocalSystem 服务身份解锁与整个目录树权限已通过管理员 SCM 验收。`run.lock` 为独占句柄；数据库、密钥和管理命令只允许一个进程拥有目录。`runtime-user.dpapi` 加密保存固定 `DROPKEY1` magic、32 字节独立随机数据库材料、32 字节独立随机附件主密钥、server_id 和 generation。秘密不进入配置、命令行和日志。二进制字段均为字节数组，无平台对齐差异；两 ID 各含结束零，总大小 138 字节。

`server.db` 使用 SQLCipher 4 默认页格式。`sqlite3_open_v2` 之后立即 `sqlite3_key`，在任何 SQL、schema 写入及 journal 创建前配置密钥；确认 cipher_version 非空，正确解锁才设置 WAL。启用 temp_store=MEMORY、cipher_memory_security、foreign_keys、trusted_schema=OFF、secure_delete、synchronous=FULL。源库支持 SQLITE_HAS_CODEC 和 SQLITE_TEMP_STORE=2；不启用 plaintext header。无法解锁时直接失败，不改用明文、不重新生成密钥、不覆盖旧数据库。

schema 1：`meta(key,value)`、`accounts(user_id,login,display_name,password_hash,enabled,quota_bytes,failed_logins,locked_until)`、`devices(user_id,device_id,name,token_digest,expires_at,revoked,last_seen)`。账号 ID 固定；设备主键为 `(user_id,device_id)`。密码是 libsodium Argon2id 字符串（当前 interactive 参数：2 次迭代、64 MiB）；设备 token 仅存摘要。时间为 Unix 秒。SQLite INTEGER 为带符号 64 位，未来文件长度须额外限制到 INT64_MAX 以内，不把协议 UINT64_MAX 直接强转入库。

schema 2 增加 conversations、messages、account_sequences 和 events。会话由规范化双方 ID 唯一确定，自发 lo=hi；消息按发送账号/请求 ID 唯一，SHA-256 指纹绑定收件人和正文。消息和所有参与账号连续事件在同一 FULL 同步事务提交。正文 BLOB 保留实际压缩标记及原始长度，阶段五已接入独立 none/zstd 配置，修改仅影响新记录。

schema 3 增加 objects、chunks、attachments；上传所有权/请求 ID 唯一，块索引唯一，附件对象只允许引用一次。objects 保存原名、长度、原文 SHA-256、加密头部及 uploading/complete 状态；chunks 的原文块摘要位于 SQLCipher 内，attachments 保留顺序。新消息指纹绑定收件人、正文长度/内容与有序附件 ID。已有 schema-2 无附件文本仍可按原指纹幂等重试，修改正文仍冲突。

空库先建立加密 schema 1，再逐级升级到 11。每次升级前，在独占启动锁下检查 WAL checkpoint 成功，复制加密页到唯一 `migration-v<旧版本>.<随机 ID>.db`，刷新文件并用原密钥解锁及 integrity_check，通过后才执行升级事务。回退副本不覆盖旧快照；仍须保管相匹配的原用户 DPAPI 包。非空 schema-1 账号及 schema-2 消息迁移已实际验证；可移植恢复已通过隔离新目录原生验收。未知格式拒绝，密钥包与数据库身份/代次错配拒绝。

客户端 session.dpapi 保存服务器 URL、ID、账号/设备 ID、代次、token 及可选绝对 CA 文件路径，不保存密码。数据按服务器 ID/账号 ID 分目录；SQLite schema 1 为 meta、pending、messages，schema 2 增加 pending_files 与 downloads。发送附件先复制并刷新为随机 local_id.source，再事务记录正文/附件任务；上传成功保留 object_id.file。同步事务保存整页正文、下载任务及连续游标，错误页回滚；完成所有正文页后再下载。失败文件保留 .part、进度/错误，不妨碍其它下载任务。每块刷新后更新保存字节数，重启按实际 .part 长度向下取整到完整块续传；全文件长度和 SHA-256 通过后才原子发布为 .file 并记录 complete。默认内容可明文保存，DACL 隔离，令牌只有 DPAPI 文件持久副本。发现同一服务器的新代次时，事务清零正文和状态游标，保留本机正文、完整文件和通知标记；结果不确定的 queued 发送任务转为 restore_review，用户核对后明确重试。旧快照之外的未完成下载暂停，快照重放到对应消息后才恢复。

正文公共格式：实际算法 `none` 或 `zstd`，格式版本 1，原始 UTF-8 长度，内容 BLOB。配置 `none` 独立于整数压缩级别，zstd 1–19；压缩未缩小则存原文。解压上限 1 MiB，限制帧长度及声明长度、只允许一完整帧，解压后重新验证 UTF-8。每条记录实际算法独立于当前配置，修改配置只影响新数据。

客户端 schema 3 增加 ui_messages(message_id,unread,notified)。当前发送设备自己的记录不产生未读或重复提醒，其它设备/账号记录在本机持久记录通知抑制状态；查看正文清除本机未读。阶段五使用独立持久状态流同步账号已读和设备保存确认。ui.json 只保存服务器 URL、CA 路径、登录名、提醒、自启动及正文压缩开关/级别，不保存密码或 token。后台独占 SQLite 和文件，界面只收到不含凭据的历史/状态快照。

附件格式 1：所有整数固定小端；对象 ID 为 32 字节 ASCII 小写十六进制，不含结束零。`manifest.bin` 固定 260 字节：

| 部分 | 版本 1 约束 |
| --- | --- |
| 偏移 0–67 | magic DROPOBJ1（8）、version uint32（1）、object_id（32）、total uint64、chunk_size uint32（1048576）、chunk_count uint64、key_version uint32（1）。 |
| 偏移 68–139 | wrap nonce（24）、32 字节独立随机对象密钥的 XChaCha20-Poly1305 密文/tag（48）；附件主密钥保护，AAD 为前 68 字节。 |
| 偏移 140–259 | 清单 nonce（24）、80 字节清单的密文/tag（96）；对象密钥保护，AAD 为前 140 字节。清单含原文 SHA-256（32）、有序密文块摘要（32）、总长 uint64、总块数 uint64。 |

块文件位于 `objects/<object_id>/<16位小写十六进制索引>.chunk`。固定 60 字节 AAD 为 magic DROPCHK1（8）、object_id（32）、index uint64、total uint64、该块明文长度 uint32；之后是随机 nonce（24）、明文长度字节的密文及 16 字节 tag，每块额外 100 字节。AEAD 绑定对象、顺序、总长和块长度；空对象无块，但存在认证清单。每次重新加密生成新 nonce；相同重试读取并核对既有块，不重复加密。不得复用 nonce。

创建上传时先写认证的初始清单，状态仍为 uploading；完成时逐块认证解密、校验完整原文 SHA-256，计算 `SHA256(SHA256(完整密文块0)||SHA256(完整密文块1)||...)`，用新 nonce 封装最终清单，刷新后更新数据库 complete。每次读取检查物理清单等于加密数据库头部、认证块及数据库内块原文摘要；完整密文聚合摘要在完成阶段计算，当前未在每次单块读取时重新扫描整个对象。未知版本、错误对象/索引/长度、认证失败和截断拒绝。

服务端临时上传和续传状态也只存密文；先刷新对象文件并原子发布，再事务提交消息引用。中间崩溃由恢复逻辑协调；不能只依赖文件写调用成功。原文件名只放在加密数据库内，内部随机 ID 不含路径。客户端 `.part` 核对明文总长与 SHA-256 后原子发布，再确认设备保存。配额不足不删除历史，也不假报完成。

测试与实际存储检查见 VERIFICATION.md。已提供附件对象及自动下载；设备保存确认已接入；七天宽限的未引用对象清理、恢复包及本地在线备份已实现，实际结果见阶段六验收记录。

SQLCipher 密钥设置时机、错误密钥读库检查及 cipher_integrity_check 的结果语义依据 [官方 API 文档](https://www.zetetic.net/sqlcipher/sqlcipher-api/)；加密后端和编译选项同时以固定版本 port 及实际自测核对。

服务器 schema 4 新增 saved_bodies、saved_objects、read_positions、state_sequences、state_events；schema 5 为 messages 增加显式 server_order 及唯一索引，避免依赖 SQLite 维护时可能变化的隐式行号。升级前分别留加密快照。客户端 schema 4 增加 message_orders、device_states、object_states、account_reads、pending_reads；downloads.saved_ack 持久保存确认进度。独立 state_cursor 和 saved_ack_cursor 放入 meta。升级前通过 SQLite 在线备份留完整本机回退副本并检查完整性；重放正文只补顺序元数据，保留正文、文件和通知状态。

客户端启动校验 complete 文件的总长与 SHA-256；丢失或损坏则恢复下载任务，不继续报告本机完整保存。损坏文件转为独立 .corrupt 随机副本保留，新 .part 校验后发布。启动校验会读取所有已完成附件；实际 4 GiB + 17 字节已通过，多次启动会重复全文件校验，Debug 大文件演练耗时不能直接当作网络吞吐，见 VERIFICATION.md。

账号逻辑配额统计参与消息的原始正文长度、每条消息 1024 字节元数据、拥有的上传对象长度加每对象 1024 字节，以及接收的另一账号附件。发送者和接收者均须满足配额；实际数据库/文件写入空间不足另行报告，不删除历史。

服务器 schema 6：objects.last_activity、object_cleanup 持久清理墓碑及 object_pins。活跃上传刷新活动时间；只清理超过七天且未被消息或备份引用的对象。先提交删除及墓碑，再删除已检查的随机编号目录中的已知密文文件；遇未知文件或重解析点拒绝清理。schema 7：backup_jobs 持久保存目标目录、任务编号、building/retry/complete、开始及完成时间。失败保留在线加密快照及对象预留，重试复用已验证的密文副本。

恢复包 DROPRK01 固定 202 字节：magic 8、Argon2id 盐 16、随机 XChaCha20 nonce 24、138 字节 DROPKEY1 的密文加 tag 154。固定 KDF 参数为 Argon2id 2 次、64 MiB，先校验格式和长度，不接受包内任意资源参数。AAD 绑定前 24 字节；恢复密码至少 16 UTF-8 字节。当前仅支持已实现的密钥版本 1，不宣称已实现主密钥轮换。

完整备份清单 complete.bin 固定 176 字节：DROPBAK1 8、nonce 24、128 字节清单密文及 tag 144。加密清单包含 DROPBM01、schema uint64、server_id 32、task_id 32、加密快照 SHA-256 32、完整对象个数 uint64、时间 uint64。快照目标开库立即配置 SQLCipher 密钥；完成后刷新并原子发布 snapshot.db，复制所有 complete 对象，包括尚未绑定消息的完整上传。引用、长度、原文摘要及认证头部在加密快照内；恢复逐块验证 AEAD、原文 SHA-256、密文聚合摘要及数据库完整性。上传中的对象不恢复为 complete，其本机稳定副本可重新创建上传。最后才发布清单。

## 大文件校验（schema 8）

objects 新增 finalize_error。超过 32 MiB 上传进入 validating，最多两条后台任务验证块 AEAD、长度、原文 SHA-256 和有序密文摘要；验证时不占用全局对象锁。完成请求返回当前状态，只有 complete 后才能提交消息。查询包括 finalize_error，失败不标完成，客户端保留副本。进程重启恢复 uploading/interrupted，原块保留。迁移前保存加密 schema-7 快照。

## 云盘队列（schema 9）

cloud_backups 保存备份 ID、本地完整根目录、queued/retry/complete/deleted、错误和完成时间；cloud_files 保存内部条目、密文长度/SHA-256、预分配 Drive ID、会话 URL、64 位接受偏移和状态，全在 SQLCipher 内。迁移前保存加密 schema-8 快照。云盘授权另用 DPAPI，备份不包含授权。恢复 schema 9 快照时清空旧云盘队列，避免使用旧环境会话和路径；支持 schema 7–9 备份，认证清单 schema 必须与实际快照匹配。
