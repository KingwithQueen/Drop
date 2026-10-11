/* WinHTTP 异步事件收发；发送和接收可并行，各自只允许一个操作，引用计数保护关闭回调状态。 */
#include "network.h"
#include <sodium.h>
#include <stdlib.h>
#include <string.h>

/* socket 异步状态及共享序号；发送和接收各有完成事件，回调持有引用直到 HANDLE_CLOSING。 */
typedef struct drop_ws_io {
    HINTERNET socket;
    HANDLE received, sent, closed;
    CRITICAL_SECTION guard;
    /* 原子引用计数和一次性关闭标记，保证回调状态在最后一个使用者离开后才释放。 */
    LONG references, closing;
    DWORD rx_error, tx_error, bytes;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
    /* 连接副本共用的方向序号；单发送者和单接收者分别维护，不能退回或复用。 */
    uint64_t tx_sequence, rx_sequence;
} drop_ws_io;
/* 减少异步状态引用计数，最后一个引用释放事件和锁；关闭回调持有的引用也必须归还。 */
static void release(drop_ws_io *io) {
    if (io && InterlockedDecrement(&io->references) == 0) {
        CloseHandle(io->received);
        CloseHandle(io->sent);
        CloseHandle(io->closed);
        DeleteCriticalSection(&io->guard);
        free(io);
    }
}
/* 将 WinHTTP 完成或错误回调转为收发事件；句柄关闭回调释放其引用，防止提前销毁状态。 */
static void CALLBACK completed(HINTERNET handle, DWORD_PTR context, DWORD status, void *info, DWORD size) {
    drop_ws_io *io = (drop_ws_io *)context;
    (void)handle;
    if (!io) return;
    if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
        SetEvent(io->closed);
        release(io);
        return;
    }
    if (status == WINHTTP_CALLBACK_STATUS_READ_COMPLETE && info && size == sizeof(WINHTTP_WEB_SOCKET_STATUS)) {
        WINHTTP_WEB_SOCKET_STATUS *result = info;
        io->bytes = result->dwBytesTransferred;
        io->type = result->eBufferType;
        SetEvent(io->received);
    } else if (status == WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE)
        SetEvent(io->sent);
    else if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR && info && size == sizeof(WINHTTP_ASYNC_RESULT)) {
        DWORD error = ((WINHTTP_ASYNC_RESULT *)info)->dwError;
        io->rx_error = error;
        io->tx_error = error;
        SetEvent(io->received);
        SetEvent(io->sent);
    }
}
/* 为升级后的 socket 安装独立收发事件和关闭回调；成功后 socket 回调与调用者各持有引用。 */
int drop_ws_attach(drop_ws *ws) {
    drop_ws_io *io = calloc(1, sizeof(*io));
    DWORD_PTR context;
    if (!io) return 0;
    io->references = 1;
    io->socket = ws->socket;
    InitializeCriticalSection(&io->guard);
    io->received = CreateEventW(NULL, TRUE, FALSE, NULL);
    io->sent = CreateEventW(NULL, TRUE, FALSE, NULL);
    io->closed = CreateEventW(NULL, TRUE, FALSE, NULL);
    context = (DWORD_PTR)io;
    if (!io->received || !io->sent || !io->closed || !WinHttpSetOption(ws->socket, WINHTTP_OPTION_CONTEXT_VALUE, &context, sizeof(context)) ||
        WinHttpSetStatusCallback(ws->socket, completed,
            WINHTTP_CALLBACK_FLAG_READ_COMPLETE | WINHTTP_CALLBACK_FLAG_WRITE_COMPLETE | WINHTTP_CALLBACK_FLAG_REQUEST_ERROR | WINHTTP_CALLBACK_FLAG_HANDLES,
            0) == WINHTTP_INVALID_STATUS_CALLBACK) {
        WinHttpSetStatusCallback(ws->socket, NULL, 0, 0);
        release(io);
        return 0;
    }
    InterlockedIncrement(&io->references);
    ws->io = io;
    return 1;
}
/* 为跨线程复制的连接状态增加引用，必须与 drop_ws_release 成对使用。 */
void drop_ws_retain(drop_ws *ws) {
    if (ws->io) InterlockedIncrement(&ws->io->references);
}
/* 释放当前连接副本持有的状态引用并清零副本；不代替拥有者关闭整个连接。 */
void drop_ws_release(drop_ws *ws) {
    release(ws->io);
    sodium_memzero(ws, sizeof(*ws));
}
/* 在状态锁内只关闭 socket 一次，允许发送、接收和取消路径同时请求中止。 */
static void abort_socket(drop_ws_io *io) {
    if (!io) return;
    EnterCriticalSection(&io->guard);
    if (!InterlockedExchange(&io->closing, 1)) WinHttpCloseHandle(io->socket);
    LeaveCriticalSection(&io->guard);
}
/* 等待操作完成、连接关闭或取消；取消和超时先关闭并等关闭回调，确保异步缓冲不再被访问。 */
static DWORD wait_result(drop_ws_io *io, HANDLE ready, HANDLE cancel, DWORD timeout) {
    HANDLE events[3] = {ready, io->closed, cancel};
    DWORD result = WaitForMultipleObjects(cancel ? 3 : 2, events, FALSE, timeout);
    if (result == WAIT_OBJECT_0) return NO_ERROR;
    if (result == WAIT_TIMEOUT || result == WAIT_OBJECT_0 + 2) {
        abort_socket(io);
        WaitForSingleObject(io->closed, INFINITE);
    }
    return result == WAIT_TIMEOUT ? ERROR_WINHTTP_TIMEOUT : ERROR_OPERATION_ABORTED;
}
/* 启动一次异步接收并等待结果；调用者保持接收缓冲有效，且同一连接只有一个接收者。 */
static DWORD receive_raw(drop_ws *ws, void *buffer, DWORD capacity, DWORD *length, WINHTTP_WEB_SOCKET_BUFFER_TYPE *type, HANDLE cancel) {
    drop_ws_io *io = ws->io;
    DWORD error;
    if (!io) return ERROR_INVALID_HANDLE;
    EnterCriticalSection(&io->guard);
    if (io->closing) {
        LeaveCriticalSection(&io->guard);
        return ERROR_OPERATION_ABORTED;
    }
    ResetEvent(io->received);
    io->rx_error = 0;
    io->bytes = 0;
    error = WinHttpWebSocketReceive(io->socket, buffer, capacity, NULL, NULL);
    LeaveCriticalSection(&io->guard);
    if (error == NO_ERROR) error = wait_result(io, io->received, cancel, INFINITE);
    if (error == NO_ERROR) {
        error = io->rx_error;
        *length = io->bytes;
        *type = io->type;
    }
    return error;
}
/* 启动一次异步发送并等待结果或超时；缓冲保留到操作或关闭回调结束，同一连接只有一个发送者。 */
static DWORD send_raw(drop_ws *ws, const void *buffer, DWORD length, HANDLE cancel) {
    drop_ws_io *io = ws->io;
    DWORD error;
    if (!io) return ERROR_INVALID_HANDLE;
    EnterCriticalSection(&io->guard);
    if (io->closing) {
        LeaveCriticalSection(&io->guard);
        return ERROR_OPERATION_ABORTED;
    }
    ResetEvent(io->sent);
    io->tx_error = 0;
    error = WinHttpWebSocketSend(io->socket, ws->encrypted ? WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE : WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (void *)buffer, length);
    LeaveCriticalSection(&io->guard);
    if (error == NO_ERROR) error = wait_result(io, io->sent, cancel, 15000);
    return error == NO_ERROR ? io->tx_error : error;
}
/* 接收 TLS 文本或解密 PAKE 帧，要求加密帧序号严格连续；认证失败不交付内容。 */
DWORD drop_ws_receive(drop_ws *ws, void *buffer, DWORD capacity, DWORD *length, WINHTTP_WEB_SOCKET_BUFFER_TYPE *type, HANDLE cancel) {
    unsigned char *wire, *plain = NULL;
    DWORD size = 0, plain_size = 0, error;
    uint64_t sequence = 0;
    if (!ws->encrypted) return receive_raw(ws, buffer, capacity, length, type, cancel);
    if (capacity > DROP_MAX_JSON) return ERROR_INVALID_PARAMETER;
    wire = malloc((size_t)capacity + DROP_PAKE_FRAME_OVERHEAD);
    if (!wire) return ERROR_NOT_ENOUGH_MEMORY;
    error = receive_raw(ws, wire, capacity + DROP_PAKE_FRAME_OVERHEAD, &size, type, cancel);
    if (error == NO_ERROR && *type != WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
        if (*type != WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE || !drop_pake_decrypt(&ws->keys, DROP_PAKE_WS_RX, "ws-v1", wire, size, &sequence, &plain, &plain_size) ||
            !sequence || sequence != ws->io->rx_sequence + 1 || plain_size > capacity)
            error = ERROR_INVALID_DATA;
        else {
            memcpy(buffer, plain, plain_size);
            *length = plain_size;
            *type = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
            ws->io->rx_sequence = sequence;
        }
    } else if (error == NO_ERROR)
        *length = 0;
    drop_pake_bytes_free(wire, (size_t)capacity + DROP_PAKE_FRAME_OVERHEAD);
    drop_pake_bytes_free(plain, plain_size);
    return error;
}
/* 按连接模式发送文本或认证帧，递增独立发送序号；失败后由上层关闭并重建连接。 */
DWORD drop_ws_send(drop_ws *ws, const void *buffer, DWORD length, HANDLE cancel) {
    unsigned char *frame = NULL;
    DWORD size = 0, error;
    if (!ws->encrypted) return send_raw(ws, buffer, length, cancel);
    if (!ws->io || ws->io->tx_sequence == UINT64_MAX || !drop_pake_encrypt(&ws->keys, DROP_PAKE_WS_TX, ++ws->io->tx_sequence, "ws-v1", buffer, length, &frame, &size))
        return ERROR_INVALID_DATA;
    error = send_raw(ws, frame, size, cancel);
    drop_pake_bytes_free(frame, size);
    return error;
}
/* 中止 socket 并等待关闭回调，再释放上层 WinHTTP 句柄和拥有者引用，最后清零密钥状态。 */
void drop_ws_close(drop_ws *ws) {
    if (ws->io) {
        abort_socket(ws->io);
        release(ws->io);
    } else if (ws->socket)
        WinHttpCloseHandle(ws->socket);
    if (ws->connection) WinHttpCloseHandle(ws->connection);
    if (ws->session) WinHttpCloseHandle(ws->session);
    sodium_memzero(ws, sizeof(*ws));
}
