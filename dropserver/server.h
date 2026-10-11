#ifndef DROP_SERVER_H
#define DROP_SERVER_H
#include "drop_common.h"
#define SQLITE_HAS_CODEC 1
#include <sqlcipher/sqlite3.h>
#include <sodium.h>
#include "object.h"
#include "pake.h"

/* 服务器持久运行密钥格式，布局在两个架构间固定；由 DPAPI 或独立恢复包保护，账号密码不作为存储密钥。 */
typedef struct drop_keys {
    char magic[8];
    unsigned char database_key[32];
    unsigned char object_master_key[32];
    char server_id[DROP_ID_CAP];
    char generation[DROP_ID_CAP];
} drop_keys;
_Static_assert(sizeof(drop_keys) == 138, "runtime key format must be architecture independent");
/* 服务器共享上下文；后台线程结束后才可销毁，数据库、对象、连接及登录限速各由相应锁保护。 */
typedef struct drop_server {
    sqlite3 *database;
    drop_keys keys;
    WCHAR directory[32768];
    WCHAR backup_directory[32768];
    unsigned backup_seconds;
    HANDLE backup_thread, backup_request;
    HANDLE cloud_thread;
    HANDLE finalize_threads[2];
    CRITICAL_SECTION finalize_lock;
    /* 本机管理管道与线程；线程退出后才能关闭共享数据库及运行密钥。 */
    HANDLE admin_pipe, admin_thread;
    /* 数据目录独占锁，排除另一进程管理相同 SQLCipher 数据。 */
    HANDLE lock_file;
    /* 所有后台线程借用的退出事件，服务器拥有并在全部线程结束后关闭。 */
    HANDLE stop_event;
    /* 限制昂贵密码校验并发的信号量，登录路径必须归还已取得槽位。 */
    HANDLE password_slots;
    /* 保护共享数据库连接及事务，文件和网络操作避免长期持有此锁。 */
    CRITICAL_SECTION database_lock;
    CRITICAL_SECTION login_lock;
    /* 保护在线事件连接链表以及广播与关闭间的指针生命周期。 */
    CRITICAL_SECTION peers_lock;
    /* 串行化附件落盘、完整校验和清理，与数据库引用变更协调。 */
    CRITICAL_SECTION objects_lock;
    /* 串行化后台镜像与在线云盘命令；包含令牌刷新及远端写入，但不长期占用数据库锁。 */
    CRITICAL_SECTION cloud_lock;
    struct drop_peer *peers;
    ULONGLONG login_window;
    unsigned login_count;
    int use_zstd, zstd_level;
    unsigned connection_timeout_seconds;
    /* 分批孤立目录扫描的持久句柄，限制每轮清理工作量。 */
    HANDLE orphan_scan;
    WIN32_FIND_DATAW orphan_item;
    char dummy_password_hash[crypto_pwhash_STRBYTES];
    struct drop_pake_server *transport;
} drop_server;
/* 迁移账号 PAKE 材料表，旧结构先保留加密快照，原消息及附件保持可用。 */
int drop_pake_schema(drop_server *server);
/* 为大小写保留的账号生成独立盐及 SRP 验证材料，密码派生值和临时秘密随后清零。 */
int drop_pake_account_make(drop_server *server, const char *login, const char *password, drop_pake_record *record);
/* 在账号事务内保存 PAKE 盐、验证器及材料版本；不保存明文密码。 */
int drop_pake_account_store(drop_server *server, const char *user, const drop_pake_record *record);
/* 初始化 SRP 及受限握手缓存和锁，成功后服务器拥有其生命周期。 */
int drop_pake_server_start(drop_server *server);
/* 在请求停止后销毁全部握手槽位、同步锁和缓存，并清零传输状态。 */
void drop_pake_server_stop(drop_server *server);
/* 处理参数、SRP 开始及认证完成步骤，限制槽位和速率，验证双方证明后才签发设备会话。 */
int drop_pake_handshake(drop_server *server, const char *path, const cJSON *input, cJSON **output);
/* 在握手锁内认证 HTTP 帧，以 64 位滑动窗口拒绝重复或过旧序号；成功返回令牌、密钥和调用者须清理的明文。 */
int drop_pake_server_open(drop_server *server, const char *context, const unsigned char *frame, DWORD size, drop_pake_keys *keys, uint64_t *sequence, unsigned char **plain,
    DWORD *length, char token[DROP_TOKEN_CAP]);
/* 为已认证账号创建或更新设备及凭据，在事务中持久化；成功返回的登录 JSON 由调用者释放。 */
int drop_login_device(drop_server *server, const char *user, const char *device, const char *name, const char *pake_version, cJSON **response);
/* 独占数据目录，加载或初始化运行密钥，立即解锁 SQLCipher 并迁移；失败统一清理句柄及秘密。 */
int drop_server_open(drop_server *server, const WCHAR *directory, int initialize);
/* 先结束依赖数据库和密钥的后台线程，再关闭数据库、同步锁及句柄，并清零运行密钥。 */
void drop_server_close(drop_server *server);
/* 验证 ASCII 账号长度和允许字符，保留英文字母大小写，创建和登录使用同一规则。 */
int drop_account_login_valid(const char *login);
/* 校验账号和密码，生成 Argon2id 及 PAKE 材料后以事务创建；重复账号不覆盖原密码，返回明确错误。 */
drop_error drop_account_create_status(drop_server *server, const char *login, const char *display, const char *password);
/* 以布尔结果封装账号创建，详细失败原因由 drop_account_create_status 提供。 */
int drop_account_create(drop_server *server, const char *login, const char *display, const char *password);
/* 限制全局尝试速率和密码校验并发，验证账号后调用设备登录；未知账号也执行受限密码校验。 */
int drop_login(drop_server *server, const char *login, const char *password, const char *device_id, const char *device_name, cJSON **response);
/* 校验凭据摘要、有效期、账号启用和设备撤销状态；成功写入固定账号与设备 ID。 */
int drop_authorize(drop_server *server, const char *token, char user_id[DROP_ID_CAP], char device_id[DROP_ID_CAP]);
/* 撤销指定令牌对应的凭据，使后续 HTTP 和事件通道重新认证失败。 */
int drop_logout(drop_server *server, const char *token);
/* 按证书参数选择 HTTP/PAKE 或 HTTPS/TLS，注册受限路由及后台任务，等待退出并统一停止服务资源。 */
int drop_server_run(drop_server *server, const char *listen, const char *certificate);
/* 在事务中启停账号或重置密码及 PAKE 材料，并撤销旧凭据；失败回滚，不改动既有消息。 */
int drop_admin_account(drop_server *server, const char *login, int enabled, const char *new_password);
/* 按登录名和设备 ID 撤销设备及其凭据，其他设备和历史保留。 */
int drop_admin_revoke(drop_server *server, const char *login, const char *device);
/* 从 schema 10 迁移账户删除标记，先保存加密快照；历史仍引用原账号 ID。 */
int drop_admin_schema(drop_server *server);
/* 修改独立显示名，或删除可登录身份并释放用户名；删除保留历史和附件，撤销全部设备凭据。 */
int drop_admin_identity(drop_server *server, const char *login, const char *display, int deleting);
/* 分派本机管理请求，输出结构由调用者释放；网络和文件操作使用各自既有锁，避免长期占用数据库锁。 */
drop_error drop_admin_execute(drop_server *server, const cJSON *request, cJSON **response);
/* 创建仅限本机、实例用户和管理员的认证加密管理通道；同一实例只有一个管理线程。 */
int drop_admin_start(drop_server *server);
/* 请求停止并取消管道等待，等待线程退出后释放管理通道。 */
void drop_admin_stop(drop_server *server);
/* 使用实例密钥认证本机管理请求；返回 -1 表示实例未运行，0 表示请求失败，1 表示收到有效回复。 */
int drop_admin_local(const WCHAR *directory, cJSON *request, cJSON **response);
/* 释放含管理密码的 JSON，清零秘密字符串后释放对象。 */
void drop_admin_json_free(cJSON *json);
/* 向所有现有事件连接提示账户资料变化；无效凭据也收到提示以立即触发重新认证。 */
void drop_notify_accounts(drop_server *server);
/* 批量查询明确指定且有会话或已知登录名的账户资料；不提供账户全量枚举。 */
drop_error drop_profiles_read(drop_server *server, const char *user, const cJSON *request, cJSON **response);
/* 收紧数据树权限并将运行密钥及云盘秘密转为机器 DPAPI，供 Windows 服务使用；失败不宣称转换完成。 */
int drop_runtime_machine(drop_server *server);
/* 处理服务调度、安装、启停和卸载；固定数据路径，失败回滚本次安装，不影响其他服务。 */
int drop_service_command(int argc, WCHAR **argv);
/* 校验管理参数并分派初始化、账号、备份、恢复和运行命令，统一清理密码及服务器资源。 */
int drop_server_command(int argc, WCHAR **argv);
/* 将显式目录或 EXE 旁默认目录解析为确定路径，供前台和服务安装共用。 */
int drop_server_resolve_directory(const WCHAR *directory, WCHAR *path, size_t capacity);
/* 服务器完成监听后向服务控制器报告运行状态，并绑定其退出事件。 */
void drop_service_ready(drop_server *server);
/* 用独立恢复密码派生保护密钥，导出服务器运行密钥包；清零临时秘密，不依赖原机器 DPAPI。 */
int drop_recovery_export(drop_server *server, const WCHAR *path, const char *password);
/* 创建或续作在线加密快照并固定对象引用；全部密文及清单校验完成后才写完成标记。 */
int drop_backup_create(drop_server *server, const WCHAR *target, char task_id[DROP_ID_CAP]);
/* 仅向允许的新目标恢复；用独立密钥包核查数据库及全部附件后建立新的同步代次，失败不报告可用恢复。 */
int drop_backup_restore(const WCHAR *backup, const WCHAR *package, const WCHAR *target, const char *password);
/* 在隔离目录验证恢复包、在线备份和完整恢复，并检查错误密码及密文篡改拒绝。 */
int drop_backup_test(const WCHAR *root);
/* 建立持久备份任务和对象引用结构，使中断续作能复用同一快照。 */
int drop_backup_schema(drop_server *server);
/* 迁移附件完整校验状态，启动时将中断的 validating 对象恢复为可重新校验的上传状态。 */
int drop_finalize_schema(drop_server *server);
/* 发出退出事件并等待两个校验槽位结束，确保后续可安全关闭对象、数据库及密钥。 */
void drop_finalize_stop(drop_server *server);
/* 验证全部密文块、明文摘要及完整清单后发布 complete；后台模式响应退出，失败不允许消息引用。 */
drop_error drop_upload_finish_verify(drop_server *server, const char *user, const char *id, cJSON **output, int asynchronous);
/* 按受限 ACL 创建本实例备份事件和线程；未配置备份目录时直接成功，不启动后台任务。 */
int drop_backup_start(drop_server *server);
/* 通知并等待备份线程停止后释放句柄；服务器必须存活到线程退出。 */
void drop_backup_stop(drop_server *server);
/* 打开既有服务器目录对应的备份事件并置位，只请求运行实例执行备份。 */
int drop_backup_trigger(const WCHAR *directory);
/* 在隔离目录验证加密数据库、账号、凭据和真实密文篡改拒绝，不清除既有账号。 */
int drop_database_test(const WCHAR *directory);
/* 构造非空旧结构并验证迁移及加密回退快照，核对幂等重试和历史保留。 */
int drop_migration_test(const WCHAR *directory);
/* 在事务内构造受限模拟附件元数据验证真实分页序列化，结束回滚；不代表附件密文已验收。 */
int drop_pagination_test(drop_server *server, const char *user, const char *device);
/* 建立或迁移消息、幂等请求和同步序列结构，保留旧版正文指纹的重试语义。 */
int drop_text_schema(drop_server *server);
/* 在启动独占锁下先检查点再复制加密数据库及匹配密钥，核查回退副本后才允许结构迁移。 */
int drop_schema_snapshot(drop_server *server, int version);
/* 建立或迁移加密对象、分块进度和消息附件关系表，保持已有上传任务可恢复。 */
int drop_objects_schema(drop_server *server);
/* 建立设备保存、账号已读及独立状态序列表，供跨设备复制进度。 */
int drop_receipts_schema(drop_server *server);
/* 建立配额和孤立对象维护结构，升级前保留可恢复的加密快照。 */
int drop_maintenance_schema(drop_server *server);
/* 只清理超过宽限期且没有活动任务、消息或备份引用的对象，失败保留维护状态供后续重试。 */
drop_error drop_upload_cleanup(drop_server *server, uint64_t now);
/* 在隔离目录验证配额、活动引用保护及宽限清理，避免依赖真实磁盘填满。 */
int drop_maintenance_test(const WCHAR *root);
/* 检查账号消息和存储配额，超限明确返回空间错误，不自动删除历史。 */
drop_error drop_account_capacity(drop_server *server, const char *user, uint64_t extra);
/* 检查消息和附件权限，幂等保存设备确认并生成状态事件；失败回滚，未完成附件不可确认。 */
drop_error drop_saved_submit(drop_server *server, const char *user, const char *device, const cJSON *input, cJSON **output);
/* 校验账号可见的消息顺序，只向前推进账号已读位置，并以事务写入跨设备状态事件。 */
drop_error drop_read_submit(drop_server *server, const char *user, const cJSON *input, cJSON **output);
/* 按账号和状态游标分页返回保存及已读变化，限制页大小并报告后续位置。 */
drop_error drop_states_read(drop_server *server, const char *user, const cJSON *input, cJSON **output);
/* 校验附件描述和账号容量，创建独立加密对象及持久上传任务；失败不提供可引用对象。 */
drop_error drop_upload_create(drop_server *server, const char *user, const cJSON *input, cJSON **output);
/* 检查对象归属并返回持久上传进度，供客户端恢复缺失分块及等待完整校验。 */
drop_error drop_upload_status(drop_server *server, const char *user, const char *id, cJSON **output);
/* 核对归属、块编号和长度后在内存中加密并落盘；重复块必须内容一致，进度持久保存。 */
drop_error drop_upload_chunk(drop_server *server, const char *user, const char *id, uint64_t index, const unsigned char *data, DWORD length);
/* 小附件同步校验，大附件使用受限后台槽位；只有 complete 可供消息引用，重复请求返回当前状态。 */
drop_error drop_upload_finish(drop_server *server, const char *user, const char *id, cJSON **output);
/* 检查消息参与权限和对象完成状态，认证并解密指定块；成功明文由调用者清零和 free。 */
drop_error drop_attachment_chunk(drop_server *server, const char *user, const char *id, uint64_t index, unsigned char **data, DWORD *length);
/* 按消息附件顺序读取元数据并填入调用者提供的数组，不把未完成上传当作可下载附件。 */
int drop_attachment_list(drop_server *server, const char *message, cJSON *array);
/* 校验目标和完整附件，以账号与请求 ID 保证幂等；同事务保存消息及双方事件，提交后才返回接收成功。 */
drop_error drop_message_submit(drop_server *server, const char *user, const char *device, const cJSON *input, cJSON **output, char recipient[DROP_ID_CAP]);
/* 校验服务器代次和账号游标，按记录数及响应容量返回有序增量；正文及附件关联来自持久数据。 */
drop_error drop_sync_read(drop_server *server, const char *user, const cJSON *input, cJSON **output);
/* 按登录名查询可发送账号的固定 ID，不公开密码、设备凭据或管理信息。 */
drop_error drop_account_lookup(drop_server *server, const cJSON *input, cJSON **output);
/* 向发送和接收账号的有效在线设备提示变化；通知只触发补拉，持久消息由增量接口提供。 */
void drop_notify(drop_server *server, const char *user, const char *recipient);
/* 比较在线账号的持久状态序号，仅在状态推进后提示补拉，写成功才更新连接记录。 */
void drop_notify_states(drop_server *server);
#endif
