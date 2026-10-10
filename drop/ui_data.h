#ifndef DROP_UI_DATA_H
#define DROP_UI_DATA_H
#include "client.h"
#define DROP_HISTORY_PAGE 50
typedef struct drop_history_row {
    char id[DROP_ID_CAP]; int pending,unread;
    WCHAR time[32],direction[96],preview[320],files[48],state[96];
} drop_history_row;
typedef struct drop_history {
    int count,more,unread,new_notifications,pending_count,downloads,restore_review;
    drop_history_row rows[DROP_HISTORY_PAGE];
} drop_history;
typedef struct drop_file_view {
    char id[DROP_ID_CAP],sha[65]; uint64_t total; int complete;
    WCHAR name[1025];
} drop_file_view;
typedef struct drop_detail {
    WCHAR *text; int count; drop_file_view files[DROP_MAX_ATTACHMENTS];
} drop_detail;
int drop_client_ui_schema(drop_client *c);
drop_history *drop_ui_history(drop_client *c,int page,const char *search,int64_t from,int64_t to);
drop_detail *drop_ui_detail(drop_client *c,const char *id,int pending);
void drop_ui_detail_free(drop_detail *detail);
int drop_ui_mark_read(drop_client *c,const char *id);
drop_error drop_ui_recipient(drop_client *c,const char *login,char id[DROP_ID_CAP]);
drop_error drop_ui_export(drop_client *c,const drop_file_view *file,const WCHAR *destination);
void drop_ui_safe_filename(const WCHAR *original,WCHAR safe[260]);
int drop_ui_run(HINSTANCE instance,const WCHAR *root,int hidden);
#endif
