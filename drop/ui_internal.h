#ifndef DROP_UI_INTERNAL_H
#define DROP_UI_INTERNAL_H
#include "ui_data.h"
#include <commctrl.h>
#include <shellapi.h>
#define DROP_UI_RESULT (WM_APP + 1)
#define DROP_UI_TRAY (WM_APP + 2)
#define DROP_UI_SHOW (WM_APP + 3)
#define DROP_UI_VISIBILITY (WM_APP + 4)
/* 消息列表以行号通知选择；只在界面线程传递，不发布裸附件指针。 */
#define DROP_UI_SELECT_MESSAGE (WM_APP + 6)
/* 图标后台只移交独立结果，不借用界面附件路径；旧代次结果由界面释放。 */
#define DROP_UI_DRAFT_ICONS (WM_APP + 7)
#if DROP_DIAGNOSTICS
/* 仅隔离测试构建接收指定 DPI；消息只携带整数，避免跨进程传递 RECT 指针。 */
#define DROP_UI_TEST_DPI (WM_APP + 5)
#endif
enum {
    UI_URL = 100,
    UI_CA,
    UI_LOGIN,
    UI_PASSWORD,
    UI_PROBE,
    UI_SIGNIN,
    UI_RECIPIENT,
    UI_BODY,
    UI_ATTACH,
    UI_SEND,
    UI_CLEAR,
    UI_SYNC,
    UI_NOTIFICATIONS,
    UI_AUTOSTART,
    UI_SEARCH,
    UI_PREVIOUS,
    UI_NEXT,
    UI_HISTORY,
    UI_DETAIL,
    UI_FILES,
    UI_EXPORT,
    UI_STATUS,
    UI_FROM,
    UI_TO,
    UI_FIND,
    UI_LOGOUT,
    UI_EXIT,
    UI_COMPRESSION,
    UI_LEVEL,
    UI_RESTORE_RETRY,
    UI_TRAY_FLASH,
    UI_CONFIGURATION,
    UI_SAVE_CONFIG,
    UI_CONFIG_CLOSE,
    UI_CONFIG_STATUS,
    UI_OVERVIEW,
    UI_CONNECTION_LABEL,
    UI_URL_LABEL,
    UI_CA_LABEL,
    UI_LOGIN_LABEL,
    UI_PASSWORD_LABEL,
    UI_PREFERENCES_LABEL,
    UI_CONFIG_HINT,
    UI_HISTORY_LABEL,
    UI_DETAIL_LABEL,
    UI_USERS,
    UI_USER_SEARCH,
    UI_ADD_USER,
    UI_RESET_FILTER,
    UI_SPLITTER,
    UI_DRAFT_FILES,
    UI_DRAFT_LABEL,
    UI_REMOVE_ATTACH,
    UI_SEND_HINT,
    UI_DELETE_MESSAGE,
    UI_VIEW_MESSAGE,
    UI_COUNT
};
enum { DROP_UI_BACKGROUND, DROP_UI_SURFACE, DROP_UI_PANEL, DROP_UI_TINT, DROP_UI_TEXT, DROP_UI_MUTED, DROP_UI_ACCENT, DROP_UI_LINE, DROP_UI_ON_ACCENT, DROP_UI_COLOR_COUNT };
typedef struct ui_icon_job ui_icon_job;
/* 界面提交给工作线程的任务种类；任务内容按类型解释，不在界面线程执行网络或数据库操作。 */
typedef enum ui_job_type {
    JOB_NONE,
    JOB_LOGIN,
    JOB_PROBE,
    JOB_SEND,
    JOB_VIEW,
    JOB_DETAIL,
    JOB_EXPORT,
    JOB_LOGOUT,
    JOB_SETTINGS,
    JOB_RESTORE_RETRY,
    JOB_CONTACT,
    JOB_DELETE
} ui_job_type;
/* 界面构造的任务快照，提交成功后归工作线程；拥有密码、正文和附件路径，释放时清零密码。 */
typedef struct ui_job {
    /* 仅阅读任务使用有界链表；每条已显示消息的已读意图独立保留，不覆盖其他会话。 */
    struct ui_job *next;
    ui_job_type type;
    WCHAR url[2049], ca[32768], destination[32768];
    char login[65], recipient[65], id[DROP_ID_CAP], search[1025];
    char peer[DROP_ID_CAP];
    char *password, *body;
    int count, page, pending, selection;
    int64_t from, to;
    int notifications, tray_flash, autostart, use_zstd, zstd_level, previous_autostart;
    WCHAR *paths[DROP_MAX_ATTACHMENTS];
    drop_file_view file;
} ui_job;
/* 工作线程经窗口消息移交的结果；界面可接管 history 和 detail，其余内容由结果释放函数清理。 */
typedef struct ui_result {
    ui_job_type type;
    int ready, queued, selection, task_done, delete_queued;
    drop_error error;
    WCHAR status[256];
    drop_history *history;
    drop_detail *detail;
    int settings_saved, notifications, tray_flash, autostart, use_zstd, zstd_level;
    DWORD system_error;
} ui_result;
/* 事件连接及心跳线程共用的上下文；自身 stop 控制连接生命周期，app_stop 借用主程序退出事件。 */
typedef struct ui_ws_context {
    HANDLE stop, thread, timer, reconnect;
    /* 保护共享连接或任务指针及关联状态，耗时操作尽量在取得稳定引用后移到锁外。 */
    CRITICAL_SECTION lock;
    drop_ws ws;
    WCHAR url[2049], ca[32768];
    char token[DROP_TOKEN_CAP];
    HANDLE wake, app_stop;
    /* 锁内维护的单调时钟时间，用于回应期限和下一次心跳调度。 */
    ULONGLONG last_received, next_heartbeat;
    unsigned heartbeat_seconds, timeout_seconds;
} ui_ws_context;
/* 主界面状态；控件和显示数据归界面线程，job 与 ws 的跨线程发布和取消受 lock 保护。 */
typedef struct drop_ui {
    HINSTANCE instance;
    HWND window, configuration, document, controls[UI_COUNT - UI_URL];
    /* 顶层窗口分别拥有字体和 DPI，主窗口与配置窗口另有独立滚动位置，允许不同屏幕缩放。 */
    HFONT font, configuration_font, document_font;
    UINT dpi, configuration_dpi, document_dpi;
    int scroll, configuration_scroll;
    WCHAR root[32768], class_name[80], url[2049], ca[32768], login[65];
    HANDLE singleton, stop, wake, worker, work_pending;
    /* 保护共享连接或任务指针及关联状态，耗时操作尽量在取得稳定引用后移到锁外。 */
    CRITICAL_SECTION lock;
    /* 锁内发布的单个待执行任务，由工作线程取走并最终释放。 */
    ui_job *job;
    /* 锁内维护最多 64 条阅读任务；工作线程优先取出，普通操作仍使用原单任务槽。 */
    ui_job *reads, *last_read;
    int read_count;
    int notifications, tray_flash, autostart, ready, busy, page, more, selection, tray_added, hidden, sending;
    HICON tray_icon, tray_blank_icon;
    int tray_flashing, tray_blank, taskbar_flashing, balloon_pending;
    int use_zstd, zstd_level;
    char search[1025];
    int64_t from, to;
    int file_count;
    WCHAR *paths[DROP_MAX_ATTACHMENTS];
    /* 图标线程只处理副本，锁内替换尚未执行的请求；图像列表及配色画刷归界面所有。 */
    HANDLE icon_event, icon_worker;
    ui_icon_job *icon_job;
    unsigned icon_generation;
    HIMAGELIST draft_icons;
    COLORREF colors[DROP_UI_COLOR_COUNT];
    HBRUSH brushes[DROP_UI_COLOR_COUNT];
    drop_history *history;
    drop_detail *detail;
    UINT taskbar_created;
    HMODULE message_controls;
    int divider, dragging, rebuilding, attachment_selection;
    char peer[DROP_ID_CAP], recipient_id[DROP_ID_CAP], selected_id[DROP_ID_CAP];
    /* RichEdit 字符范围用于关联消息和附件点击；重建后全部重新计算，不跨线程使用。 */
    LONG message_start[DROP_HISTORY_PAGE], message_end[DROP_HISTORY_PAGE];
    /* 保存实际绘制的身份及正文起点，状态标题长度变化或新增消息时仍按原消息恢复选中文字。 */
    char rendered_ids[DROP_HISTORY_PAGE][DROP_ID_CAP];
    LONG message_body[DROP_HISTORY_PAGE];
    LONG attachment_start[DROP_HISTORY_PAGE][DROP_MAX_ATTACHMENTS], attachment_end[DROP_HISTORY_PAGE][DROP_MAX_ATTACHMENTS];
    /* 锁内发布的连接上下文，工作线程拥有并在取消及回调结束后销毁。 */
    ui_ws_context *ws;
    HANDLE network_notification;
} drop_ui;
/* 独占本机同步核心，处理界面任务及定时补拉；发送失败仍拉取新正文，退出前关闭事件连接并释放任务。 */
DWORD WINAPI drop_ui_worker(void *context);
/* 释放任务的正文、密码和路径；密码先清零，任务移交后由工作线程调用。 */
void drop_ui_job_free(ui_job *job);
/* 释放尚未被界面接管的历史和详情，再释放结果结构。 */
void drop_ui_result_free(ui_result *result);
/* 读取本机公开设置并应用默认值，兼容旧提醒字段；不从普通设置文件读取密码或令牌。 */
int drop_ui_settings_load(drop_ui *ui);
/* 将服务器地址、提醒和压缩等公开设置写入本机文件；秘密材料另由 DPAPI 管理。 */
int drop_ui_settings_save(drop_ui *ui);
/* 仅修改本数据目录实例对应的用户启动项，启用命令带后台参数；不触碰其他应用启动配置。 */
int drop_ui_autostart(drop_ui *ui, int enabled);
/* 向事件连接发出可并发处理的取消信号，供退出、设置或网络恢复路径调用。 */
void drop_ui_cancel_ws(drop_ui *ui);
/* 网络或休眠恢复后打断退避并唤醒同步，使持久增量及时补齐。 */
void drop_ui_resume(drop_ui *ui);
/* 判断控件是否属于配置窗口，避免将连接和账号内容混入日常消息窗口。 */
int drop_ui_configuration_control(int id);
/* 创建由主窗口拥有的独立配置窗口；控件由界面初始化阶段统一建立。 */
int drop_ui_configuration_create(drop_ui *ui);
/* 显示配置窗口或关闭并清除尚未提交的密码；关闭配置不会停止同步。 */
void drop_ui_configuration_show(drop_ui *ui, int show);
/* 按指定 DPI 创建新字体并直接应用于原生控件，旧字体在所有控件切换后释放。 */
void drop_ui_dpi(drop_ui *ui, HWND window, UINT dpi);
int drop_ui_scale(UINT dpi, int value);
/* 按逻辑坐标定位控件，短屏上的垂直偏移由所属窗口的滚动状态决定。 */
void drop_ui_move(drop_ui *ui, int id, int x, int y, int width, int height);
/* 设置滚动范围并返回布局使用的逻辑尺寸，小屏幕保留全部操作的可达性。 */
void drop_ui_viewport(drop_ui *ui, HWND window, int minimum_height, int *width, int *height);
/* 响应垂直滚动或滚轮，只移动当前窗口中的控件，不滚动另一个窗口。 */
void drop_ui_scroll(drop_ui *ui, HWND window, UINT message, WPARAM wparam);
/* 完整正文只在明确查看时显示；窗口有独立 DPI，关闭隐藏且不影响主窗口和同步。 */
int drop_ui_document_create(drop_ui *ui);
void drop_ui_document_show(drop_ui *ui);
/* 按屏幕工作区限制初始窗口与最小跟踪尺寸，避免高缩放下超出桌面。 */
void drop_ui_place(drop_ui *ui, HWND window, int width, int height);
void drop_ui_minimum(drop_ui *ui, HWND window, MINMAXINFO *info, int width, int height);
/* 在界面线程重建原生消息文本，逐个显示附件名字、大小及保存状态，保留阅读位置。 */
void drop_ui_messages(drop_ui *ui);
/* 更新本机参与者列表；只展示自己、历史参与者和明确查找的账号。 */
void drop_ui_users(drop_ui *ui);
/* 为消息控件安装键盘与附件命中处理；不自动打开收到的附件。 */
void drop_ui_message_control(drop_ui *ui);
/* 将所选消息附件填入原生表格；下标按同一详情快照解释。 */
void drop_ui_files(drop_ui *ui);
/* 初始化文件图标后台及输入框的文件粘贴、拖入处理，普通文字仍交给原生编辑框。 */
void drop_ui_compose_create(drop_ui *ui);
/* 停止并回收图标后台及结果后销毁图像列表；超时返回失败，调用者按既有退出策略处理。 */
int drop_ui_compose_close(drop_ui *ui);
/* 校验并持有独立路径副本，拒绝目录、重复路径及超限文件；不读取附件内容。 */
int drop_ui_draft_add(drop_ui *ui, const WCHAR *path);
/* 更新待发送附件的原始名称、大小和图标，重新布局；未发送的路径只归界面所有。 */
void drop_ui_draft_refresh(drop_ui *ui);
/* 移除当前选中草稿附件，释放路径并保持其余附件；发送时不允许修改。 */
void drop_ui_draft_remove(drop_ui *ui);
/* 消费拖入文件，刷新草稿并释放拖放句柄；剪贴板借用句柄不调用本函数。 */
void drop_ui_draft_drop(drop_ui *ui, HDROP files);
/* 将图标结果应用到仍匹配的草稿，所有结果无论是否过期都在本函数释放。 */
void drop_ui_draft_icons(drop_ui *ui, void *result);
/* 重建本实例的蓝灰配色；高对比度使用系统颜色，不修改系统主题。 */
void drop_ui_theme(drop_ui *ui);
/* 释放本实例配色画刷，所有窗口销毁后调用。 */
void drop_ui_theme_close(drop_ui *ui);
/* 为标准控件提供文字和背景色，画刷仍归界面所有。 */
LRESULT drop_ui_control_color(drop_ui *ui, HDC dc, HWND control);
/* 绘制原生发送按钮及其禁用、按下和焦点状态，其余按钮保留系统绘制。 */
int drop_ui_draw_button(drop_ui *ui, const DRAWITEMSTRUCT *draw);
#if DROP_DIAGNOSTICS
/* 在窗口线程中构造建议矩形并调用真实 DPI 消息路径，不修改 Windows 的屏幕缩放设置。 */
void drop_ui_test_dpi(HWND window, UINT dpi, int width, int height);
#endif
#endif
