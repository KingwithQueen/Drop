#ifndef DROP_CLIENT_H
#define DROP_CLIENT_H
#include "network.h"
#include <sqlite3.h>

/* 由单个同步核心拥有的本机上下文；目录锁排除其他写入者，UI 通过任务读取数据，关闭时清零凭据。 */
typedef struct drop_client {
    sqlite3 *database;
    /* 本机目录独占锁，不作为跨线程数据库锁。 */
    HANDLE lock;
    /* 借用界面的手动复位事件；交互任务到达时在分块边界让出传输，关闭核心不释放此句柄。 */
    HANDLE work_pending;
    WCHAR root[32768], url[2049], ca_file[32768];
    WCHAR files[32768];
    char user[DROP_ID_CAP], device[DROP_ID_CAP], server[DROP_ID_CAP], generation[DROP_ID_CAP], token[DROP_TOKEN_CAP];
    /* 当前进程实际下载字节统计，不代替附件完成校验。 */
    uint64_t downloaded_bytes;
    int use_zstd, zstd_level;
    unsigned heartbeat_seconds, connection_timeout_seconds, reconcile_seconds;
    /* 单次增量页大小，零值沿用核心默认上限。 */
    unsigned sync_page_limit;
    /* 单次下载工作量，零值使用默认策略，防止下载长期阻塞正文同步。 */
    uint64_t download_quantum;
    /* 提示工作线程继续处理持久任务，不能据此将游标提前推进。 */
    int more_bodies, more_downloads, more_profiles, more_deletions;
    /* 由经认证服务器身份检查更新；旧服务器不广告此扩展时，消息删除入口保持不可用。 */
    int deletion_supported;
    /* 可重建的名称缓存按账号 ID 分批刷新；进程重启从第一页重新检查，不影响消息游标。 */
    char profile_after[DROP_ID_CAP];
    /* 服务器代次变化后的本机复核标记，阻止不确定发送任务盲目重发。 */
    int restored;
} drop_client;
/* 按所选 TLS 或 PAKE 模式登录并持久保存 DPAPI 凭据；检查服务器身份，失败不建立可用会话。 */
int drop_client_login(const WCHAR *root, const WCHAR *url, const char *login, const char *password, const WCHAR *ca_file);
/* 锁定并打开本机数据目录，加载受保护凭据、数据库及迁移状态；失败统一释放已取得资源。 */
int drop_client_open(drop_client *client, const WCHAR *root);
/* 关闭数据库和目录锁，撤销进程内凭据绑定并清零客户端状态。 */
void drop_client_close(drop_client *client);
/* 将纯文本消息加入持久发送队列并生成稳定请求 ID，后续重试沿用同一 ID。 */
drop_error drop_client_queue(drop_client *client, const char *body, const char *recipient, char request[DROP_ID_CAP]);
/* 依次上传持久待发送消息及附件；服务器确认后才更新状态，失败保留任务及原请求 ID。 */
drop_error drop_client_flush(drop_client *client);
/* 拉取一页增量记录，在同一事务中保存正文、附件任务及游标；失败不跳过记录，附件下载另行推进。 */
drop_error drop_client_sync(drop_client *client);
/* 刷新自己、已知联系人及历史参与者的显示名和账号状态，每次最多一百个，不枚举陌生账号。 */
drop_error drop_client_profiles(drop_client *client);
/* 保存经过认证且字段完整的账号资料；调用者负责事务，返回协议或数据库错误，不修改消息正文。 */
drop_error drop_client_profile_save(drop_client *client, const cJSON *profile);
/* 使用当前凭据连接事件通道，处理变化提示并补拉持久消息；用于诊断客户端。 */
int drop_client_watch(drop_client *client);
/* 在隔离目录验证本机队列、迁移和容量错误处理，保持已有待发送记录可恢复。 */
int drop_client_test(const WCHAR *root);
/* 建立或迁移本机附件上传、下载和消息关联结构，保留持久续传进度。 */
int drop_client_files_schema(drop_client *client);
/* 先创建正文及附件的稳定本机副本，再以事务记录发送任务；失败不将未保存附件标记为成功。 */
drop_error drop_client_queue_files(drop_client *client, const char *body, const char *recipient, const WCHAR *const *paths, int count, char request[DROP_ID_CAP]);
/* 按发送任务顺序推进全部附件上传，并构造提交消息所需的附件对象列表。 */
drop_error drop_client_uploads(drop_client *client, const char *request, cJSON *array);
/* 按设定工作量推进待下载附件，记录是否仍有任务，避免一次循环长期占用同步线程。 */
drop_error drop_client_downloads(drop_client *client);
/* 重新核查完成缓存，缺失或损坏时恢复下载任务，避免错误地继续报告附件已保存。 */
int drop_client_verify_files(drop_client *client);
/* 从已校验消息登记自动下载任务；在正文同步事务内调用，确保游标与任务一起提交。 */
drop_error drop_client_register_files(drop_client *client, const cJSON *message);
/* 建立本机界面和通知去重元数据，迁移时保留已有历史。 */
int drop_client_ui_schema(drop_client *client);
/* 升级本机保存确认、已读和顺序元数据结构；保留旧正文、附件及通知去重记录。 */
int drop_client_receipts_schema(drop_client *client);
/* 升级本机删除标记、离线意图和受限文件清理队列，升级前保留完整数据库快照。 */
int drop_client_deletions_schema(drop_client *client);
/* 按账号确认本机消息归属并持久记录删除意图，允许其他发送设备的消息，断网仍保留任务。 */
drop_error drop_client_delete_queue(drop_client *client, const char *message);
/* 在调用者的同步事务内保存经身份校验的删除标记，清除正文、附件任务和相应发送副本元数据。 */
drop_error drop_client_deleted_save(drop_client *client, const cJSON *message);
/* 检查已删除消息的重放身份；返回 1 表示应忽略正文，0 表示正常消息，-1 表示身份或数据库错误。 */
int drop_client_deleted_known(drop_client *client, const cJSON *message);
/* 优先重试删除意图并补拉独立删除序列；文件回收只处理已持久提交的内部缓存路径。 */
drop_error drop_client_deletions(drop_client *client);
/* 保存服务端消息顺序，用于已读位置和状态关联；重放不覆盖本机正文及文件状态。 */
drop_error drop_client_message_order(drop_client *client, const cJSON *message);
/* 依次处理正文确认、可选附件确认、已读提交及状态增量，失败保留未完成步骤。 */
drop_error drop_client_receipts(drop_client *client, int include_files);
/* 优先提交已读并拉取状态，不上传附件、不重新散列缓存文件；断网保留持久已读队列。 */
drop_error drop_client_read_receipts(drop_client *client);
/* 将消息对应的账号已读位置加入本机持久队列，网络恢复后再提交。 */
int drop_client_read_queue(drop_client *client, const char *message);
/* 在本机结构升级前保存对应版本的数据库快照，为失败后的手动恢复保留依据。 */
int drop_client_snapshot(drop_client *client, int version);
/* 将本机 SQLite 错误映射为项目错误，单独报告存储空间不足。 */
drop_error drop_client_database_error(drop_client *client);
/* 检查服务器身份和同步代次；恢复旧快照时保留本机历史，并将不确定发送任务转为待人工确认。 */
drop_error drop_client_reconcile(drop_client *client);
/* 显式恢复经用户确认的待复核发送任务，使其重新进入发送流程。 */
drop_error drop_client_retry_review(drop_client *client);
#endif
