# HTTP/PAKE 与 HTTPS 双模式

2026-10-11 的新增要求覆盖原计划中的 HTTPS 单模式限制。客户端按地址明确选择；HTTP 使用 SRP-6a 密码认证建立会话，然后用 XChaCha20-Poly1305 加密和认证正文、附件块、登录令牌、设备资料、响应及 WebSocket 事件。HTTPS 保留 Windows/应用内 CA 证书检查和 TLS/WSS，发送原业务 JSON 与附件块，不叠加 PAKE 帧。服务器不带 --certificate 时监听 HTTP，带证书时监听 HTTPS；同一实例只提供所选模式。HTTPS 失败不自动转为 HTTP。

这属于客户端到中央服务器的传输加密。服务器仍能处理明文，跨账号以及同账号不同设备均使用各自会话密钥；不保存账号明文密码，不以密码作为静态传输密钥。SQLCipher、附件认证加密、加密备份、Google HTTPS 后端独立保留。

## 认证与凭据

套件固定为 `srp6a-sha256-4096-argon2id-xchacha20poly1305-v1`。SRP-6a 来自固定的 cocagne/csrp rfc5054_compat 实现，使用 4096 位固定素数、生成元 5、SHA-256、RFC 5054 的整数填充。密码先经 Argon2id（ops=2，64 MiB，输出 32 字节）和随机 16 字节账号盐处理；SRP 的标识为 `drop-pake-v1:<server_id>:<login>`，另用随机 32 字节 SRP 盐。login 保留账号名原始大小写，不作大小写转换，与 HTTPS 的精确账号匹配一致。账号创建与本机密码重置在同一数据库事务保存原 HTTPS 密码哈希及 PAKE 验证材料。

客户端首次 HTTP 登录访问三个 POST JSON 端点：`/api/v1/pake/parameters`、`/start`、`/finish`。前两步交换固定套件、账号盐、服务器 ID 和临时公值；它们在服务器证明验证前不可信。start 返回随机 session_id、SRP 盐、B；finish 包含客户端证明与加密的设备元数据。服务器验证证明并检查账号/凭据后，返回服务器证明与加密的登录响应。客户端必须先验证服务器证明，再接受令牌及服务器信息。明文密码及派生凭据从不发送到 HTTP 服务器。

缓存的是 Argon2id 派生凭据、账号盐、账号名及绑定的服务器/设备/令牌，保存在用户 DPAPI 保护的 session.dpapi 中；每次进程启动、会话到期或服务器重启会建立新的 PAKE 会话，原设备令牌恢复时不轮换。缓存凭据具有登录能力，不能作为普通配置文件复制或公开。关闭客户端会擦除进程内缓存。密码重置更换盐/验证材料并撤销设备令牌，需要重新登录。会话待认证期限 30 秒、认证期限一小时；客户端提前重新认证。最多 64 个会话，每秒最多 4 次 start，最多 2 个并发密码计算；设备重新认证清理其旧 HTTP 会话。

服务器 schema 9 自动建立 schema 10 表并保留加密迁移快照，但旧密码哈希无法转换为 PAKE 验证材料。旧账号继续支持 HTTPS 登录。启用 HTTP 前停止服务器，执行 `dropserver.exe account-reset --data "E:\DropData\server" --login alice`，在本机输入密码（可以使用原密码），再重启并在客户端重新登录。该操作撤销账号的旧设备凭据，不删除消息或附件。禁止通过明文 HTTP 上传密码来补建材料。

## 记录格式与重放处理

SRP 共享值的 SHA-256 结果作为 keyed BLAKE2b 密钥，输入为套件字符串及 NUL、32 字节 ASCII server_id、16 字节 session_id，得到 32 字节主密钥。libsodium KDF 上下文为 8 字节 `DrpPAKE1`，子密钥 ID 0..5 分别对应 finish 客户端/服务器、HTTP 客户端/服务器、WS 客户端/服务器。不同方向和用途分离。

帧为 `DPK1`（4 字节）、session_id（16 字节）、序号（8 字节小端）、密文及 16 字节认证标签。XChaCha nonce 为 session_id+序号；AAD 为前 28 字节头与 context 的 SHA-256。finish 序号为 0，context 为 `pake-finish-v1`；HTTP 序号从 1 开始，context 为 `METHOD:/api/v1/path`。请求明文为类型标记（0 JSON / 1 二进制）、64 字节令牌及原正文；响应明文为 4 字节小端业务 HTTP 状态、类型标记及原响应。外层 HTTP 成功为 200/octet-stream，业务错误也加密。仅未登录 server-info、握手或无法验证的请求返回公开 JSON；HTTP 不接受原来的明文 login、Bearer REST、WS 文本应用帧。

服务器在验证标签后才更新 64 位滑动重放窗口，允许窗口内乱序但每序号只接受一次；方法、路由、方向、会话和内容替换均使认证失败。客户端要求响应序号对应当前请求。网络重试仍依靠原消息 request_id 和附件块幂等性；传输认证不替代业务所有权验证。

HTTP WS 使用 `/api/v1/pake/events`；X-Drop-Channel 头携带绑定 GET 路径的加密 HTTP 记录。将该记录序号加入 `ws-channel-v1` 上下文，分别用 keyed BLAKE2b 派生新的双向 WS 子密钥，防止重连时序号归零导致 nonce 重用。每个方向从 1 开始严格递增；正文事件和心跳均为二进制认证帧，context 为 `ws-v1`。协议 Ping/Pong/Close 控制帧不承载 Drop 消息。HTTP 握手和请求均禁止重定向和集成 Windows 认证。

## 边界与验收

HTTP 的地址、路径、登录握手中的账号名/盐/公值、流量长度和时序仍可见；PAKE 保护账号认证和应用内容，不提供 TLS 对整个 HTTP 通道的覆盖。主动攻击者仍可阻断连接，弱密码仍面临在线猜测，终端或服务器被控制后不能靠传输加密保护进程明文。此实现完成本机互操作和故障验收，并未经过独立密码协议安全审计。未来使用 HTTPS 时只需证书和 https 地址，Google 授权及数据存储逻辑不因模式切换而改变。

`tests/pake.py` 使用独立 Python 大整数/SHA-256/BLAKE2b 运算以及 libsodium 原语核对 native SRP 证明、方向密钥和记录；另验证错误密码、公值范围、标签篡改、路由替换、重放/乱序、跨账号文本、多块附件、缓存凭据重连及加密 WS。`tests/integration.py` 保留完整 HTTPS 回归。具体运行记录及部署边界见 VERIFICATION.md。
