#ifndef DROP_UI_DATA_H
#define DROP_UI_DATA_H
#include "client.h"
#define DROP_HISTORY_PAGE 50
typedef struct drop_detail drop_detail;
/* 本机历史列表的一行；拥有 content，附件名称和状态直接用于消息绘制。 */
typedef struct drop_history_row {
    char id[DROP_ID_CAP];
    int pending, unread, deletable;
    WCHAR time[32], direction[96], preview[320], files[48], state[96];
    WCHAR sender[160];
    int mine;
    drop_detail *content;
} drop_history_row;
/* 已有消息的参与者或明确查找过的账号，不枚举服务器其他账号。 */
typedef struct drop_peer {
    char id[DROP_ID_CAP];
    WCHAR name[160], login[65];
    int unread, available;
} drop_peer;
/* 一页历史及本机任务统计，new_notifications 来自持久去重；由后台构造并移交界面。 */
typedef struct drop_history {
    int count, more, unread, new_notifications, pending_count, downloads, restore_review, pending_deletes, deletion_supported;
    drop_history_row rows[DROP_HISTORY_PAGE];
    char self[DROP_ID_CAP], peer[DROP_ID_CAP];
    int peer_count;
    drop_peer *peers;
} drop_history;
/* 供界面展示和导出的附件元数据，complete 仍需在导出前结合长度及摘要复核。 */
typedef struct drop_file_view {
    char id[DROP_ID_CAP], sha[65];
    uint64_t total;
    int complete;
    WCHAR name[1025];
} drop_file_view;
/* 完整消息正文和有限附件列表；结构拥有 text，由 drop_ui_detail_free 统一释放。 */
struct drop_detail {
    WCHAR *text;
    int count;
    drop_file_view files[DROP_MAX_ATTACHMENTS];
};
/* 建立本机界面和通知去重元数据，迁移时保留已有历史。 */
int drop_client_ui_schema(drop_client *c);
/* 按账号、分页、正文和时间查询本机历史，同时持久去重通知；空账号表示全部，结果须用 drop_ui_history_free 释放。 */
drop_history *drop_ui_history(drop_client *c, int page, const char *search, int64_t from, int64_t to, const char *peer);
/* 释放每条消息拥有的正文与附件快照、参与者列表及历史结构。 */
void drop_ui_history_free(drop_history *history);
/* 缓存明确验证过的账号名，供用户列表和消息发送者显示；失败返回 0。 */
int drop_ui_contact(drop_client *c, const char *id, const char *login);
/* 读取消息完整正文及附件状态，支持待发送记录；返回结果由 drop_ui_detail_free 释放。 */
drop_detail *drop_ui_detail(drop_client *c, const char *id, int pending);
/* 释放详情正文和结构，允许传入空指针。 */
void drop_ui_detail_free(drop_detail *detail);
/* 更新本机未读状态并排队账号已读位置，使其他设备随后同步相同位置。 */
int drop_ui_mark_read(drop_client *c, const char *id);
/* 查询登录名对应的可发送账号 ID；空接收人使用当前账号，失败不构造发送目标。 */
drop_error drop_ui_recipient(drop_client *c, const char *login, char id[DROP_ID_CAP]);
/* 从本机完整副本导出附件，先验证长度和 SHA-256、刷新临时文件，再按用户明确选择替换目标。 */
drop_error drop_ui_export(drop_client *c, const drop_file_view *file, const WCHAR *destination);
/* 将原附件名转换为安全导出名，处理路径分隔符、保留名和非法尾部；仅用于展示及显式导出。 */
void drop_ui_safe_filename(const WCHAR *original, WCHAR safe[260]);
/* 按规范化数据目录保证单实例，初始化原生窗口、网络通知和后台线程，并运行消息循环及统一清理。 */
int drop_ui_run(HINSTANCE instance, const WCHAR *root, int hidden);
#endif
