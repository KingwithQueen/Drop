#ifndef DROP_UI_INTERNAL_H
#define DROP_UI_INTERNAL_H
#include "ui_data.h"
#include <commctrl.h>
#include <shellapi.h>
#define DROP_UI_RESULT (WM_APP+1)
#define DROP_UI_TRAY (WM_APP+2)
#define DROP_UI_SHOW (WM_APP+3)
enum { UI_URL=100,UI_CA,UI_LOGIN,UI_PASSWORD,UI_PROBE,UI_SIGNIN,UI_RECIPIENT,UI_BODY,UI_ATTACH,UI_SEND,UI_CLEAR,
    UI_SYNC,UI_NOTIFICATIONS,UI_AUTOSTART,UI_SEARCH,UI_PREVIOUS,UI_NEXT,UI_HISTORY,UI_DETAIL,UI_FILES,UI_EXPORT,UI_STATUS,
    UI_FROM,UI_TO,UI_FIND,UI_LOGOUT,UI_EXIT,UI_COMPRESSION,UI_LEVEL,UI_RESTORE_RETRY };
typedef enum ui_job_type { JOB_NONE,JOB_LOGIN,JOB_PROBE,JOB_SEND,JOB_VIEW,JOB_DETAIL,JOB_EXPORT,JOB_LOGOUT,JOB_SETTINGS,JOB_RESTORE_RETRY } ui_job_type;
typedef struct ui_job {
    ui_job_type type; WCHAR url[2049],ca[32768],destination[32768];
    char login[65],recipient[65],id[DROP_ID_CAP],search[1025]; char *password,*body;
    int count,page,pending,selection; int64_t from,to;
    int notifications,autostart,use_zstd,zstd_level,previous_autostart;
    WCHAR *paths[DROP_MAX_ATTACHMENTS]; drop_file_view file;
} ui_job;
typedef struct ui_result {
    ui_job_type type; int ready,queued,selection,task_done; drop_error error;
    WCHAR status[256]; drop_history *history; drop_detail *detail;
    int settings_saved,notifications,autostart,use_zstd,zstd_level;
    DWORD system_error;
} ui_result;
typedef struct ui_ws_context {
    HANDLE stop,thread,timer,reconnect; CRITICAL_SECTION lock; drop_ws ws;
    WCHAR url[2049],ca[32768]; char token[DROP_TOKEN_CAP];
    HANDLE wake,app_stop;
    ULONGLONG last_received,next_heartbeat;
    unsigned heartbeat_seconds,timeout_seconds;
} ui_ws_context;
typedef struct drop_ui {
    HINSTANCE instance; HWND window,controls[UI_RESTORE_RETRY-UI_URL+1]; HFONT font;
    WCHAR root[32768],class_name[80],url[2049],ca[32768],login[65];
    HANDLE singleton,stop,wake,worker; CRITICAL_SECTION lock; ui_job *job;
    int notifications,autostart,ready,busy,page,more,selection,tray_added,hidden,sending;
    int use_zstd,zstd_level;
    char search[1025]; int64_t from,to;
    int file_count; WCHAR *paths[DROP_MAX_ATTACHMENTS];
    drop_history *history; drop_detail *detail; UINT taskbar_created;
    ui_ws_context *ws;
    HANDLE network_notification;
} drop_ui;
DWORD WINAPI drop_ui_worker(void *context);
void drop_ui_job_free(ui_job *job);
void drop_ui_result_free(ui_result *result);
int drop_ui_settings_load(drop_ui *ui);
int drop_ui_settings_save(drop_ui *ui);
int drop_ui_autostart(drop_ui *ui,int enabled);
void drop_ui_cancel_ws(drop_ui *ui);
void drop_ui_resume(drop_ui *ui);
#endif
