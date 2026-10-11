#ifndef DROP_TRUST_H
#define DROP_TRUST_H
#include "drop_common.h"
#include <wincrypt.h>
/* 单个 WinHTTP 请求的证书和异步完成状态，必须存活到关闭回调结束后才能释放。 */
typedef struct drop_trust {
    HCERTSTORE roots;
    HCERTCHAINENGINE engine;
    WCHAR host[256];
    LONG rejected, closed;
    int checked;
    HANDLE completed, closing;
    DWORD completion_status, operation_error, available;
} drop_trust;
/* 无 CA 文件时使用系统信任；指定 CA 时建立独占根引擎，仍验证主机名、日期及用途，失败释放资源。 */
int drop_trust_prepare(drop_trust *trust, HINTERNET request, const WCHAR *host, const WCHAR *ca_file);
/* 在请求回调结束后释放事件、证书链引擎和根存储，并清空结构。 */
void drop_trust_free(drop_trust *trust);
/* 创建完成和关闭事件并安装 WinHTTP 回调；trust 必须存活到 drop_trust_close 等到关闭回调之后。 */
int drop_trust_async(drop_trust *trust, HINTERNET request);
/* 重置下一次异步操作的完成状态；同一 request 上的操作必须顺序进行。 */
void drop_trust_begin(drop_trust *trust);
/* 等待预期回调、取消或超时，检查证书和操作错误；失败设置 Windows 错误供上层诊断。 */
int drop_trust_wait(drop_trust *trust, HINTERNET request, DWORD expected, HANDLE cancel);
/* 仅关闭请求一次并等待 HANDLE_CLOSING，保证回调不再访问 trust 和请求缓冲。 */
void drop_trust_close(drop_trust *trust, HINTERNET request);
#endif
