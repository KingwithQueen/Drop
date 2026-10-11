#ifndef DROP_CLOUD_H
#define DROP_CLOUD_H
#include "server.h"
/* 单次云盘操作的授权和网络上下文；只上传既有密文，credentials 与 access 在关闭时清零。 */
typedef struct drop_cloud {
    drop_server *server;
    cJSON *credentials;
    char access[4097];
    WCHAR origin[2049], token_origin[2049], ca[32768], secret[32768];
    int machine;
    /* 打开上下文的线程持有云盘操作锁，关闭时归还，不允许跨线程移交上下文。 */
    int locked;
} drop_cloud;
/* 云盘 HTTP 回复及续传头，结构拥有 data，由 drop_cloud_reply_free 清零并释放。 */
typedef struct cloud_reply {
    DWORD status, size;
    unsigned char *data;
    WCHAR location[4097], range[256];
} cloud_reply;
/* 建立云盘备份、文件映射和续传会话结构，持久记录失败及已确认偏移。 */
int drop_cloud_schema(drop_server *server);
/* 按运行身份取得独占云盘上下文并刷新令牌；成功或失败均须在同一线程关闭，避免与后台镜像同时操作续传及授权。 */
drop_error drop_cloud_open(drop_cloud *cloud, drop_server *server);
/* 清理云盘访问令牌和授权 JSON、归还打开时取得的锁；调用者另行释放结构本身。 */
void drop_cloud_close(drop_cloud *cloud);
/* 执行受限目标的云盘请求，按需携带访问令牌并响应退出事件；成功正文由 drop_cloud_reply_free 清理。 */
drop_error drop_cloud_http(drop_cloud *cloud, const WCHAR *method, const WCHAR *url, const WCHAR *headers, const void *body, DWORD length, int authenticated, cloud_reply *reply);
/* 清零并释放云盘响应正文，再重置回复结构。 */
void drop_cloud_reply_free(cloud_reply *reply);
/* 返回云盘回应对应的项目错误，保留重试和授权失效的区别。 */
drop_error drop_cloud_status(const cloud_reply *reply);
/* 使用刷新令牌取得新的访问令牌，更新持久秘密；授权失败保留明确错误供重新授权。 */
drop_error drop_cloud_refresh(drop_cloud *cloud);
/* 发送云盘 JSON 请求，处理令牌刷新并解析受限响应；成功对象由调用者释放。 */
drop_error drop_cloud_json(drop_cloud *cloud, const WCHAR *method, const WCHAR *path, cJSON *input, cJSON **output);
/* 启动带 PKCE 和 state 的浏览器授权及临时回环回调，成功后 DPAPI 保存；结束时关闭监听并清理秘密。 */
int drop_cloud_authorize(drop_server *server, const WCHAR *client_file);
/* 仅在诊断构建中使用明确模拟端点和 CA 完成授权，测试凭据不进入生产入口。 */
int drop_cloud_test_authorize(drop_server *server, const WCHAR *origin, const WCHAR *ca);
/* 验证本地完成备份并上传其密文文件，完整核查后发布远端完成标记；持久状态支持中断续作。 */
drop_error drop_cloud_upload(drop_cloud *cloud, const WCHAR *backup);
/* 分页查询应用私有云盘空间，可按任务 ID 和完成标记过滤；成功数组由调用者 cJSON_Delete。 */
drop_error drop_cloud_list(drop_cloud *cloud, const char *task, int complete_only, cJSON **files);
/* 读取已发布的远端备份并逐项校验下载，全部成功后才接受完成清单；不在云端或下载时解密。 */
drop_error drop_cloud_download(drop_cloud *cloud, const char *task, const WCHAR *target);
/* 显式删除指定远端备份，先移除完成标记，并记录本机 deleted 状态以避免后台重新镜像。 */
drop_error drop_cloud_delete(drop_cloud *cloud, const char *task);
/* 分派云盘状态、上传、列出、下载和显式删除命令；统一清理授权状态并报告业务错误。 */
int drop_cloud_command(drop_server *server, int argc, WCHAR **argv);
/* 仅在存在备份目录及本机授权材料时启动镜像线程，未配置云盘不妨碍服务器启动。 */
int drop_cloud_start(drop_server *server);
/* 通知并等待镜像线程结束，再释放句柄，确保服务器及密钥不被提前销毁。 */
void drop_cloud_stop(drop_server *server);
#endif
