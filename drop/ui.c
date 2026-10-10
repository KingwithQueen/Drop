#include <winsock2.h>
#include <ws2tcpip.h>
#include "ui_internal.h"
#include <iphlpapi.h>
#include <commdlg.h>
#include <shlobj.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CONTROL(ui,id) ((ui)->controls[(id)-UI_URL])
#define TRAY_OPEN 1001
#define TRAY_EXIT 1002

static void status(drop_ui *ui,const WCHAR *text) { SetWindowTextW(CONTROL(ui,UI_STATUS),text); }
static HWND control(drop_ui *ui,int id,const WCHAR *kind,const WCHAR *text,DWORD style) {
    HWND window=CreateWindowExW(!wcscmp(kind,L"EDIT")?WS_EX_CLIENTEDGE:0,kind,text,WS_CHILD|WS_VISIBLE|style,
        0,0,0,0,ui->window,(HMENU)(INT_PTR)id,ui->instance,NULL);
    if(id>=UI_URL && id<=UI_RESTORE_RETRY) CONTROL(ui,id)=window;
    SendMessageW(window,WM_SETFONT,(WPARAM)ui->font,TRUE); return window;
}
static void move(drop_ui *ui,int id,int x,int y,int width,int height) {
    MoveWindow(CONTROL(ui,id),x,y,width>0?width:1,height>0?height:1,TRUE);
}
static void hint(HWND edit,const WCHAR *text) { SendMessageW(edit,EM_SETCUEBANNER,FALSE,(LPARAM)text); }
static void enable(drop_ui *ui) {
    EnableWindow(CONTROL(ui,UI_SIGNIN),!ui->busy); EnableWindow(CONTROL(ui,UI_PROBE),!ui->busy);
    EnableWindow(CONTROL(ui,UI_SEND),ui->ready && !ui->busy); EnableWindow(CONTROL(ui,UI_SYNC),ui->ready && !ui->busy);
    EnableWindow(CONTROL(ui,UI_LOGOUT),ui->ready && !ui->busy);
    EnableWindow(CONTROL(ui,UI_RESTORE_RETRY),ui->ready && !ui->busy && ui->history && ui->history->restore_review>0);
    EnableWindow(CONTROL(ui,UI_FIND),ui->ready && !ui->busy);
    EnableWindow(CONTROL(ui,UI_PREVIOUS),ui->ready && !ui->busy && ui->page>0);
    EnableWindow(CONTROL(ui,UI_NEXT),ui->ready && !ui->busy && ui->more);
    EnableWindow(CONTROL(ui,UI_EXPORT),ui->ready && !ui->busy && ui->detail && ui->detail->count>0);
    EnableWindow(CONTROL(ui,UI_BODY),!ui->sending); EnableWindow(CONTROL(ui,UI_RECIPIENT),!ui->sending);
    EnableWindow(CONTROL(ui,UI_ATTACH),!ui->sending); EnableWindow(CONTROL(ui,UI_CLEAR),!ui->sending);
    EnableWindow(CONTROL(ui,UI_NOTIFICATIONS),!ui->busy); EnableWindow(CONTROL(ui,UI_AUTOSTART),!ui->busy);
    EnableWindow(CONTROL(ui,UI_COMPRESSION),!ui->busy); EnableWindow(CONTROL(ui,UI_LEVEL),!ui->busy && ui->use_zstd);
}
static void layout(drop_ui *ui,int width,int height) {
    int left=16,right=width-16,w=right-left,half=w/2,history_height=height-476;
    if(history_height<110) history_height=110;
    move(ui,UI_URL,left,16,w-220,28); move(ui,UI_PROBE,right-210,16,100,28); move(ui,UI_SIGNIN,right-100,16,100,28);
    move(ui,UI_CA,left,52,w-110,28); move(ui,UI_LOGOUT,right-100,52,100,28);
    move(ui,UI_LOGIN,left,88,half-8,28); move(ui,UI_PASSWORD,left+half,88,half,28);
    move(ui,UI_NOTIFICATIONS,left,124,150,24); move(ui,UI_AUTOSTART,left+160,124,240,24);
    move(ui,UI_COMPRESSION,left+420,122,210,150); move(ui,UI_LEVEL,left+640,122,160,400);
    move(ui,UI_RECIPIENT,left,156,w,28); move(ui,UI_BODY,left,192,w,84);
    move(ui,UI_ATTACH,left,284,160,28); move(ui,UI_CLEAR,left+170,284,90,28);
    move(ui,UI_SEND,right-200,284,90,28); move(ui,UI_SYNC,right-100,284,100,28);
    move(ui,UI_RESTORE_RETRY,left+270,284,220,28);
    move(ui,UI_SEARCH,left,320,w-560,28); move(ui,UI_FROM,right-550,320,160,28); move(ui,UI_TO,right-380,320,160,28);
    move(ui,UI_FIND,right-210,320,70,28); move(ui,UI_PREVIOUS,right-130,320,60,28); move(ui,UI_NEXT,right-60,320,60,28);
    move(ui,UI_HISTORY,left,356,w,history_height);
    move(ui,UI_DETAIL,left,364+history_height,w-300,72); move(ui,UI_FILES,right-290,364+history_height,290,200);
    move(ui,UI_EXPORT,right-290,402+history_height,120,28); move(ui,UI_EXIT,right-160,402+history_height,160,28);
    move(ui,UI_STATUS,left,height-28,w,22);
}
static void create_controls(drop_ui *ui) {
    HWND edit; int i; LVCOLUMNW column={0};
    const WCHAR *titles[]={L"时间",L"方向 / 账号",L"正文",L"附件",L"保存 / 已读状态"}; int widths[]={135,165,300,55,430};
    control(ui,UI_URL,L"EDIT",ui->url,ES_AUTOHSCROLL|WS_TABSTOP); hint(CONTROL(ui,UI_URL),L"服务器地址与端口，例如 https://example.com:8443");
    control(ui,UI_CA,L"EDIT",ui->ca,ES_AUTOHSCROLL|WS_TABSTOP); hint(CONTROL(ui,UI_CA),L"可选：明确指定的私有 CA PEM 文件路径（留空使用 Windows 信任）");
    control(ui,UI_LOGIN,L"EDIT",ui->login,ES_AUTOHSCROLL|WS_TABSTOP); hint(CONTROL(ui,UI_LOGIN),L"Drop 登录名");
    control(ui,UI_PASSWORD,L"EDIT",L"",ES_PASSWORD|ES_AUTOHSCROLL|WS_TABSTOP); hint(CONTROL(ui,UI_PASSWORD),L"密码（仅本次登录，不保存）");
    SendMessageW(CONTROL(ui,UI_LOGIN),EM_SETLIMITTEXT,64,0); SendMessageW(CONTROL(ui,UI_PASSWORD),EM_SETLIMITTEXT,1024,0);
    control(ui,UI_PROBE,L"BUTTON",L"测试连接",WS_TABSTOP); control(ui,UI_SIGNIN,L"BUTTON",L"登录",WS_TABSTOP);
    control(ui,UI_LOGOUT,L"BUTTON",L"退出账号",WS_TABSTOP);
    control(ui,UI_NOTIFICATIONS,L"BUTTON",L"新消息提醒",BS_AUTOCHECKBOX|WS_TABSTOP);
    control(ui,UI_AUTOSTART,L"BUTTON",L"登录 Windows 后自动启动",BS_AUTOCHECKBOX|WS_TABSTOP);
    SendMessageW(CONTROL(ui,UI_NOTIFICATIONS),BM_SETCHECK,ui->notifications?BST_CHECKED:BST_UNCHECKED,0);
    SendMessageW(CONTROL(ui,UI_AUTOSTART),BM_SETCHECK,ui->autostart?BST_CHECKED:BST_UNCHECKED,0);
    control(ui,UI_COMPRESSION,WC_COMBOBOXW,L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP);
    SendMessageW(CONTROL(ui,UI_COMPRESSION),CB_ADDSTRING,0,(LPARAM)L"正文存储：不压缩");
    SendMessageW(CONTROL(ui,UI_COMPRESSION),CB_ADDSTRING,0,(LPARAM)L"正文存储：Zstandard");
    SendMessageW(CONTROL(ui,UI_COMPRESSION),CB_SETCURSEL,(WPARAM)ui->use_zstd,0);
    control(ui,UI_LEVEL,WC_COMBOBOXW,L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP);
    for(i=1;i<=19;++i) { WCHAR text[32]; swprintf_s(text,32,L"压缩级别 %d",i); SendMessageW(CONTROL(ui,UI_LEVEL),CB_ADDSTRING,0,(LPARAM)text); }
    SendMessageW(CONTROL(ui,UI_LEVEL),CB_SETCURSEL,(WPARAM)(ui->zstd_level-1),0);
    control(ui,UI_RECIPIENT,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP); hint(CONTROL(ui,UI_RECIPIENT),L"收件人登录名，留空发给自己（所有已授权设备）");
    SendMessageW(CONTROL(ui,UI_RECIPIENT),EM_SETLIMITTEXT,64,0);
    edit=control(ui,UI_BODY,L"EDIT",L"",ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN|WS_VSCROLL|WS_TABSTOP);
    SendMessageW(edit,EM_SETLIMITTEXT,DROP_MAX_BODY,0); hint(edit,L"正文，可同时选择多个附件；收到文件后不会自动运行");
    control(ui,UI_ATTACH,L"BUTTON",L"添加附件（0）",WS_TABSTOP); control(ui,UI_CLEAR,L"BUTTON",L"清除附件",WS_TABSTOP);
    control(ui,UI_SEND,L"BUTTON",L"发送",BS_DEFPUSHBUTTON|WS_TABSTOP); control(ui,UI_SYNC,L"BUTTON",L"同步 / 重试",WS_TABSTOP);
    control(ui,UI_RESTORE_RETRY,L"BUTTON",L"重新提交恢复后待核对任务",WS_TABSTOP);
    control(ui,UI_SEARCH,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP); hint(CONTROL(ui,UI_SEARCH),L"查询本机正文（逐字匹配）");
    SendMessageW(CONTROL(ui,UI_SEARCH),EM_SETLIMITTEXT,256,0);
    control(ui,UI_FROM,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP); hint(CONTROL(ui,UI_FROM),L"起日 YYYY-MM-DD");
    control(ui,UI_TO,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP); hint(CONTROL(ui,UI_TO),L"止日 YYYY-MM-DD");
    SendMessageW(CONTROL(ui,UI_FROM),EM_SETLIMITTEXT,10,0); SendMessageW(CONTROL(ui,UI_TO),EM_SETLIMITTEXT,10,0);
    control(ui,UI_FIND,L"BUTTON",L"查询",WS_TABSTOP); control(ui,UI_PREVIOUS,L"BUTTON",L"上一页",WS_TABSTOP); control(ui,UI_NEXT,L"BUTTON",L"下一页",WS_TABSTOP);
    control(ui,UI_HISTORY,WC_LISTVIEWW,L"",LVS_REPORT|LVS_SINGLESEL|LVS_SHOWSELALWAYS|WS_TABSTOP|WS_BORDER);
    ListView_SetExtendedListViewStyle(CONTROL(ui,UI_HISTORY),LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER);
    column.mask=LVCF_TEXT|LVCF_WIDTH; for(i=0;i<5;++i) { column.pszText=(LPWSTR)titles[i]; column.cx=widths[i]; ListView_InsertColumn(CONTROL(ui,UI_HISTORY),i,&column); }
    control(ui,UI_DETAIL,L"EDIT",L"选择历史记录查看完整正文。关闭窗口后仍在托盘同步。",ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|WS_VSCROLL|WS_TABSTOP);
    SendMessageW(CONTROL(ui,UI_DETAIL),EM_SETLIMITTEXT,DROP_MAX_BODY,0);
    control(ui,UI_FILES,WC_COMBOBOXW,L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP);
    control(ui,UI_EXPORT,L"BUTTON",L"导出所选附件",WS_TABSTOP); control(ui,UI_EXIT,L"BUTTON",L"真正退出（停止同步）",WS_TABSTOP);
    control(ui,UI_STATUS,L"STATIC",L"正在读取本机状态…",SS_LEFT); enable(ui);
}
static void files_clear(drop_ui *ui) {
    int i; for(i=0;i<ui->file_count;++i) free(ui->paths[i]); ui->file_count=0;
    SetWindowTextW(CONTROL(ui,UI_ATTACH),L"添加附件（0）");
}
static void files_choose(drop_ui *ui) {
    OPENFILENAMEW dialog={0}; WCHAR *buffer=calloc(65536,sizeof(WCHAR)),text[64]; const WCHAR *next; int count=ui->file_count;
    if(!buffer) return;
    dialog.lStructSize=sizeof(dialog); dialog.hwndOwner=ui->window; dialog.lpstrFile=buffer; dialog.nMaxFile=65536;
    dialog.lpstrFilter=L"所有文件\0*.*\0\0"; dialog.Flags=OFN_EXPLORER|OFN_ALLOWMULTISELECT|OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if(GetOpenFileNameW(&dialog)) {
        next=buffer+wcslen(buffer)+1;
        if(!*next) {
            if(count<DROP_MAX_ATTACHMENTS) ui->paths[count++]=_wcsdup(buffer);
        } else while(*next && count<DROP_MAX_ATTACHMENTS) {
            WCHAR *path=malloc(32768*sizeof(WCHAR));
            if(!path || !drop_path_join(buffer,next,path,32768)) { free(path); break; }
            ui->paths[count++]=path; next+=wcslen(next)+1;
        }
        if(count && !ui->paths[count-1]) --count;
        ui->file_count=count; swprintf_s(text,64,L"添加附件（%d）",count); SetWindowTextW(CONTROL(ui,UI_ATTACH),text);
        status(ui,count==DROP_MAX_ATTACHMENTS?L"已选择 32 个附件；其它文件未加入。":L"附件已选择。点击发送后先保存独立副本，再进入发送队列。");
    } free(buffer);
}
static char *edit_utf8(drop_ui *ui,int id,size_t max) {
    int length=GetWindowTextLengthW(CONTROL(ui,id)); WCHAR *wide; char *text;
    if(length<0 || (size_t)length>max) return NULL;
    wide=calloc((size_t)length+1,sizeof(WCHAR)); text=malloc(max+1);
    if(!wide || !text) { free(wide); free(text); return NULL; }
    GetWindowTextW(CONTROL(ui,id),wide,length+1);
    if(!drop_to_utf8(wide,text,max+1)) { free(text); text=NULL; }
    if(id==UI_PASSWORD) sodium_memzero(wide,((size_t)length+1)*sizeof(WCHAR)); free(wide); return text;
}
static ui_job *new_job(ui_job_type type) { ui_job *job=calloc(1,sizeof(*job)); if(job) { job->type=type; job->to=INT64_MAX; } return job; }
static int submit(drop_ui *ui,ui_job *job) {
    int ok=0; if(!job) { status(ui,L"内存不足。请重试。"); return 0; }
    EnterCriticalSection(&ui->lock);
    if(!ui->job && !ui->busy) { ui->job=job; ui->busy=1; ok=1; }
    LeaveCriticalSection(&ui->lock);
    if(ok) { enable(ui); SetEvent(ui->wake); status(ui,L"后台正在处理，窗口可继续操作…"); }
    else { drop_ui_job_free(job); status(ui,L"后台任务尚未完成，请稍后重试。"); } return ok;
}
static void credentials(drop_ui *ui,ui_job_type type) {
    ui_job *job=new_job(type); char *login;
    if(!job) return;
    GetWindowTextW(CONTROL(ui,UI_URL),job->url,2049); GetWindowTextW(CONTROL(ui,UI_CA),job->ca,32768);
    wcscpy_s(ui->url,2049,job->url); wcscpy_s(ui->ca,32768,job->ca); GetWindowTextW(CONTROL(ui,UI_LOGIN),ui->login,65);
    job->notifications=ui->notifications; job->autostart=ui->autostart; job->use_zstd=ui->use_zstd; job->zstd_level=ui->zstd_level;
    login=edit_utf8(ui,UI_LOGIN,64);
    if(!login) { drop_ui_job_free(job); return; } strcpy_s(job->login,sizeof(job->login),login); free(login);
    if(type==JOB_LOGIN) {
        job->password=edit_utf8(ui,UI_PASSWORD,1024); SetWindowTextW(CONTROL(ui,UI_PASSWORD),L"");
        if(!*job->login || !job->password || !*job->password) { drop_ui_job_free(job); status(ui,L"请填写登录名和密码。"); return; }
    } (void)submit(ui,job);
}
static void settings_save(drop_ui *ui) {
    ui_job *job=new_job(JOB_SETTINGS); char *login;
    if(!job) return;
    GetWindowTextW(CONTROL(ui,UI_URL),job->url,2049); GetWindowTextW(CONTROL(ui,UI_CA),job->ca,32768);
    login=edit_utf8(ui,UI_LOGIN,64); if(!login) { drop_ui_job_free(job); return; }
    strcpy_s(job->login,sizeof(job->login),login); free(login);
    job->notifications=SendMessageW(CONTROL(ui,UI_NOTIFICATIONS),BM_GETCHECK,0,0)==BST_CHECKED;
    job->autostart=SendMessageW(CONTROL(ui,UI_AUTOSTART),BM_GETCHECK,0,0)==BST_CHECKED;
    job->previous_autostart=ui->autostart;
    job->use_zstd=SendMessageW(CONTROL(ui,UI_COMPRESSION),CB_GETCURSEL,0,0)==1;
    job->zstd_level=(int)SendMessageW(CONTROL(ui,UI_LEVEL),CB_GETCURSEL,0,0)+1;
    if(!submit(ui,job)) {
        SendMessageW(CONTROL(ui,UI_NOTIFICATIONS),BM_SETCHECK,ui->notifications?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(CONTROL(ui,UI_AUTOSTART),BM_SETCHECK,ui->autostart?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(CONTROL(ui,UI_COMPRESSION),CB_SETCURSEL,(WPARAM)ui->use_zstd,0);
        SendMessageW(CONTROL(ui,UI_LEVEL),CB_SETCURSEL,(WPARAM)(ui->zstd_level-1),0);
    }
}
static int date_value(drop_ui *ui,int id,int end,int64_t *value) {
    WCHAR text[16]; SYSTEMTIME local={0},utc; FILETIME file; ULARGE_INTEGER ticks; unsigned year,month,day; int i;
    GetWindowTextW(CONTROL(ui,id),text,16); if(!*text) { *value=end?INT64_MAX:0; return 1; }
    if(wcslen(text)!=10 || text[4]!=L'-' || text[7]!=L'-') return 0;
    for(i=0;i<10;++i) if(i!=4 && i!=7 && (text[i]<L'0' || text[i]>L'9')) return 0;
    if(swscanf_s(text,L"%4u-%2u-%2u",&year,&month,&day)!=3 || year<1970 || year>9999 || month<1 || month>12 || day<1 || day>31) return 0;
    local.wYear=(WORD)year; local.wMonth=(WORD)month; local.wDay=(WORD)day;
    if(end) { local.wHour=23; local.wMinute=59; local.wSecond=59; }
    if(!TzSpecificLocalTimeToSystemTime(NULL,&local,&utc) || !SystemTimeToFileTime(&utc,&file)) return 0;
    ticks.LowPart=file.dwLowDateTime; ticks.HighPart=file.dwHighDateTime;
    *value=(int64_t)(ticks.QuadPart/UINT64_C(10000000))-(int64_t)UINT64_C(11644473600); return *value>=0;
}
static void history_request(drop_ui *ui,int page,int new_filter) {
    ui_job *job=new_job(JOB_VIEW); char *text;
    if(!job) return;
    if(new_filter) {
        text=edit_utf8(ui,UI_SEARCH,1024);
        if(!text || !date_value(ui,UI_FROM,0,&ui->from) || !date_value(ui,UI_TO,1,&ui->to) || ui->from>ui->to) {
            free(text); drop_ui_job_free(job); status(ui,L"日期应为 YYYY-MM-DD，起日不能晚于止日。"); return;
        } strcpy_s(ui->search,sizeof(ui->search),text); free(text);
    }
    job->page=page; strcpy_s(job->search,sizeof(job->search),ui->search); job->from=ui->from; job->to=ui->to;
    if(submit(ui,job)) ui->page=page;
}
static void send_message(drop_ui *ui) {
    ui_job *job=new_job(JOB_SEND); char *recipient; int i;
    if(!job) return;
    job->body=edit_utf8(ui,UI_BODY,DROP_MAX_BODY); recipient=edit_utf8(ui,UI_RECIPIENT,64);
    if(!job->body || !recipient || (!*job->body && !ui->file_count)) { free(recipient); drop_ui_job_free(job); status(ui,L"正文不能超过 1 MiB；正文与附件至少填写一项。"); return; }
    strcpy_s(job->recipient,sizeof(job->recipient),recipient); free(recipient);
    for(i=0;i<ui->file_count;++i) {
        job->paths[i]=_wcsdup(ui->paths[i]); if(!job->paths[i]) { job->count=i; drop_ui_job_free(job); return; } ++job->count;
    } ui->sending=1; if(!submit(ui,job)) ui->sending=0; enable(ui);
}
static void detail_request(drop_ui *ui,int index) {
    ui_job *job;
    if(!ui->history || index<0 || index>=ui->history->count || ui->busy) return;
    job=new_job(JOB_DETAIL); if(!job) return;
    strcpy_s(job->id,sizeof(job->id),ui->history->rows[index].id); job->pending=ui->history->rows[index].pending; job->selection=++ui->selection;
    (void)submit(ui,job);
}
static void export_file(drop_ui *ui) {
    int index=(int)SendMessageW(CONTROL(ui,UI_FILES),CB_GETCURSEL,0,0); ui_job *job; OPENFILENAMEW dialog={0}; WCHAR safe[260];
    if(!ui->detail || index<0 || index>=ui->detail->count) return;
    if(!ui->detail->files[index].complete) { status(ui,L"附件尚未完整保存，等待自动下载或点击同步 / 重试。"); return; }
    job=new_job(JOB_EXPORT); if(!job) return;
    job->file=ui->detail->files[index]; drop_ui_safe_filename(job->file.name,safe); wcscpy_s(job->destination,32768,safe);
    dialog.lStructSize=sizeof(dialog); dialog.hwndOwner=ui->window; dialog.lpstrFile=job->destination; dialog.nMaxFile=32768;
    dialog.lpstrFilter=L"所有文件\0*.*\0\0"; dialog.Flags=OFN_EXPLORER|OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if(GetSaveFileNameW(&dialog)) (void)submit(ui,job); else drop_ui_job_free(job);
}
static int tray(drop_ui *ui,DWORD operation,int unread,int notify) {
    NOTIFYICONDATAW icon={0}; icon.cbSize=sizeof(icon); icon.hWnd=ui->window; icon.uID=1;
    icon.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP; icon.uCallbackMessage=DROP_UI_TRAY; icon.hIcon=LoadIconW(NULL,IDI_INFORMATION);
    swprintf_s(icon.szTip,128,L"Drop · 未读 %d · 关闭窗口继续同步",unread);
    if(notify && ui->notifications) {
        icon.uFlags|=NIF_INFO; wcscpy_s(icon.szInfoTitle,64,L"Drop 新消息");
        swprintf_s(icon.szInfo,256,L"收到 %d 条新记录，正在自动保存正文及完整附件。",notify); icon.dwInfoFlags=NIIF_INFO|NIIF_RESPECT_QUIET_TIME;
    }
    if(!Shell_NotifyIconW(operation,&icon)) return 0;
    if(operation==NIM_ADD) { icon.uVersion=NOTIFYICON_VERSION_4; (void)Shell_NotifyIconW(NIM_SETVERSION,&icon); ui->tray_added=1; }
    return 1;
}
static void show(drop_ui *ui) { ShowWindow(ui->window,SW_RESTORE); SetForegroundWindow(ui->window); }
static void display_result(drop_ui *ui,ui_result *result) {
    int i; HWND list=CONTROL(ui,UI_HISTORY); WCHAR title[96];
    ui->ready=result->ready; if(result->task_done) { ui->busy=0; if(result->type==JOB_SEND) ui->sending=0; }
    if(result->type==JOB_SETTINGS) {
        if(result->settings_saved) { ui->notifications=result->notifications; ui->autostart=result->autostart; ui->use_zstd=result->use_zstd; ui->zstd_level=result->zstd_level; }
        SendMessageW(CONTROL(ui,UI_NOTIFICATIONS),BM_SETCHECK,ui->notifications?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(CONTROL(ui,UI_AUTOSTART),BM_SETCHECK,ui->autostart?BST_CHECKED:BST_UNCHECKED,0);
        SendMessageW(CONTROL(ui,UI_COMPRESSION),CB_SETCURSEL,(WPARAM)ui->use_zstd,0);
        SendMessageW(CONTROL(ui,UI_LEVEL),CB_SETCURSEL,(WPARAM)(ui->zstd_level-1),0);
    }
    if(result->queued) { SetWindowTextW(CONTROL(ui,UI_BODY),L""); files_clear(ui); }
    if(result->history) {
        ui->more=result->history->more;
        SendMessageW(list,WM_SETREDRAW,FALSE,0); ListView_DeleteAllItems(list);
        free(ui->history); ui->history=result->history; result->history=NULL;
        for(i=0;i<ui->history->count;++i) {
            drop_history_row *row=&ui->history->rows[i]; LVITEMW item={0}; WCHAR direction[112];
            item.mask=LVIF_TEXT; item.iItem=i; item.pszText=row->time; ListView_InsertItem(list,&item);
            swprintf_s(direction,112,L"%s%s",row->unread?L"● ":L"",row->direction);
            ListView_SetItemText(list,i,1,direction); ListView_SetItemText(list,i,2,row->preview);
            ListView_SetItemText(list,i,3,row->files); ListView_SetItemText(list,i,4,row->state);
        }
        SendMessageW(list,WM_SETREDRAW,TRUE,0); InvalidateRect(list,NULL,TRUE);
        swprintf_s(title,96,L"Drop · 第 %d 页 · 未读 %d",ui->page+1,ui->history->unread); SetWindowTextW(ui->window,title);
        (void)tray(ui,ui->tray_added?NIM_MODIFY:NIM_ADD,ui->history->unread,ui->history->new_notifications);
    }
    if(result->detail && result->selection==ui->selection) {
        drop_ui_detail_free(ui->detail); ui->detail=result->detail; result->detail=NULL;
        SetWindowTextW(CONTROL(ui,UI_DETAIL),ui->detail->text); SendMessageW(CONTROL(ui,UI_FILES),CB_RESETCONTENT,0,0);
        for(i=0;i<ui->detail->count;++i) {
            WCHAR text[1200]; drop_file_view *file=&ui->detail->files[i];
            swprintf_s(text,1200,L"%s · %llu 字节 · %s",file->name,(unsigned long long)file->total,file->complete?L"已保存":L"待下载");
            SendMessageW(CONTROL(ui,UI_FILES),CB_ADDSTRING,0,(LPARAM)text);
        } if(ui->detail->count) SendMessageW(CONTROL(ui,UI_FILES),CB_SETCURSEL,0,0);
    }
    status(ui,result->status); enable(ui); drop_ui_result_free(result);
}
static void exit_app(drop_ui *ui) {
    MSG message;
    SetEvent(ui->stop); SetEvent(ui->wake);
    if(ui->network_notification) { CancelMibChangeNotify2(ui->network_notification); ui->network_notification=NULL; }
    drop_ui_cancel_ws(ui); if(ui->tray_added) (void)tray(ui,NIM_DELETE,0,0);
    /* Exit only this Drop instance; durable queues and block files recover on restart. */
    if(ui->worker && WaitForSingleObject(ui->worker,3000)!=WAIT_OBJECT_0) ExitProcess(0);
    while(PeekMessageW(&message,ui->window,DROP_UI_RESULT,DROP_UI_RESULT,PM_REMOVE)) drop_ui_result_free((ui_result *)message.lParam);
    DestroyWindow(ui->window);
}
static LRESULT CALLBACK window_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    drop_ui *ui=(drop_ui *)GetWindowLongPtrW(window,GWLP_USERDATA);
    if(message==WM_NCCREATE) { ui=((CREATESTRUCTW *)lparam)->lpCreateParams; ui->window=window; SetWindowLongPtrW(window,GWLP_USERDATA,(LONG_PTR)ui); }
    if(!ui) return DefWindowProcW(window,message,wparam,lparam);
    if(message==ui->taskbar_created && ui->taskbar_created) { ui->tray_added=0; (void)tray(ui,NIM_ADD,ui->history?ui->history->unread:0,0); return 0; }
    switch(message) {
    case WM_CREATE: create_controls(ui); DragAcceptFiles(window,TRUE); (void)tray(ui,NIM_ADD,0,0); return 0;
    case WM_DROPFILES: {
        HDROP files=(HDROP)wparam; UINT count=DragQueryFileW(files,0xffffffff,NULL,0),i; WCHAR text[64];
        if(!ui->sending) for(i=0;i<count && ui->file_count<DROP_MAX_ATTACHMENTS;++i) {
            UINT length=DragQueryFileW(files,i,NULL,0); WCHAR *path;
            if(!length || length>=32768) continue;
            path=malloc(((size_t)length+1)*sizeof(WCHAR)); if(!path) break;
            if(DragQueryFileW(files,i,path,length+1)==length) ui->paths[ui->file_count++]=path; else free(path);
        }
        DragFinish(files); swprintf_s(text,64,L"添加附件（%d）",ui->file_count); SetWindowTextW(CONTROL(ui,UI_ATTACH),text); return 0;
    }
    case WM_SIZE: layout(ui,LOWORD(lparam),HIWORD(lparam)); return 0;
    case WM_GETMINMAXINFO: ((MINMAXINFO *)lparam)->ptMinTrackSize.x=1000; ((MINMAXINFO *)lparam)->ptMinTrackSize.y=700; return 0;
    case WM_CLOSE:
        if(!ui->tray_added && !tray(ui,NIM_ADD,0,0)) { status(ui,L"托盘不可用，窗口保持打开。真正退出按钮可停止同步。"); return 0; }
        ShowWindow(window,SW_HIDE); return 0;
    case DROP_UI_SHOW: show(ui); return 0;
    case DROP_UI_RESULT: display_result(ui,(ui_result *)lparam); return 0;
    case DROP_UI_TRAY:
        if(LOWORD(lparam)==NIN_SELECT || LOWORD(lparam)==NIN_KEYSELECT || LOWORD(lparam)==WM_LBUTTONDBLCLK || LOWORD(lparam)==NIN_BALLOONUSERCLICK) show(ui);
        else if(LOWORD(lparam)==WM_CONTEXTMENU || LOWORD(lparam)==WM_RBUTTONUP) {
            HMENU menu=CreatePopupMenu(); POINT point; GetCursorPos(&point);
            AppendMenuW(menu,MF_STRING,TRAY_OPEN,L"打开 Drop"); AppendMenuW(menu,MF_STRING,TRAY_EXIT,L"真正退出（停止同步）");
            SetForegroundWindow(window); TrackPopupMenu(menu,TPM_RIGHTBUTTON,point.x,point.y,0,window,NULL); DestroyMenu(menu);
        } return 0;
    case WM_COMMAND:
        switch(LOWORD(wparam)) {
        case TRAY_OPEN: show(ui); break;
        case TRAY_EXIT: case UI_EXIT: exit_app(ui); break;
        case UI_PROBE: credentials(ui,JOB_PROBE); break;
        case UI_SIGNIN: credentials(ui,JOB_LOGIN); break;
        case UI_SEND: send_message(ui); break;
        case UI_ATTACH: files_choose(ui); break;
        case UI_CLEAR: files_clear(ui); break;
        case UI_SYNC: (void)submit(ui,new_job(JOB_NONE)); break;
        case UI_RESTORE_RETRY:
            if(MessageBoxW(ui->window,L"服务器恢复后，部分发送任务的接收结果可能不确定。请先核对本机记录与服务器历史。确认重新提交全部待核对任务？",L"重新提交待核对任务",MB_YESNO|MB_ICONQUESTION)==IDYES) (void)submit(ui,new_job(JOB_RESTORE_RETRY));
            break;
        case UI_LOGOUT: (void)submit(ui,new_job(JOB_LOGOUT)); break;
        case UI_FIND: history_request(ui,0,1); break;
        case UI_PREVIOUS: if(ui->page>0) history_request(ui,ui->page-1,0); break;
        case UI_NEXT: if(ui->more) history_request(ui,ui->page+1,0); break;
        case UI_EXPORT: export_file(ui); break;
        case UI_NOTIFICATIONS: case UI_AUTOSTART: settings_save(ui); break;
        case UI_COMPRESSION: case UI_LEVEL: if(HIWORD(wparam)==CBN_SELCHANGE) settings_save(ui); break;
        } return 0;
    case WM_NOTIFY: {
        NMHDR *header=(NMHDR *)lparam;
        if(header->idFrom==UI_HISTORY && header->code==LVN_ITEMCHANGED) {
            NMLISTVIEW *change=(NMLISTVIEW *)lparam;
            if((change->uNewState&LVIS_SELECTED) && !(change->uOldState&LVIS_SELECTED)) detail_request(ui,change->iItem);
        } return 0;
    }
    case WM_POWERBROADCAST:
        if(wparam==PBT_APMRESUMEAUTOMATIC || wparam==PBT_APMRESUMESUSPEND) drop_ui_resume(ui); return TRUE;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
static void CALLBACK network_changed(void *context,PMIB_IPINTERFACE_ROW row,MIB_NOTIFICATION_TYPE type) {
    drop_ui *ui=context; (void)row; (void)type;
    if(WaitForSingleObject(ui->stop,0)==WAIT_TIMEOUT) drop_ui_resume(ui);
}
int drop_ui_run(HINSTANCE instance,const WCHAR *root,int hidden) {
    drop_ui *ui=calloc(1,sizeof(*ui)); WNDCLASSEXW cls={0}; INITCOMMONCONTROLSEX controls={sizeof(controls),ICC_LISTVIEW_CLASSES|ICC_STANDARD_CLASSES};
    MSG message; unsigned char hash[32]; char *utf8=NULL,hex[65]; WCHAR wide[65],mutex_name[96]; DWORD length; int result=1,ole=0;
    if(!ui) return 1; ui->instance=instance; ui->hidden=hidden;
    if(root) length=GetFullPathNameW(root,32768,ui->root,NULL);
    else {
        PWSTR local=NULL; if(FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData,0,NULL,&local))) goto done;
        length=drop_path_join(local,L"Drop",ui->root,32768)?(DWORD)wcslen(ui->root):0; CoTaskMemFree(local);
    }
    if(!length || length>=32768 || !drop_private_directory(ui->root)) goto done;
    utf8=malloc(131072); if(!utf8 || !drop_to_utf8(ui->root,utf8,131072)) goto done;
    /* Case-insensitive Windows paths map to one per-root UI and one worker. */
    CharLowerBuffW(ui->root,(DWORD)wcslen(ui->root)); crypto_hash_sha256(hash,(const unsigned char *)ui->root,wcslen(ui->root)*sizeof(WCHAR));
    sodium_bin2hex(hex,sizeof(hex),hash,32); drop_to_wide(hex,wide,65);
    swprintf_s(ui->class_name,80,L"DropWindow-%.16s",wide); swprintf_s(mutex_name,96,L"Local\\%s",ui->class_name);
    ui->singleton=CreateMutexW(NULL,FALSE,mutex_name);
    if(!ui->singleton) goto done;
    if(GetLastError()==ERROR_ALREADY_EXISTS) {
        HWND existing=FindWindowW(ui->class_name,NULL); if(existing) PostMessageW(existing,DROP_UI_SHOW,0,0); result=0; goto done;
    }
    if(!drop_ui_settings_load(ui)) { ui->notifications=1; ui->to=INT64_MAX; }
    if(FAILED(OleInitialize(NULL))) goto done; ole=1;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE); InitCommonControlsEx(&controls);
    ui->font=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
    ui->stop=CreateEventW(NULL,TRUE,FALSE,NULL); ui->wake=CreateEventW(NULL,FALSE,FALSE,NULL); InitializeCriticalSection(&ui->lock);
    if(!ui->stop || !ui->wake) goto initialized;
    ui->taskbar_created=RegisterWindowMessageW(L"TaskbarCreated");
    cls.cbSize=sizeof(cls); cls.hInstance=instance; cls.lpfnWndProc=window_proc; cls.lpszClassName=ui->class_name;
    cls.hCursor=LoadCursorW(NULL,IDC_ARROW); cls.hIcon=LoadIconW(NULL,IDI_INFORMATION); cls.hbrBackground=(HBRUSH)(COLOR_WINDOW+1);
    if(!RegisterClassExW(&cls)) goto initialized;
    ui->window=CreateWindowExW(0,ui->class_name,L"Drop",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,1100,780,NULL,NULL,instance,ui);
    if(!ui->window) goto initialized;
    ui->worker=CreateThread(NULL,0,drop_ui_worker,ui,0,NULL); if(!ui->worker) { DestroyWindow(ui->window); goto initialized; }
    if(NotifyIpInterfaceChange(AF_UNSPEC,network_changed,ui,FALSE,&ui->network_notification)!=NO_ERROR) ui->network_notification=NULL;
    if(!hidden || !ui->tray_added) { ShowWindow(ui->window,SW_SHOW); UpdateWindow(ui->window); }
    while(GetMessageW(&message,NULL,0,0)>0) { if(!IsDialogMessageW(ui->window,&message)) { TranslateMessage(&message); DispatchMessageW(&message); } }
    result=0;
initialized:
    if(ui->network_notification) CancelMibChangeNotify2(ui->network_notification);
    if(ui->worker) CloseHandle(ui->worker); if(ui->stop) CloseHandle(ui->stop); if(ui->wake) CloseHandle(ui->wake);
    DeleteCriticalSection(&ui->lock); drop_ui_job_free(ui->job); files_clear(ui); free(ui->history); drop_ui_detail_free(ui->detail);
    if(ui->font) DeleteObject(ui->font);
done:
    if(ui->singleton) CloseHandle(ui->singleton); if(ole) OleUninitialize(); free(utf8); free(ui); return result;
}
