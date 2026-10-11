# 协议 v1

HTTP/HTTPS origin URL 接受域名、IPv4 与方括号 IPv6；拒绝用户名、密码、查询、fragment 及除根路径外的路径。客户端不跟随重定向，不忽略 HTTPS 证书错误，不自动降级。以下表格描述业务层：HTTPS/WSS 直接使用 TLS 内的 UTF-8 JSON、Bearer 和二进制附件，不叠加 PAKE；HTTP/WS 必须先通过 PAKE，再将业务正文、令牌及响应包在认证密文中，禁止原明文登录/令牌端点。HTTP 记录和加密 WS 的完整格式见 [PAKE.md](PAKE.md)。秘密不能进入 URL。

固定 ID 为 16 个随机字节的 32 位小写十六进制；令牌为 32 个随机字节的 64 位小写十六进制字符串。`server_id` 表示服务器身份，`generation` 表示同步代次。序号、长度、偏移、时间戳和配额等 64 位值一律使用规范十进制字符串（无符号、无空格、不允许多余前导零），严格检查溢出；版本和有明确小范围的枚举可用 JSON 整数。

上限：HTTP 头部 16 KiB，JSON 2 MiB，正文 1 MiB UTF-8，32 附件，16 层 JSON，单个对象/数组最多 256 成员、全体最多 4096 结构逗号，登录名 3–64 个 ASCII 英文字母（支持大小写）/数字/`_.-`，显示名及设备名称 128 UTF-8 字节，密码 12–1024 UTF-8 字节。账号名保留输入大小写并按大小写精确匹配；`Jsnowly` 与 `jsnowly` 是不同账号，现有小写账号不改名。重复键、NUL（包括 `\u0000`）、不合法 UTF-8、尾随数据拒绝。解析前检查嵌套与结构复杂度，避免恶意深度和重复键检查耗时失控。阶段一登录请求必须包含 Content-Length，暂不接受 chunked JSON。

| 接口 | 权限 | 当前状态 |
| --- | --- | --- |
| GET `/api/v1/server-info` | 无需登录 | 返回版本、固定服务器 ID、同步代次；已实现。 |
| POST `/api/v1/login` | 账号密码 | `login,password,device_id,device_name`；返回账号/设备/服务器 ID、代次及 token；已实现。 |
| GET `/api/v1/devices` | Bearer token | 只返回当前账号的设备 ID、名称及撤销状态，暂最多 100；已实现，分页待后续。 |
| POST `/api/v1/logout` | Bearer token | 撤销当前设备令牌；已实现。 |
| POST `/api/v1/accounts` | Bearer token | `login` 精确查询启用账号的 ID、登录名及显示名；已实现。 |
| POST `/api/v1/profiles` | Bearer token + 已知身份范围 | `users:[{user_id,login?}]`，最多 100 个；返回 profiles，字段为 user_id、login、display_name、enabled、deleted。自己、会话参与者或能精确提供该身份登录名的联系人可读取；无权限身份省略。 |
| POST `/api/v1/messages` | Bearer token | `request_id,recipient_user_id`（省略为自己）、body、attachments 对象 ID 数组；正文为空时必须有附件。附件必须由发送账号拥有、完整且尚未引用。返回 message_id 和 duplicate；已实现。 |
| POST `/api/v1/sync` | Bearer token | `generation,after,limit`；默认 50、最多 100，另限制响应大小。返回 server_id、generation、events、next、more；已实现。 |
| WSS `/api/v1/events` | Bearer token | 升级前和发送/心跳时认证；changed 提示变化；持久同步补齐；已实现，暂最多 16 连接。 |
| POST `/api/v1/uploads` | Bearer token + 所有权 | request_id、filename、total_size、sha256；创建幂等上传，返回对象信息；已实现。 |
| GET `/api/v1/uploads/<id>` | Bearer token + 所有权 | 返回 object_id、total_size、chunk_count（十进制字符串）、chunk_size（1048576）、sha256、state、received_bitmap；已实现。 |
| PUT `/api/v1/uploads/<id>/chunks/<index>` | Bearer token + 所有权 | application/octet-stream，必须与该块预期长度一致；重复相同明文幂等，不同内容冲突；已实现。 |
| POST `/api/v1/uploads/<id>/complete` | Bearer token + 所有权 | 验证全体认证块及完整原文 SHA-256，刷新认证清单后标记 complete；已实现。 |
| GET `/api/v1/attachments/<id>/chunks/<index>` | Bearer token + 消息参与者 | 已引用完整对象的认证明文块，application/octet-stream；已实现。当前采用按块续传，未提供 HTTP Range。 |
| POST `/api/v1/saved`、POST `/api/v1/read` | Bearer token + 对象权限 | 正文/附件按设备确认，已读按账号同步；阶段五草案。 |

本机管理命令支持在线和离线，详见 ACCOUNT_MANAGEMENT.md。在线管道仅允许运行用户、SYSTEM 和提升的管理员，拒绝远程连接；以独立用途标签从实例密钥派生 XChaCha20-Poly1305 密钥，帧上限 512 KiB，请求/响应绑定不同方向，每次连接以服务器新挑战防止重放。离线仍须取得目录独占锁，不开放网络管理端点。

登录对未知账号做同等 Argon2id 校验，限制每秒 4 次、最多 2 个密码校验并发；连续 5 次失败暂停 60 秒。设备 token 30 天到期，重新登录同设备使旧 token 失效，只保存摘要。未登录、撤销、到期、禁用及删除均拒绝。登录提交前重新核对密码哈希或 PAKE 材料版本，避免旧认证跨越在线重置事务签发令牌。

改名、启停或删除提交后通过现有 WebSocket changed 提示补拉资料，通知不含名称、正文或凭据；每批至多 100 个已知身份，消息游标独立。通知丢失由定时对账补齐。删除资料返回空 login、最后显示名、enabled=false、deleted=true；原登录名只在加密库内部用于验证此前精确查找过的身份。资料修改不改写历史消息身份和内容。

HTTP 错误 JSON：`{"error":"invalid_argument"}`。稳定错误名称由 `drop_error_name` 定义，HTTP 400 参数/格式、401 凭据、404 不支持、413 格式/文件超限、409 冲突、507 空间/账号配额不足、500 持久化失败。507 的稳定名称为 space_unavailable；任务和已有历史保留。客户端另区分 network_error、certificate_error 和 protocol_error。服务端不返回数据库原始错误及秘密。

阶段五新增保存与已读接口均须 Bearer 认证，generation 必须匹配。`POST /api/v1/saved` 输入 `{generation,body_after,objects:[{object_id,sha256}]}`；正文位置不能越过账号持久序号，最多 32 个已完整保存附件。确认按账号/设备记录，重复请求无新增状态事件，返回 `{saved:true,changed:bool}`。客户端下载长度和 SHA-256 通过后才提交附件确认，正文确认在附件下载前提交。

`POST /api/v1/read` 输入 `{generation,conversation_id,message_id}`，核实会话参与权限，账号级已读位置只能前进，返回 `{read:true,changed:bool}`。本机阅读意图先持久排队，断网不丢失；其它账号设备同步后清除该会话已读位置之前的本机未读。

`POST /api/v1/states` 输入 `{generation,after}`，返回 `{server_id,generation,events:[{seq,kind,data}],next,more}`，默认 50 条。此游标独立于消息正文游标。body_saved 的 data 为 conversation_id/user_id/device_id/server_order；object_saved 为 conversation_id/user_id/device_id/object_id；read 为 conversation_id/user_id/server_order。参与会话的双方账号均获得相应状态，自发只生成一份账号事件。server_order 为代次内持久全局消息顺序，64 位十进制字符串；保存状态和游标在本机同一事务提交。WSS 仍只提示 changed，不传递正文或秘密。

分页同时限制字节数和结构逗号数量，附件密集的页面可以少于请求数量，客户端以 next/more 连续补齐。

阶段二的提交约束：`(sender_user_id,request_id)` 唯一；同 ID 同内容返回已存在消息，不同内容返回 409。自发只创建一条消息及一条账号事件，双方发送各生成一条账号事件。事务同时提交消息、已完成附件引用及事件。WSS 不作为消息本体或确认存储。客户端在单一事务保存正文、下载任务并推进连续游标；附件延迟不阻止正文。

阶段三：幂等指纹绑定收件人、正文长度/内容和有序附件 ID；兼容已有 schema-2 无附件文本指纹。上传状态位图为十六进制字节，第 index 块位于字节 index/8 的低位 index%8。空文件为零块，仍须提交认证清单。当前文件上限及默认账号附件预留配额均为 10 GiB，包含未引用对象；清理与可配置完整配额在后续阶段。同步消息含 attachments 数组，元素为 object_id、filename（仅展示）、total_size 和原文 sha256。未提交消息前附件不可下载；第三账号不可越权。

旧快照恢复须保留 server_id、更新 generation；代次不匹配返回明确重新核对状态，不接受旧的过大 after 游标。后续接口实现时同步更新本文与验收记录，草案不等于已实现。

## 大文件校验（schema 8）

objects 新增 finalize_error。超过 32 MiB 上传进入 validating，最多两条后台任务验证块 AEAD、长度、原文 SHA-256 和有序密文摘要；验证时不占用全局对象锁。完成请求返回当前状态，只有 complete 后才能提交消息。查询包括 finalize_error，失败不标完成，客户端保留副本。进程重启恢复 uploading/interrupted，原块保留。迁移前保存加密 schema-7 快照。
