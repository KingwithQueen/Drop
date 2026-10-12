#ifndef DROP_NETWORK_H
#define DROP_NETWORK_H
#include "drop_common.h"
#include "pake.h"
/* 使用默认系统证书信任发送 JSON 请求；成功响应由调用者 cJSON_Delete。 */
drop_error drop_http_json(const WCHAR *url, const WCHAR *method, const WCHAR *path, const char *token, const cJSON *body, cJSON **response, DWORD *status);
/* 查询服务器信息并检查协议版本及标识；HTTP 探测仅表示可达，尚未建立 PAKE 身份信任。 */
int drop_network_probe(const WCHAR *url);
/* 设置网络请求共用的退出事件；调用者保证事件在所有请求及回调结束前保持有效。 */
void drop_network_cancel_event(HANDLE event);
/* 可被退出事件打断的网络退避等待；返回 1 表示等待时间已到，0 表示应停止。 */
int drop_network_wait(DWORD milliseconds);
/* 序列化受限 JSON，按指定 CA 和传输模式请求并解析响应；临时正文清零，成功响应归调用者所有。 */
drop_error drop_http_json_ca(const WCHAR *url, const WCHAR *method, const WCHAR *path, const char *token, const cJSON *body, cJSON **response, DWORD *status, const WCHAR *ca_file);
/* 根据地址显式选择 HTTPS/TLS 或 HTTP/PAKE，校验大小及指针，不自动降级加密模式。 */
drop_error drop_http_bytes(const WCHAR *url, const WCHAR *method, const WCHAR *path, const char *token, const unsigned char *body, DWORD body_size, int binary_request,
    int binary_response, unsigned char **response, DWORD *response_size, DWORD *status, const WCHAR *ca_file);
/* 执行可取消的原始 HTTP 请求，禁止重定向和隐式凭据发送；HTTPS 校验证书，关闭回调后才释放状态及缓冲。 */
drop_error drop_http_bytes_raw(
    const WCHAR *, const WCHAR *, const WCHAR *, const char *, const unsigned char *, DWORD, int, int, unsigned char **, DWORD *, DWORD *, const WCHAR *);
/* 串行认证并加密 HTTP 请求，将方法、路由和序号绑定到帧；验证响应后返回正文，失败保留明确错误。 */
drop_error drop_pake_client_http(const WCHAR *, const WCHAR *, const WCHAR *, const char *, const unsigned char *, DWORD, int, int, unsigned char **, DWORD *, DWORD *);
/* 将已解开的 DPAPI 凭据绑定到受锁保护的有限缓存；复用同地址和令牌的已有槽位，临时秘密始终清零。 */
int drop_pake_client_bind(const WCHAR *url, const cJSON *credential);
/* 移除指定地址和令牌的缓存凭据，同时清零会话密钥及认证材料。 */
void drop_pake_client_forget(const WCHAR *url, const char *token);
/* 只使缓存传输会话失效，保留受保护凭据解开的认证材料以便重新握手。 */
void drop_pake_client_expire(const WCHAR *url, const char *token);
/* 生成绑定 WebSocket 路由的认证头，并派生本次连接专用双向密钥；输出秘密由连接拥有者清理。 */
int drop_pake_client_channel(const WCHAR *url, const char *token, drop_pake_keys *keys, WCHAR header[512]);
/* 事件连接拥有者或持有引用的副本；io 负责并发收发与关闭，秘密随副本释放清零。 */
typedef struct drop_ws {
#ifdef _WIN32
    HINTERNET session, connection, socket;
#endif
    struct drop_ws_io *io;
    int encrypted;
    drop_pake_keys keys;
    uint64_t tx_sequence, rx_sequence;
} drop_ws;
/* 建立事件通道；HTTP 会话失效时清除缓存会话并尝试重新认证一次，HTTPS 保持 TLS 模式。 */
drop_error drop_ws_open(const WCHAR *url, const char *token, drop_ws *ws, const WCHAR *ca_file);
/* 中止 socket 并等待关闭回调，再释放上层 WinHTTP 句柄和拥有者引用，最后清零密钥状态。 */
void drop_ws_close(drop_ws *ws);
/* 为升级后的 socket 安装独立收发事件和关闭回调；成功后 socket 回调与调用者各持有引用。 */
int drop_ws_attach(drop_ws *ws);
/* 为跨线程复制的连接状态增加引用，必须与 drop_ws_release 成对使用。 */
void drop_ws_retain(drop_ws *ws);
/* 释放当前连接副本持有的状态引用并清零副本；不代替拥有者关闭整个连接。 */
void drop_ws_release(drop_ws *ws);
/* 接收 TLS 文本或解密 PAKE 帧，要求加密帧序号严格连续；认证失败不交付内容。 */
DWORD drop_ws_receive(drop_ws *ws, void *buffer, DWORD capacity, DWORD *length, WINHTTP_WEB_SOCKET_BUFFER_TYPE *type, HANDLE cancel);
/* 按连接模式发送文本或认证帧，递增独立发送序号；失败后由上层关闭并重建连接。 */
DWORD drop_ws_send(drop_ws *ws, const void *buffer, DWORD length, HANDLE cancel);
#endif
