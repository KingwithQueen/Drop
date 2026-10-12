# 1.0.3 账户与在线管理

当前发行版为 1.0.3，账户在线管理自 1.0.1 提供。替换 EXE/DLL 时正常停止一次并保留原数据目录；从 schema 10 升级的服务器先保存加密回退快照并迁移到 11，已有 1.0.1/schema 11 无需新的迁移。不重新初始化、不重新授权 Google、不重新安装服务。新版启动后，以下操作可以保持服务运行。

## 账户命令

在服务器的本机控制台执行；LocalSystem 服务使用提升的管理员控制台。--data 必须指向实际服务的数据目录。

```powershell
$exe = 'E:\Program Files\Drop\dropserver.exe'
$data = 'E:\ProgramData\DropData\server'
& $exe account-list --data $data
& $exe account-create --data $data --login 'NewUser' --display '新用户'
& $exe account-display --data $data --login 'NewUser' --display '新的显示名字'
& $exe account-delete --data $data --login 'NewUser'
```

创建和重置密码仍在遮蔽控制台输入两次，密码不进入命令行。显示名可使用中文，长度为 1–128 个 UTF-8 字节；修改显示名不改变登录名及大小写。在线客户端自动更新用户列表、会话标题和历史消息的发送者名称，不必重新登录或再次查找。名称未知时先显示账号 ID，连接恢复后补齐。旧版客户端须升级后才有自动资料同步。

删除撤销全部登录、清除密码和 PAKE 认证材料、释放登录名，保留历史消息、已引用附件、原账号 ID 和最后的显示名。其他参与者仍可读取已有内容；客户端标记“已删除”，禁止向原身份继续发送。删除自己账号后，客户端保留本机历史并提示凭据失效。重新创建同名账号得到新 ID，不继承旧账号的数据和权限。删除不能用 account-enable 撤销；临时禁止登录请使用 account-disable。

account-list 输出 JSON，rows 每行依次为账号 ID、登录名、显示名、启用状态、字节配额，各字段均为字符串。每页最多 100 个未删除账号；more 为 true 时，以 next 作为 --after 的值继续读取。

```powershell
& $exe account-disable --data $data --login 'NewUser'
& $exe account-enable --data $data --login 'NewUser'
& $exe account-reset --data $data --login 'NewUser'
& $exe device-revoke --data $data --login 'NewUser' --device '实际设备ID'
& $exe quota-set --data $data --login 'NewUser' --bytes 10737418240
```

账号停用、删除、密码重置和设备撤销立即影响后续认证；启用后需重新登录，旧令牌不会恢复。配额修改影响新写入，不删除已有内容。资料通过 WebSocket 变化提示刷新，并由定时对账补齐遗漏通知；不向客户端枚举陌生账号目录。

## 在线支持范围

在线支持账户创建/启停/删除/改显示名/重置密码、设备撤销、配额、account-list、storage-status、cleanup、backup、recovery-export，以及完成初次授权后的 drive-status/list/upload/download/delete。

```powershell
& $exe storage-status --data $data
& $exe cleanup --data $data
& $exe backup --data $data --target 'E:\DropBackups'
& $exe recovery-export --data $data --target 'E:\Protected\DropRecovery.bin'
```

backup 等待完整结果，成功任务目录包含 complete.bin；backup-trigger 仍仅提交后台请求。cleanup 仅处理符合期限且未引用的上传/残留文件，保留历史附件。独立恢复密码仍需遮蔽输入两次，和恢复包分开保存。

运行中的服务器通过仅限本机的命名管道执行操作，复用原实例的锁和事务；受 Windows 权限限制，并使用实例密钥认证加密请求，不开放远程管理端点。服务未运行时，同一命令取得原有目录独占锁后离线执行。在线认证或通信失败不会改为另一进程直接写库；结果不确定时先查询状态再重试。

恢复到新目录、runtime-machine 身份转换、Google 首次授权、替换程序、监听端口/证书/启动参数变更仍按各自部署流程执行，必要时停止服务。请使用管理命令操作数据，不使用外部工具直接编辑加密数据库或运行密钥。
