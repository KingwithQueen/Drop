#ifndef DROP_RUNTIME_H
#define DROP_RUNTIME_H
#include "ui_data.h"

typedef struct drop_runtime drop_runtime;
typedef enum {
    DROP_TASK_SYNC,
    DROP_TASK_VIEW,
    DROP_TASK_LOGIN,
    DROP_TASK_LOGOUT,
    DROP_TASK_SEND,
    DROP_TASK_FIND,
    DROP_TASK_READ,
    DROP_TASK_DELETE,
    DROP_TASK_EXPORT,
    DROP_TASK_REVIEW,
    DROP_TASK_PREFERENCES
} drop_task_kind;
/* 界面提交后转移全部字符串所有权；队列和密码由后台统一释放。view_serial 标识最新视图意图，不用于协议或数据库。 */
typedef struct drop_task {
    drop_task_kind kind;
    char peer[DROP_ID_CAP], message[DROP_ID_CAP], object[DROP_ID_CAP], account[DROP_ID_CAP];
    char *text, *url, *login, *password, *ca, *destination;
    char *paths[DROP_MAX_ATTACHMENTS];
    int count, page;
    int notify, attention;
    int64_t from, to;
    uint64_t view_serial;
    struct drop_task *next;
} drop_task;
/* 快照拥有历史，界面取走后必须释放；view_serial 回传已处理的视图意图，防止迟到快照覆盖新的用户选择。 */
typedef struct drop_snapshot {
    drop_history *history;
    char status[256], url[8193], login[65], ca[131072];
    int ready, busy;
    int notify, attention;
    uint64_t send_serial, send_finished;
    uint64_t view_serial;
    drop_error error;
} drop_snapshot;

/* 建立目录级界面独占锁并创建后台线程；root 为 NULL 时使用程序旁 Data/Drop。 */
drop_runtime *drop_runtime_start(const WCHAR *root);
/* 设置取消、唤醒线程并等待全部网络和文件工作结束，之后才释放事件及客户端。 */
void drop_runtime_stop(drop_runtime *runtime);
/* 有界非阻塞任务提交；成功转移所有权，失败时调用者仍负责 drop_task_free。 */
int drop_runtime_submit(drop_runtime *runtime, drop_task *task);
/* 取走最新快照，不阻塞工作线程；没有新快照时返回 NULL。 */
drop_snapshot *drop_runtime_take(drop_runtime *runtime);
void drop_snapshot_free(drop_snapshot *snapshot);
void drop_task_free(drop_task *task);
/* 各平台原生界面入口，界面必须在主线程运行。 */
int drop_native_ui(const WCHAR *root, int hidden);
/* 信号处理器只记录停止意图，由主线程在事件循环中进行正常关闭，避免信号上下文使用 GUI/锁。 */
int drop_process_stop_requested(void);
#endif
