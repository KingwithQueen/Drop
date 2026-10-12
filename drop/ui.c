/* 原生窗口和托盘界面；本文件在界面线程执行，耗时任务通过 ui_job 交给同步工作线程。 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include "ui_internal.h"
#include "resource.h"
#include <iphlpapi.h>
#include <commdlg.h>
#include <shlobj.h>
#include <richedit.h>
#include <windowsx.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CONTROL(ui, id) ((ui)->controls[(id) - UI_URL])
#define TRAY_OPEN 1001
#define TRAY_EXIT 1002
#define TRAY_TIMER 1

/* 更新界面状态文本；只在窗口所属线程调用。 */
static void status(drop_ui *ui, const WCHAR *text) {
    SetWindowTextW(CONTROL(ui, UI_STATUS), text);
    SendMessageW(CONTROL(ui, UI_STATUS), SB_SETTEXTW, 0, (LPARAM)text);
    SetWindowTextW(CONTROL(ui, UI_CONFIG_STATUS), text);
}
/* 创建一个原生控件并登记到窗口状态，使用统一字体供后续布局和命令处理。 */
static HWND control(drop_ui *ui, int id, const WCHAR *kind, const WCHAR *text, DWORD style) {
    int configuration = drop_ui_configuration_control(id);
    HWND parent = configuration ? ui->configuration : ui->window;
    HWND window =
        CreateWindowExW(!wcscmp(kind, L"EDIT") ? WS_EX_CLIENTEDGE : 0, kind, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent, (HMENU)(INT_PTR)id, ui->instance, NULL);
    if (id >= UI_URL && id < UI_COUNT) CONTROL(ui, id) = window;
    SendMessageW(window, WM_SETFONT, (WPARAM)(configuration ? ui->configuration_font : ui->font), TRUE);
    return window;
}
/* 根据控件 ID 调整其位置和尺寸，供界面线程布局使用。 */
static void move(drop_ui *ui, int id, int x, int y, int width, int height) {
    drop_ui_move(ui, id, x, y, width, height);
}
/* 为编辑框设置输入提示，不改变其实际输入内容。 */
static void hint(HWND edit, const WCHAR *text) {
    SendMessageW(edit, EM_SETCUEBANNER, FALSE, (LPARAM)text);
}
/* 发送对象必须是当前选中且已验证的会话身份，停用或删除后禁止排队，不能退回默认发给自己。 */
static int recipient_available(drop_ui *ui) {
    if (ui->history && drop_id_valid(ui->peer))
        for (int i = 0; i < ui->history->peer_count; ++i)
            if (!strcmp(ui->peer, ui->history->peers[i].id)) return ui->history->peers[i].available;
    return 0;
}
/* 依据当前消息的发送账号及服务器能力启用删除；不比较发送设备，恢复后尚未核对的记录保持禁用。 */
static int delete_available(drop_ui *ui) {
    if (!ui->ready || ui->busy || !ui->history || !ui->history->deletion_supported || !*ui->selected_id) return 0;
    for (int i = 0; i < ui->history->count; ++i)
        if (!strcmp(ui->selected_id, ui->history->rows[i].id)) return ui->history->rows[i].deletable;
    return 0;
}
/* 根据登录、任务及已知收件人状态更新按钮和菜单，凭据失效时仍允许退出账号以清除保存状态。 */
static void enable(drop_ui *ui) {
    EnableWindow(CONTROL(ui, UI_SIGNIN), !ui->busy);
    EnableWindow(CONTROL(ui, UI_PROBE), !ui->busy);
    EnableWindow(CONTROL(ui, UI_SEND), ui->ready && !ui->busy && recipient_available(ui));
    EnableWindow(CONTROL(ui, UI_SYNC), ui->ready && !ui->busy);
    EnableWindow(CONTROL(ui, UI_LOGOUT), (ui->ready || ui->history) && !ui->busy);
    EnableWindow(CONTROL(ui, UI_RESTORE_RETRY), ui->ready && !ui->busy && ui->history && ui->history->restore_review > 0);
    EnableWindow(CONTROL(ui, UI_FIND), ui->ready && !ui->busy);
    EnableWindow(CONTROL(ui, UI_PREVIOUS), ui->ready && !ui->busy && ui->page > 0);
    EnableWindow(CONTROL(ui, UI_NEXT), ui->ready && !ui->busy && ui->more);
    EnableWindow(CONTROL(ui, UI_EXPORT), ui->ready && !ui->busy && ui->detail && ui->detail->count > 0);
    EnableWindow(CONTROL(ui, UI_DELETE_MESSAGE), delete_available(ui));
    EnableWindow(CONTROL(ui, UI_BODY), !ui->sending);
    EnableWindow(CONTROL(ui, UI_RECIPIENT), !ui->sending);
    EnableWindow(CONTROL(ui, UI_ATTACH), !ui->sending);
    EnableWindow(CONTROL(ui, UI_CLEAR), !ui->sending);
    EnableWindow(CONTROL(ui, UI_REMOVE_ATTACH), !ui->sending && ui->file_count > 0);
    EnableWindow(CONTROL(ui, UI_DRAFT_FILES), !ui->sending);
    EnableWindow(CONTROL(ui, UI_NOTIFICATIONS), !ui->busy);
    EnableWindow(CONTROL(ui, UI_TRAY_FLASH), !ui->busy);
    EnableWindow(CONTROL(ui, UI_AUTOSTART), !ui->busy);
    EnableWindow(CONTROL(ui, UI_COMPRESSION), !ui->busy);
    EnableWindow(CONTROL(ui, UI_LEVEL), !ui->busy && ui->use_zstd);
    EnableWindow(CONTROL(ui, UI_SAVE_CONFIG), !ui->busy);
    EnableWindow(CONTROL(ui, UI_URL), !ui->busy);
    EnableWindow(CONTROL(ui, UI_CA), !ui->busy);
    EnableWindow(CONTROL(ui, UI_LOGIN), !ui->busy);
    EnableWindow(CONTROL(ui, UI_PASSWORD), !ui->busy);
    EnableWindow(CONTROL(ui, UI_USERS), ui->ready && !ui->busy && !ui->sending);
    EnableWindow(CONTROL(ui, UI_ADD_USER), ui->ready && !ui->busy);
    EnableMenuItem(GetMenu(ui->window), UI_SEND, MF_BYCOMMAND | (ui->ready && !ui->busy && recipient_available(ui) ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(GetMenu(ui->window), UI_SYNC, MF_BYCOMMAND | (ui->ready && !ui->busy ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(GetMenu(ui->window), UI_EXPORT, MF_BYCOMMAND | (ui->ready && !ui->busy && ui->detail && ui->detail->count ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(GetMenu(ui->window), UI_DELETE_MESSAGE, MF_BYCOMMAND | (delete_available(ui) ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(GetMenu(ui->window), UI_ATTACH, MF_BYCOMMAND | (!ui->sending ? MF_ENABLED : MF_GRAYED));
    EnableMenuItem(GetMenu(ui->window), UI_RESTORE_RETRY, MF_BYCOMMAND | (IsWindowEnabled(CONTROL(ui, UI_RESTORE_RETRY)) ? MF_ENABLED : MF_GRAYED));
}
/* 按当前窗口尺寸安排原生控件，保持消息编辑、历史和状态区域可用。 */
static void layout(drop_ui *ui, int width, int height) {
    int left, right, w, files_width, parts[2], draft = ui->file_count ? 92 : 0;
    drop_ui_viewport(ui, ui->window, 550 + draft, &width, &height);
    if (ui->divider < 200) ui->divider = 200;
    if (ui->divider > width - 580) ui->divider = width - 580;
    left = ui->divider + 8;
    right = width - 8;
    w = right - left;
    files_width = w / 2;
    move(ui, UI_SYNC, 8, 8, 90, 28);
    move(ui, UI_CONFIGURATION, 106, 8, 116, 28);
    move(ui, UI_EXPORT, 230, 8, 116, 28);
    move(ui, UI_RESTORE_RETRY, 354, 8, 170, 28);
    move(ui, UI_RESET_FILTER, 532, 8, 94, 28);
    move(ui, UI_DELETE_MESSAGE, 634, 8, 90, 28);
    move(ui, UI_EXIT, right - 98, 8, 98, 28);
    move(ui, UI_USER_SEARCH, 8, 46, ui->divider - 106, 26);
    move(ui, UI_ADD_USER, ui->divider - 92, 46, 84, 26);
    move(ui, UI_USERS, 8, 80, ui->divider - 16, height - 150);
    move(ui, UI_HISTORY_LABEL, 8, height - 64, ui->divider - 16, 36);
    move(ui, UI_SPLITTER, ui->divider, 44, 4, height - 72);
    move(ui, UI_OVERVIEW, left, 46, w, 24);
    move(ui, UI_SEARCH, left, 80, w - 400, 26);
    move(ui, UI_FROM, right - 394, 80, 100, 26);
    move(ui, UI_TO, right - 288, 80, 100, 26);
    move(ui, UI_FIND, right - 182, 80, 54, 26);
    move(ui, UI_PREVIOUS, right - 122, 80, 58, 26);
    move(ui, UI_NEXT, right - 58, 80, 58, 26);
    move(ui, UI_HISTORY, left, 112, w, height - 410 - draft);
    move(ui, UI_DETAIL_LABEL, left, height - 292 - draft, w, 20);
    move(ui, UI_DETAIL, left, height - 266 - draft, w - files_width - 6, 84);
    move(ui, UI_FILES, right - files_width, height - 266 - draft, files_width, 84);
    move(ui, UI_RECIPIENT, left, height - 174 - draft, w, 26);
    move(ui, UI_DRAFT_LABEL, left, height - 234, w, 20);
    move(ui, UI_DRAFT_FILES, left, height - 210, w, 64);
    move(ui, UI_BODY, left, height - 140, w, 74);
    move(ui, UI_ATTACH, left, height - 58, 116, 26);
    move(ui, UI_CLEAR, left + 124, height - 58, 88, 26);
    move(ui, UI_REMOVE_ATTACH, left + 220, height - 58, 88, 26);
    move(ui, UI_SEND_HINT, left + 316, height - 54, w - 408, 20);
    move(ui, UI_SEND, right - 84, height - 58, 84, 26);
    /* 状态栏贴住实际像素边界，避免分数 DPI 将逻辑底边向上舍入后多出一个像素。 */
    RECT client;
    GetClientRect(ui->window, &client);
    int bottom = client.bottom > drop_ui_scale(ui->dpi, 550 + draft) ? client.bottom : drop_ui_scale(ui->dpi, 550 + draft);
    MoveWindow(CONTROL(ui, UI_STATUS), 0, bottom - drop_ui_scale(ui->dpi, 24) - ui->scroll, client.right, drop_ui_scale(ui->dpi, 24), TRUE);
    ListView_SetColumnWidth(CONTROL(ui, UI_USERS), 0, drop_ui_scale(ui->dpi, ui->divider - 72));
    ListView_SetColumnWidth(CONTROL(ui, UI_USERS), 1, drop_ui_scale(ui->dpi, 46));
    ListView_SetColumnWidth(CONTROL(ui, UI_FILES), 0, drop_ui_scale(ui->dpi, files_width - 184));
    ListView_SetColumnWidth(CONTROL(ui, UI_FILES), 1, drop_ui_scale(ui->dpi, 82));
    ListView_SetColumnWidth(CONTROL(ui, UI_FILES), 2, drop_ui_scale(ui->dpi, 76));
    ListView_SetColumnWidth(CONTROL(ui, UI_DRAFT_FILES), 0, drop_ui_scale(ui->dpi, w - 156));
    ListView_SetColumnWidth(CONTROL(ui, UI_DRAFT_FILES), 1, drop_ui_scale(ui->dpi, 132));
    parts[0] = drop_ui_scale(ui->dpi, width - 118);
    parts[1] = -1;
    SendMessageW(CONTROL(ui, UI_STATUS), SB_SETPARTS, 2, (LPARAM)parts);
    SendMessageW(CONTROL(ui, UI_STATUS), SB_SETTEXTW, 1, (LPARAM)L"版本：" DROP_VERSION_WIDE);
}
/* 标准菜单复用已有命令，工具栏和菜单的行为保持一致。 */
static void menu_create(drop_ui *ui) {
    HMENU bar = CreateMenu(), file = CreatePopupMenu(), view = CreatePopupMenu(), tools = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, UI_ATTACH, L"添加附件…");
    AppendMenuW(file, MF_STRING, UI_EXPORT, L"导出所选附件…");
    AppendMenuW(file, MF_STRING, UI_SEND, L"发送\tCtrl+Enter");
    AppendMenuW(file, MF_STRING, UI_DELETE_MESSAGE, L"删除所选消息（同步所有设备）");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, UI_EXIT, L"真正退出（停止同步）");
    AppendMenuW(view, MF_STRING, UI_SYNC, L"同步 / 重试\tF5");
    AppendMenuW(view, MF_STRING, UI_RESET_FILTER, L"清除筛选");
    AppendMenuW(tools, MF_STRING, UI_CONFIGURATION, L"配置与账号…");
    AppendMenuW(tools, MF_STRING, UI_RESTORE_RETRY, L"重新提交待核对任务…");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"文件(&F)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"查看(&V)");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)tools, L"工具(&T)");
    SetMenu(ui->window, bar);
}
/* 拖动分隔条时由主窗口捕获鼠标，布局随目标 DPI 重新计算，不缩放旧控件位图。 */
static LRESULT CALLBACK splitter_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR subclass, DWORD_PTR data) {
    drop_ui *ui = (drop_ui *)data;
    (void)subclass;
    if (message == WM_SETCURSOR) {
        SetCursor(LoadCursorW(NULL, IDC_SIZEWE));
        return TRUE;
    }
    if (message == WM_LBUTTONDOWN) {
        ui->dragging = 1;
        SetCapture(ui->window);
        return 0;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}
/* 将连接与账号控件建在配置窗口，将发送和历史控件建在主窗口，控件 ID 保持稳定。 */
static void create_controls(drop_ui *ui) {
    HWND edit;
    int i;
    LVCOLUMNW column = {0};
    const WCHAR *titles[] = {L"文件名", L"大小", L"状态"};
    control(ui, UI_CONNECTION_LABEL, L"STATIC", L"连接与账号", SS_LEFT);
    control(ui, UI_URL_LABEL, L"STATIC", L"服务器地址", SS_LEFT);
    control(ui, UI_CA_LABEL, L"STATIC", L"私有 CA 文件（可选；留空使用 Windows 信任）", SS_LEFT);
    control(ui, UI_LOGIN_LABEL, L"STATIC", L"登录账号", SS_LEFT);
    control(ui, UI_PASSWORD_LABEL, L"STATIC", L"登录密码", SS_LEFT);
    control(ui, UI_URL, L"EDIT", ui->url, ES_AUTOHSCROLL | WS_TABSTOP);
    hint(CONTROL(ui, UI_URL), L"http://地址:8080（PAKE）或 https://地址:8443（TLS）");
    control(ui, UI_CA, L"EDIT", ui->ca, ES_AUTOHSCROLL | WS_TABSTOP);
    hint(CONTROL(ui, UI_CA), L"可选：明确指定的私有 CA PEM 文件路径（留空使用 Windows 信任）");
    control(ui, UI_LOGIN, L"EDIT", ui->login, ES_AUTOHSCROLL | WS_TABSTOP);
    hint(CONTROL(ui, UI_LOGIN), L"Drop 登录名");
    control(ui, UI_PASSWORD, L"EDIT", L"", ES_PASSWORD | ES_AUTOHSCROLL | WS_TABSTOP);
    hint(CONTROL(ui, UI_PASSWORD), L"密码（仅用于登录，不保存原始密码）");
    SendMessageW(CONTROL(ui, UI_LOGIN), EM_SETLIMITTEXT, 64, 0);
    SendMessageW(CONTROL(ui, UI_PASSWORD), EM_SETLIMITTEXT, 1024, 0);
    control(ui, UI_PROBE, L"BUTTON", L"测试连接", WS_TABSTOP);
    control(ui, UI_SIGNIN, L"BUTTON", L"登录", WS_TABSTOP);
    control(ui, UI_LOGOUT, L"BUTTON", L"退出账号", WS_TABSTOP);
    control(ui, UI_CONFIG_HINT, L"STATIC", L"登录状态自动加密保存，下次启动恢复。\n连接或账号的修改在下次登录时使用。", SS_LEFT);
    control(ui, UI_PREFERENCES_LABEL, L"STATIC", L"通知与本机选项（修改后自动保存）", SS_LEFT);
    control(ui, UI_NOTIFICATIONS, L"BUTTON", L"Windows 通知", BS_AUTOCHECKBOX | WS_TABSTOP);
    control(ui, UI_TRAY_FLASH, L"BUTTON", L"托盘图标闪烁", BS_AUTOCHECKBOX | WS_TABSTOP);
    control(ui, UI_AUTOSTART, L"BUTTON", L"登录 Windows 后自动启动", BS_AUTOCHECKBOX | WS_TABSTOP);
    SendMessageW(CONTROL(ui, UI_NOTIFICATIONS), BM_SETCHECK, ui->notifications ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(CONTROL(ui, UI_TRAY_FLASH), BM_SETCHECK, ui->tray_flash ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(CONTROL(ui, UI_AUTOSTART), BM_SETCHECK, ui->autostart ? BST_CHECKED : BST_UNCHECKED, 0);
    control(ui, UI_COMPRESSION, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP);
    SendMessageW(CONTROL(ui, UI_COMPRESSION), CB_ADDSTRING, 0, (LPARAM)L"正文存储：不压缩");
    SendMessageW(CONTROL(ui, UI_COMPRESSION), CB_ADDSTRING, 0, (LPARAM)L"正文存储：Zstandard");
    SendMessageW(CONTROL(ui, UI_COMPRESSION), CB_SETCURSEL, (WPARAM)ui->use_zstd, 0);
    control(ui, UI_LEVEL, WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP);
    for (i = 1; i <= 19; ++i) {
        WCHAR text[32];
        swprintf_s(text, 32, L"压缩级别 %d", i);
        SendMessageW(CONTROL(ui, UI_LEVEL), CB_ADDSTRING, 0, (LPARAM)text);
    }
    SendMessageW(CONTROL(ui, UI_LEVEL), CB_SETCURSEL, (WPARAM)(ui->zstd_level - 1), 0);
    control(ui, UI_SAVE_CONFIG, L"BUTTON", L"保存配置", WS_TABSTOP);
    control(ui, UI_CONFIG_CLOSE, L"BUTTON", L"返回消息", WS_TABSTOP);
    control(ui, UI_CONFIG_STATUS, L"STATIC", L"填写服务器与账号后登录。", SS_LEFT);
    control(ui, UI_OVERVIEW, L"STATIC", L"本机消息 · 正在读取登录状态…", SS_LEFT);
    control(ui, UI_CONFIGURATION, L"BUTTON", L"配置与账号…", WS_TABSTOP);
    control(ui, UI_HISTORY_LABEL, L"STATIC", L"消息历史", SS_LEFT);
    control(ui, UI_DETAIL_LABEL, L"STATIC", L"所选消息与附件", SS_LEFT);
    control(ui, UI_USER_SEARCH, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP);
    hint(CONTROL(ui, UI_USER_SEARCH), L"账号 / 筛选用户");
    SendMessageW(CONTROL(ui, UI_USER_SEARCH), EM_SETLIMITTEXT, 64, 0);
    control(ui, UI_ADD_USER, L"BUTTON", L"查找账号", WS_TABSTOP);
    control(ui, UI_RESET_FILTER, L"BUTTON", L"清除筛选", WS_TABSTOP);
    control(ui, UI_DELETE_MESSAGE, L"BUTTON", L"删除消息", WS_TABSTOP);
    control(ui, UI_SPLITTER, L"STATIC", L"", SS_ETCHEDVERT | SS_NOTIFY);
    SetWindowSubclass(CONTROL(ui, UI_SPLITTER), splitter_proc, 1, (DWORD_PTR)ui);
    control(ui, UI_USERS, WC_LISTVIEWW, L"用户", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP | WS_BORDER);
    ListView_SetExtendedListViewStyle(CONTROL(ui, UI_USERS), LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    column.mask = LVCF_TEXT | LVCF_WIDTH;
    column.pszText = L"用户 / 账号";
    column.cx = 170;
    ListView_InsertColumn(CONTROL(ui, UI_USERS), 0, &column);
    column.pszText = L"未读";
    column.cx = 46;
    ListView_InsertColumn(CONTROL(ui, UI_USERS), 1, &column);
    control(ui, UI_DRAFT_LABEL, L"STATIC", L"待发送附件", SS_LEFT);
    control(ui, UI_DRAFT_FILES, WC_LISTVIEWW, L"待发送附件", LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP | WS_BORDER);
    ListView_SetExtendedListViewStyle(CONTROL(ui, UI_DRAFT_FILES), LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    column.pszText = L"待发送文件";
    ListView_InsertColumn(CONTROL(ui, UI_DRAFT_FILES), 0, &column);
    column.pszText = L"大小";
    ListView_InsertColumn(CONTROL(ui, UI_DRAFT_FILES), 1, &column);
    control(ui, UI_REMOVE_ATTACH, L"BUTTON", L"移除所选", WS_TABSTOP);
    control(ui, UI_SEND_HINT, L"STATIC", L"Ctrl+Enter 发送", SS_LEFT);
    control(ui, UI_RECIPIENT, L"STATIC", L"请先登录，再从左侧选择发送对象。", SS_LEFT | SS_ENDELLIPSIS);
    edit = control(ui, UI_BODY, L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP);
    SendMessageW(edit, EM_SETLIMITTEXT, DROP_MAX_BODY, 0);
    hint(edit, L"输入正文；Ctrl+V 可粘贴文字或文件，附件会显示在上方");
    control(ui, UI_ATTACH, L"BUTTON", L"添加附件（0）", WS_TABSTOP);
    control(ui, UI_CLEAR, L"BUTTON", L"清除附件", WS_TABSTOP);
    control(ui, UI_SEND, L"BUTTON", L"发送", BS_OWNERDRAW | WS_TABSTOP);
    control(ui, UI_SYNC, L"BUTTON", L"同步 / 重试", WS_TABSTOP);
    control(ui, UI_RESTORE_RETRY, L"BUTTON", L"重新提交待核对任务", WS_TABSTOP);
    control(ui, UI_SEARCH, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP);
    hint(CONTROL(ui, UI_SEARCH), L"查询本机正文（逐字匹配）");
    SendMessageW(CONTROL(ui, UI_SEARCH), EM_SETLIMITTEXT, 256, 0);
    control(ui, UI_FROM, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP);
    hint(CONTROL(ui, UI_FROM), L"起日");
    control(ui, UI_TO, L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP);
    hint(CONTROL(ui, UI_TO), L"止日");
    SendMessageW(CONTROL(ui, UI_FROM), EM_SETLIMITTEXT, 10, 0);
    SendMessageW(CONTROL(ui, UI_TO), EM_SETLIMITTEXT, 10, 0);
    control(ui, UI_FIND, L"BUTTON", L"查询", WS_TABSTOP);
    control(ui, UI_PREVIOUS, L"BUTTON", L"上一页", WS_TABSTOP);
    control(ui, UI_NEXT, L"BUTTON", L"下一页", WS_TABSTOP);
    control(ui, UI_HISTORY, MSFTEDIT_CLASS, L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP | WS_BORDER);
    drop_ui_message_control(ui);
    control(ui, UI_DETAIL, L"EDIT", L"选择历史记录查看完整正文。关闭窗口后仍在托盘同步。", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP);
    SendMessageW(CONTROL(ui, UI_DETAIL), EM_SETLIMITTEXT, DROP_MAX_BODY, 0);
    control(ui, UI_FILES, WC_LISTVIEWW, L"所选消息的附件", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP | WS_BORDER);
    ListView_SetExtendedListViewStyle(CONTROL(ui, UI_FILES), LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    for (i = 0; i < 3; ++i) {
        column.pszText = (LPWSTR)titles[i];
        column.cx = 100;
        ListView_InsertColumn(CONTROL(ui, UI_FILES), i, &column);
    }
    control(ui, UI_EXPORT, L"BUTTON", L"导出所选附件", WS_TABSTOP);
    control(ui, UI_EXIT, L"BUTTON", L"真正退出", WS_TABSTOP);
    control(ui, UI_STATUS, STATUSCLASSNAMEW, L"正在读取本机状态…", SBARS_SIZEGRIP | CCS_NORESIZE | CCS_NOPARENTALIGN);
    enable(ui);
}
/* 释放界面持有的附件路径，清空已选附件并更新显示。 */
static void files_clear(drop_ui *ui) {
    int i;
    for (i = 0; i < ui->file_count; ++i)
        free(ui->paths[i]);
    ui->file_count = 0;
    memset(ui->paths, 0, sizeof(ui->paths));
    drop_ui_draft_refresh(ui);
}
/* 显示待发送文件的原始名称摘要，完整路径仍只在界面持有的附件数组中保存。 */
static void files_summary(drop_ui *ui) {
    drop_ui_draft_refresh(ui);
}
/* 通过原生多选文件框取得附件路径，检查数量和容量后交由界面持有。 */
static void files_choose(drop_ui *ui) {
    OPENFILENAMEW dialog = {0};
    WCHAR *buffer = calloc(65536, sizeof(WCHAR)), text[64];
    const WCHAR *next;
    if (!buffer) return;
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = ui->window;
    dialog.lpstrFile = buffer;
    dialog.nMaxFile = 65536;
    dialog.lpstrFilter = L"所有文件\0*.*\0\0";
    dialog.Flags = OFN_EXPLORER | OFN_ALLOWMULTISELECT | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog)) {
        next = buffer + wcslen(buffer) + 1;
        if (!*next) {
            (void)drop_ui_draft_add(ui, buffer);
        } else
            while (*next && ui->file_count < DROP_MAX_ATTACHMENTS) {
                WCHAR path[32768];
                if (!drop_path_join(buffer, next, path, 32768)) break;
                (void)drop_ui_draft_add(ui, path);
                next += wcslen(next) + 1;
            }
        swprintf_s(text, 64, L"添加附件（%d）", ui->file_count);
        SetWindowTextW(CONTROL(ui, UI_ATTACH), text);
        files_summary(ui);
        status(ui, ui->file_count == DROP_MAX_ATTACHMENTS ? L"已选择 32 个附件；其它文件未加入。" : L"待发送附件已显示在输入区，点击发送后进入队列。");
    }
    free(buffer);
}
/* 读取受限长度的编辑框文本并转换为 UTF-8；成功缓冲由调用者清零或 free。 */
static char *edit_utf8(drop_ui *ui, int id, size_t max) {
    int length = GetWindowTextLengthW(CONTROL(ui, id));
    WCHAR *wide;
    char *text;
    if (length < 0 || (size_t)length > max) return NULL;
    wide = calloc((size_t)length + 1, sizeof(WCHAR));
    text = malloc(max + 1);
    if (!wide || !text) {
        free(wide);
        free(text);
        return NULL;
    }
    GetWindowTextW(CONTROL(ui, id), wide, length + 1);
    if (!drop_to_utf8(wide, text, max + 1)) {
        free(text);
        text = NULL;
    }
    if (id == UI_PASSWORD) sodium_memzero(wide, ((size_t)length + 1) * sizeof(WCHAR));
    free(wide);
    return text;
}
/* 分配清零的后台任务并设置查询默认范围；尚未提交时由创建方拥有。 */
static ui_job *new_job(ui_job_type type) {
    ui_job *job = calloc(1, sizeof(*job));
    if (job) {
        job->type = type;
        job->to = INT64_MAX;
    }
    return job;
}
/* 普通任务使用单槽，阅读任务按界面操作顺序进入有界队列；阅读不禁用控件，成功后任务归后台所有。 */
static int submit(drop_ui *ui, ui_job *job) {
    int ok = 0;
    int reading = job && job->type == JOB_DETAIL;
    if (!job) {
        status(ui, L"内存不足。请重试。");
        return 0;
    }
    EnterCriticalSection(&ui->lock);
    if (!ui->busy) {
        if (job->type == JOB_DETAIL && ui->read_count < 64) {
            if (ui->last_read)
                ui->last_read->next = job;
            else
                ui->reads = job;
            ui->last_read = job;
            ++ui->read_count;
            ok = 1;
        } else if (job->type != JOB_DETAIL && !ui->job) {
            ui->job = job;
            ui->busy = 1;
            ok = 1;
        }
        if (ok) SetEvent(ui->work_pending);
    }
    LeaveCriticalSection(&ui->lock);
    if (ok) {
        if (!reading) enable(ui);
        SetEvent(ui->wake);
        if (!reading) status(ui, L"后台正在处理，窗口可继续操作…");
    } else {
        drop_ui_job_free(job);
        status(ui, L"后台任务尚未完成，请稍后重试。");
    }
    return ok;
}
/* 从界面取得登录或探测参数，及时清空密码控件，并将任务交给后台处理。 */
static void credentials(drop_ui *ui, ui_job_type type) {
    ui_job *job = new_job(type);
    char *login;
    if (!job) return;
    GetWindowTextW(CONTROL(ui, UI_URL), job->url, 2049);
    GetWindowTextW(CONTROL(ui, UI_CA), job->ca, 32768);
    wcscpy_s(ui->url, 2049, job->url);
    wcscpy_s(ui->ca, 32768, job->ca);
    GetWindowTextW(CONTROL(ui, UI_LOGIN), ui->login, 65);
    job->notifications = ui->notifications;
    job->tray_flash = ui->tray_flash;
    job->autostart = ui->autostart;
    job->use_zstd = ui->use_zstd;
    job->zstd_level = ui->zstd_level;
    login = edit_utf8(ui, UI_LOGIN, 64);
    if (!login) {
        drop_ui_job_free(job);
        return;
    }
    strcpy_s(job->login, sizeof(job->login), login);
    free(login);
    if (type == JOB_LOGIN) {
        job->password = edit_utf8(ui, UI_PASSWORD, 1024);
        SetWindowTextW(CONTROL(ui, UI_PASSWORD), L"");
        if (!*job->login || !job->password || !*job->password) {
            drop_ui_job_free(job);
            status(ui, L"请填写登录名和密码。");
            return;
        }
    }
    (void)submit(ui, job);
}
/* 读取提醒、压缩和自启动选项，构造后台持久保存任务，避免注册表或文件写入阻塞界面。 */
static void settings_save(drop_ui *ui) {
    ui_job *job = new_job(JOB_SETTINGS);
    char *login;
    if (!job) return;
    GetWindowTextW(CONTROL(ui, UI_URL), job->url, 2049);
    GetWindowTextW(CONTROL(ui, UI_CA), job->ca, 32768);
    login = edit_utf8(ui, UI_LOGIN, 64);
    if (!login) {
        drop_ui_job_free(job);
        return;
    }
    strcpy_s(job->login, sizeof(job->login), login);
    free(login);
    job->notifications = SendMessageW(CONTROL(ui, UI_NOTIFICATIONS), BM_GETCHECK, 0, 0) == BST_CHECKED;
    job->tray_flash = SendMessageW(CONTROL(ui, UI_TRAY_FLASH), BM_GETCHECK, 0, 0) == BST_CHECKED;
    job->autostart = SendMessageW(CONTROL(ui, UI_AUTOSTART), BM_GETCHECK, 0, 0) == BST_CHECKED;
    job->previous_autostart = ui->autostart;
    job->use_zstd = SendMessageW(CONTROL(ui, UI_COMPRESSION), CB_GETCURSEL, 0, 0) == 1;
    job->zstd_level = (int)SendMessageW(CONTROL(ui, UI_LEVEL), CB_GETCURSEL, 0, 0) + 1;
    if (!submit(ui, job)) {
        SendMessageW(CONTROL(ui, UI_NOTIFICATIONS), BM_SETCHECK, ui->notifications ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(CONTROL(ui, UI_TRAY_FLASH), BM_SETCHECK, ui->tray_flash ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(CONTROL(ui, UI_AUTOSTART), BM_SETCHECK, ui->autostart ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(CONTROL(ui, UI_COMPRESSION), CB_SETCURSEL, (WPARAM)ui->use_zstd, 0);
        SendMessageW(CONTROL(ui, UI_LEVEL), CB_SETCURSEL, (WPARAM)(ui->zstd_level - 1), 0);
    }
}
/* 解析查询日期并转换为时间边界，结束日期按查询语义处理；非法输入阻止提交。 */
static int date_value(drop_ui *ui, int id, int end, int64_t *value) {
    WCHAR text[16];
    SYSTEMTIME local = {0}, utc;
    FILETIME file;
    ULARGE_INTEGER ticks;
    unsigned year, month, day;
    int i;
    GetWindowTextW(CONTROL(ui, id), text, 16);
    if (!*text) {
        *value = end ? INT64_MAX : 0;
        return 1;
    }
    if (wcslen(text) != 10 || text[4] != L'-' || text[7] != L'-') return 0;
    for (i = 0; i < 10; ++i)
        if (i != 4 && i != 7 && (text[i] < L'0' || text[i] > L'9')) return 0;
    if (swscanf_s(text, L"%4u-%2u-%2u", &year, &month, &day) != 3 || year < 1970 || year > 9999 || month < 1 || month > 12 || day < 1 || day > 31) return 0;
    local.wYear = (WORD)year;
    local.wMonth = (WORD)month;
    local.wDay = (WORD)day;
    if (end) {
        local.wHour = 23;
        local.wMinute = 59;
        local.wSecond = 59;
    }
    if (!TzSpecificLocalTimeToSystemTime(NULL, &local, &utc) || !SystemTimeToFileTime(&utc, &file)) return 0;
    ticks.LowPart = file.dwLowDateTime;
    ticks.HighPart = file.dwHighDateTime;
    *value = (int64_t)(ticks.QuadPart / UINT64_C(10000000)) - (int64_t)UINT64_C(11644473600);
    return *value >= 0;
}
/* 提交分页历史查询；更换过滤条件时重置页码，并记录本次搜索和时间范围。 */
static void history_request(drop_ui *ui, int page, int new_filter) {
    ui_job *job = new_job(JOB_VIEW);
    char *text;
    if (!job) return;
    if (new_filter) {
        text = edit_utf8(ui, UI_SEARCH, 1024);
        if (!text || !date_value(ui, UI_FROM, 0, &ui->from) || !date_value(ui, UI_TO, 1, &ui->to) || ui->from > ui->to) {
            free(text);
            drop_ui_job_free(job);
            status(ui, L"日期应为 YYYY-MM-DD，起日不能晚于止日。");
            return;
        }
        strcpy_s(ui->search, sizeof(ui->search), text);
        free(text);
    }
    job->page = page;
    strcpy_s(job->search, sizeof(job->search), ui->search);
    job->from = ui->from;
    job->to = ui->to;
    strcpy_s(job->peer, DROP_ID_CAP, ui->peer);
    if (submit(ui, job)) ui->page = page;
}
/* 发送目标固定为当前已验证会话，显示标签不参与路由；稳定副本及网络操作由后台完成。 */
static void send_message(drop_ui *ui) {
    ui_job *job = new_job(JOB_SEND);
    int i;
    if (!job) return;
    job->body = edit_utf8(ui, UI_BODY, DROP_MAX_BODY);
    if (!job->body || !drop_id_valid(ui->peer) || (!*job->body && !ui->file_count)) {
        drop_ui_job_free(job);
        status(ui, L"正文不能超过 1 MiB；正文与附件至少填写一项。");
        return;
    }
    strcpy_s(job->id, DROP_ID_CAP, ui->peer);
    for (i = 0; i < ui->file_count; ++i) {
        job->paths[i] = _wcsdup(ui->paths[i]);
        if (!job->paths[i]) {
            job->count = i;
            drop_ui_job_free(job);
            return;
        }
        ++job->count;
    }
    ui->sending = 1;
    if (!submit(ui, job)) ui->sending = 0;
    enable(ui);
}
/* 从已拥有的历史快照立即显示完整正文，后台仅持久记录阅读并同步状态；重复选择保留文字和附件选择。 */
static void detail_request(drop_ui *ui, int index) {
    ui_job *job;
    drop_detail *detail;
    drop_history_row *row;
    if (!ui->history || index < 0 || index >= ui->history->count || ui->busy) return;
    row = &ui->history->rows[index];
    if (!strcmp(ui->selected_id, row->id) && ui->detail) {
        drop_ui_files(ui);
        return;
    }
    detail = calloc(1, sizeof(*detail));
    if (!detail) return;
    detail->text = _wcsdup(row->content->text);
    if (!detail->text) {
        free(detail);
        return;
    }
    detail->count = row->content->count;
    memcpy(detail->files, row->content->files, sizeof(detail->files));
    job = new_job(JOB_DETAIL);
    if (!job) {
        drop_ui_detail_free(detail);
        return;
    }
    strcpy_s(job->id, sizeof(job->id), row->id);
    job->pending = row->pending;
    job->selection = ui->selection + 1;
    if (!submit(ui, job)) {
        drop_ui_detail_free(detail);
        return;
    }
    ++ui->selection;
    strcpy_s(ui->selected_id, DROP_ID_CAP, row->id);
    drop_ui_detail_free(ui->detail);
    ui->detail = detail;
    SetWindowTextW(CONTROL(ui, UI_DETAIL), detail->text);
    SetWindowTextW(CONTROL(ui, UI_DETAIL_LABEL), row->state);
    drop_ui_files(ui);
    enable(ui);
}
/* 确认框期间后台快照可能更新，因此先复制消息 ID；真正删除与归属复核均在工作线程处理。 */
static void delete_message(drop_ui *ui) {
    if (!delete_available(ui)) {
        status(ui, L"只能删除当前账号发送且已与支持删除的服务器同步的消息，其他设备发送的同账号消息也可删除。");
        return;
    }
    ui_job *job = new_job(JOB_DELETE);
    if (!job) return;
    strcpy_s(job->id, DROP_ID_CAP, ui->selected_id);
    if (MessageBoxW(ui->window, L"删除这条消息及其附件关联？\n\n删除会同步到服务器、自己的所有设备及接收方设备。已导出的文件和已有备份保留。", L"删除消息",
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES)
        (void)submit(ui, job);
    else
        drop_ui_job_free(job);
}
/* 通过显式保存框选择附件导出位置，后台完成完整校验和替换写入。 */
static void export_file(drop_ui *ui) {
    int index = ListView_GetNextItem(CONTROL(ui, UI_FILES), -1, LVNI_SELECTED);
    ui_job *job;
    OPENFILENAMEW dialog = {0};
    WCHAR safe[260];
    if (!ui->detail || index < 0 || index >= ui->detail->count) return;
    if (!ui->detail->files[index].complete) {
        status(ui, L"附件尚未完整保存，等待自动下载或点击同步 / 重试。");
        return;
    }
    job = new_job(JOB_EXPORT);
    if (!job) return;
    job->file = ui->detail->files[index];
    drop_ui_safe_filename(job->file.name, safe);
    wcscpy_s(job->destination, 32768, safe);
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = ui->window;
    dialog.lpstrFile = job->destination;
    dialog.nMaxFile = 32768;
    dialog.lpstrFilter = L"所有文件\0*.*\0\0";
    dialog.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&dialog))
        (void)submit(ui, job);
    else
        drop_ui_job_free(job);
}
#if DROP_DIAGNOSTICS
/* 写入诊断构建的通知事件计数，便于隔离测试核对 API 请求行为。 */
static void notification_event(drop_ui *ui, const WCHAR *name) {
    UINT_PTR count = (UINT_PTR)GetPropW(ui->window, name);
    (void)SetPropW(ui->window, name, (HANDLE)(count + 1));
}
/* 记录诊断可观察的提醒状态，不改变 Windows 对通知实际显示的控制。 */
static void notification_state(drop_ui *ui, const WCHAR *name, int enabled) {
    (void)SetPropW(ui->window, name, (HANDLE)(UINT_PTR)(enabled != 0));
}
#else
#define notification_event(ui, name) ((void)0)
#define notification_state(ui, name, enabled) ((void)0)
#endif
/* 准备正常和空白托盘图标，空白图标用于未读闪烁，资源由退出流程销毁。 */
static void tray_icons(drop_ui *ui) {
    UINT dpi = GetDpiForSystem();
    int width = GetSystemMetricsForDpi(SM_CXSMICON, dpi), height = GetSystemMetricsForDpi(SM_CYSMICON, dpi);
    size_t mask_size;
    unsigned char *mask, *pixels;
    if (ui->tray_blank_icon) DestroyIcon(ui->tray_blank_icon);
    ui->tray_blank_icon = NULL;
    ui->tray_icon = (HICON)LoadImageW(ui->instance, MAKEINTRESOURCEW(IDI_DROP_APP), IMAGE_ICON, width, height, LR_SHARED);
    if (width < 1 || height < 1 || width > 256 || height > 256) return;
    mask_size = (size_t)((width + 15) / 16) * 2 * (size_t)height;
    mask = malloc(mask_size);
    pixels = calloc((size_t)width * (size_t)height, 4);
    if (mask && pixels) {
        memset(mask, 0xff, mask_size);
        ui->tray_blank_icon = CreateIcon(ui->instance, width, height, 1, 32, mask, pixels);
    }
    free(mask);
    free(pixels);
}
/* 添加或更新托盘图标，并按窗口隐藏和通知配置决定是否请求气泡提醒。 */
static int tray(drop_ui *ui, DWORD operation, int unread, int notify) {
    NOTIFYICONDATAW icon = {0};
    icon.cbSize = sizeof(icon);
    icon.hWnd = ui->window;
    icon.uID = 1;
    icon.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    icon.uCallbackMessage = DROP_UI_TRAY;
    icon.hIcon = ui->tray_blank && ui->tray_blank_icon ? ui->tray_blank_icon : ui->tray_icon;
    swprintf_s(icon.szTip, 128, L"Drop " DROP_VERSION_WIDE L" · 未读 %d · 关闭窗口继续同步", unread);
    if (operation != NIM_DELETE && notify > 0 && unread > 0 && ui->notifications && !IsWindowVisible(ui->window)) {
        icon.uFlags |= NIF_INFO | NIF_REALTIME;
        wcscpy_s(icon.szInfoTitle, 64, L"Drop 新消息");
        swprintf_s(icon.szInfo, 256, L"收到 %d 条新记录，正在自动保存正文及完整附件。", notify);
        icon.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    }
    if (!Shell_NotifyIconW(operation, &icon)) return 0;
    if (icon.uFlags & NIF_INFO) {
        ui->balloon_pending = 1;
        notification_event(ui, L"DROP_TEST_BALLOON_REQUESTS");
    }
    if (operation == NIM_MODIFY && ui->tray_flashing) notification_event(ui, ui->tray_blank ? L"DROP_TEST_TRAY_BLANK_FRAMES" : L"DROP_TEST_TRAY_ICON_FRAMES");
    if (operation == NIM_ADD) {
        icon.uVersion = NOTIFYICON_VERSION_4;
        (void)Shell_NotifyIconW(NIM_SETVERSION, &icon);
        ui->tray_added = 1;
    }
    if (operation == NIM_DELETE) ui->tray_added = 0;
    return 1;
}
/* 清除本实例待显示的托盘气泡，避免窗口重新打开后继续弹出旧提醒。 */
static void balloon_clear(drop_ui *ui) {
    NOTIFYICONDATAW icon = {0};
    if (!ui->balloon_pending) return;
    icon.cbSize = sizeof(icon);
    icon.hWnd = ui->window;
    icon.uID = 1;
    icon.uFlags = NIF_INFO;
    (void)Shell_NotifyIconW(NIM_MODIFY, &icon);
    ui->balloon_pending = 0;
}
/* 停止本窗口的任务栏闪烁并重置记录状态，供激活窗口或未读清零时调用。 */
static void taskbar_stop(drop_ui *ui) {
    FLASHWINFO info = {sizeof(info), ui->window, FLASHW_STOP, 0, 0};
    if (ui->taskbar_flashing) {
        (void)FlashWindowEx(&info);
        ui->taskbar_flashing = 0;
    }
    notification_state(ui, L"DROP_TEST_TASKBAR_ACTIVE", 0);
}
/* 根据窗口可见性、未读数和两个独立开关更新托盘及任务栏提醒；系统决定通知最终显示效果。 */
static void attention(drop_ui *ui, int new_messages) {
    int unread = ui->ready && ui->history ? ui->history->unread : 0, visible = IsWindowVisible(ui->window) != 0;
    if (!unread || visible || !ui->notifications) balloon_clear(ui);
    if (!unread || !visible)
        taskbar_stop(ui);
    else if (new_messages > 0) {
        FLASHWINFO info = {sizeof(info), ui->window, FLASHW_TRAY | FLASHW_TIMERNOFG, 0, 0};
        (void)FlashWindowEx(&info);
        ui->taskbar_flashing = 1;
        notification_event(ui, L"DROP_TEST_TASKBAR_REQUESTS");
        notification_state(ui, L"DROP_TEST_TASKBAR_ACTIVE", 1);
    }
    if (unread && !visible && ui->tray_flash && ui->tray_added && ui->tray_blank_icon) {
        if (!ui->tray_flashing && SetTimer(ui->window, TRAY_TIMER, 500, NULL)) ui->tray_flashing = 1;
    } else if (ui->tray_flashing) {
        KillTimer(ui->window, TRAY_TIMER);
        ui->tray_flashing = 0;
        ui->tray_blank = 0;
        if (ui->tray_added) (void)tray(ui, NIM_MODIFY, unread, 0);
    }
    notification_state(ui, L"DROP_TEST_TRAY_ACTIVE", ui->tray_flashing);
    notification_state(ui, L"DROP_TEST_TRAY_BLANK", ui->tray_blank);
}
/* 恢复并激活消息窗口，清除托盘气泡和任务栏提示，再刷新提醒状态。 */
static void show(drop_ui *ui) {
    ShowWindow(ui->window, SW_RESTORE);
    balloon_clear(ui);
    attention(ui, 0);
    taskbar_stop(ui);
    SetForegroundWindow(ui->window);
}
/* 在界面线程接收后台结果，更新历史、详情及任务状态，并释放或接管结果中的数据。 */
/* 比较实际显示的消息字段；没有变化时保留 RichEdit 的文字选择，避免定时同步打断复制。 */
static int history_same(const drop_history *left, const drop_history *right) {
    if (!left || !right || left->count != right->count || strcmp(left->peer, right->peer)) return 0;
    for (int i = 0; i < left->count; ++i) {
        const drop_history_row *a = &left->rows[i], *b = &right->rows[i];
        if (strcmp(a->id, b->id) || a->unread != b->unread || a->pending != b->pending || a->mine != b->mine || wcscmp(a->time, b->time) || wcscmp(a->sender, b->sender) ||
            wcscmp(a->state, b->state) || wcsncmp(a->content->text, b->content->text, 320) || a->content->count != b->content->count ||
            memcmp(a->content->files, b->content->files, (size_t)a->content->count * sizeof(drop_file_view)))
            return 0;
    }
    return 1;
}
/* 在界面线程接管后台快照，保留稳定消息选择，更新下载状态并释放结果的其余资源。 */
static void display_result(drop_ui *ui, ui_result *result) {
    int i;
    WCHAR title[192];
    ui->ready = result->ready;
    SetWindowTextW(CONTROL(ui, UI_OVERVIEW), ui->ready ? L"本机消息 · 已登录 · 自动同步" : L"本机消息 · 请在配置中登录");
    if (result->task_done && result->type != JOB_DETAIL) {
        ui->busy = 0;
        if (result->type == JOB_SEND) ui->sending = 0;
    }
    if (result->type == JOB_SETTINGS) {
        if (result->settings_saved) {
            ui->notifications = result->notifications;
            ui->tray_flash = result->tray_flash;
            ui->autostart = result->autostart;
            ui->use_zstd = result->use_zstd;
            ui->zstd_level = result->zstd_level;
        }
        SendMessageW(CONTROL(ui, UI_NOTIFICATIONS), BM_SETCHECK, ui->notifications ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(CONTROL(ui, UI_TRAY_FLASH), BM_SETCHECK, ui->tray_flash ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(CONTROL(ui, UI_AUTOSTART), BM_SETCHECK, ui->autostart ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(CONTROL(ui, UI_COMPRESSION), CB_SETCURSEL, (WPARAM)ui->use_zstd, 0);
        SendMessageW(CONTROL(ui, UI_LEVEL), CB_SETCURSEL, (WPARAM)(ui->zstd_level - 1), 0);
    }
    if (result->queued) {
        SetWindowTextW(CONTROL(ui, UI_BODY), L"");
        files_clear(ui);
    }
    if (result->history) {
        int redraw = !history_same(ui->history, result->history);
        ui->more = result->history->more;
        drop_ui_history_free(ui->history);
        ui->history = result->history;
        result->history = NULL;
        strcpy_s(ui->peer, DROP_ID_CAP, ui->history->peer);
        if (result->type == JOB_CONTACT || result->type == JOB_LOGIN) {
            ui->page = 0;
            ui->search[0] = 0;
            ui->from = 0;
            ui->to = INT64_MAX;
            SetWindowTextW(CONTROL(ui, UI_SEARCH), L"");
            SetWindowTextW(CONTROL(ui, UI_FROM), L"");
            SetWindowTextW(CONTROL(ui, UI_TO), L"");
        }
        const WCHAR *peer_name = L"当前会话";
        for (i = 0; i < ui->history->peer_count; ++i) {
            if (strcmp(ui->peer, ui->history->peers[i].id)) continue;
            peer_name = ui->history->peers[i].name;
            strcpy_s(ui->recipient_id, DROP_ID_CAP, ui->peer);
            swprintf_s(title, 192, !strcmp(ui->peer, ui->history->self) ? L"发送到：我的设备（自己） · %s" : L"发送给：%s（%s）", peer_name, ui->history->peers[i].login);
            SetWindowTextW(CONTROL(ui, UI_RECIPIENT), title);
            SetWindowTextW(CONTROL(ui, UI_SEND), !strcmp(ui->peer, ui->history->self) ? L"发给自己" : L"发送");
        }
        swprintf_s(title, 192, !strcmp(ui->peer, ui->history->self) ? L"我的设备 · %s · 第 %d 页" : L"%s · 第 %d 页", peer_name, ui->page + 1);
        SetWindowTextW(CONTROL(ui, UI_OVERVIEW), title);
        swprintf_s(
            title, 96, L"未读 %d · 待发送 %d\r\n附件待完成 %d · 待删除 %d", ui->history->unread, ui->history->pending_count, ui->history->downloads, ui->history->pending_deletes);
        SetWindowTextW(CONTROL(ui, UI_HISTORY_LABEL), title);
        if (redraw) drop_ui_messages(ui);
        drop_ui_users(ui);
        swprintf_s(title, 96, L"Drop " DROP_VERSION_WIDE L" · 第 %d 页 · 未读 %d", ui->page + 1, ui->history->unread);
        SetWindowTextW(ui->window, title);
        (void)tray(ui, ui->tray_added ? NIM_MODIFY : NIM_ADD, ui->history->unread, ui->history->new_notifications);
        attention(ui, ui->history->new_notifications);
    } else
        attention(ui, 0);
    if (result->detail && result->selection == ui->selection) {
        int changed = !ui->detail || wcscmp(ui->detail->text, result->detail->text);
        drop_ui_detail_free(ui->detail);
        ui->detail = result->detail;
        result->detail = NULL;
        if (changed) SetWindowTextW(CONTROL(ui, UI_DETAIL), ui->detail->text);
    }
    if (ui->history && *ui->selected_id) {
        int found = 0;
        for (i = 0; i < ui->history->count; ++i) {
            drop_history_row *row = &ui->history->rows[i];
            if (strcmp(ui->selected_id, row->id)) continue;
            found = 1;
            SetWindowTextW(CONTROL(ui, UI_DETAIL_LABEL), row->state);
            if (ui->detail) {
                ui->detail->count = row->content->count;
                memcpy(ui->detail->files, row->content->files, sizeof(ui->detail->files));
            }
        }
        if (!found) {
            ui->selected_id[0] = 0;
            ++ui->selection;
            drop_ui_detail_free(ui->detail);
            ui->detail = NULL;
            SetWindowTextW(CONTROL(ui, UI_DETAIL), L"");
            SetWindowTextW(CONTROL(ui, UI_DETAIL_LABEL), L"所选消息与附件");
        }
    }
    drop_ui_files(ui);
    status(ui, result->status);
    enable(ui);
    drop_ui_result_free(result);
}
/* 通知后台停止并取消网络连接，等工作线程结束后销毁本实例资源；持久队列留给下次启动恢复。 */
static void exit_app(drop_ui *ui) {
    MSG message;
    KillTimer(ui->window, TRAY_TIMER);
    taskbar_stop(ui);
    balloon_clear(ui);
    SetEvent(ui->stop);
    SetEvent(ui->wake);
    if (ui->network_notification) {
        CancelMibChangeNotify2(ui->network_notification);
        ui->network_notification = NULL;
    }
    drop_ui_cancel_ws(ui);
    if (ui->tray_added) (void)tray(ui, NIM_DELETE, 0, 0);
    /* 仅退出当前 Drop 实例；持久队列和分块文件在下次启动时恢复。 */
    if (ui->worker && WaitForSingleObject(ui->worker, 3000) != WAIT_OBJECT_0) ExitProcess(0);
    if (!drop_ui_compose_close(ui)) ExitProcess(0);
    while (PeekMessageW(&message, ui->window, DROP_UI_RESULT, DROP_UI_RESULT, PM_REMOVE))
        drop_ui_result_free((ui_result *)message.lParam);
    DestroyWindow(ui->window);
}
/* 分派窗口、托盘、控件和后台结果消息；关闭或最小化保留托盘，真正退出才停止同步。 */
static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    drop_ui *ui = (drop_ui *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        ui = ((CREATESTRUCTW *)lparam)->lpCreateParams;
        ui->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)ui);
    }
    if (!ui) return DefWindowProcW(window, message, wparam, lparam);
    if (message == ui->taskbar_created && ui->taskbar_created) {
        ui->tray_added = 0;
        ui->tray_blank = 0;
        ui->balloon_pending = 0;
        (void)tray(ui, NIM_ADD, ui->history ? ui->history->unread : 0, 0);
        attention(ui, 0);
        return 0;
    }
    switch (message) {
#if DROP_DIAGNOSTICS
    case DROP_UI_TEST_DPI:
        drop_ui_test_dpi(window, (UINT)wparam, LOWORD(lparam), HIWORD(lparam));
        return 0;
#endif
    case WM_CREATE:
        ui->dpi = GetDpiForWindow(window);
        drop_ui_dpi(ui, window, ui->dpi);
        if (!drop_ui_configuration_create(ui)) return -1;
        menu_create(ui);
        create_controls(ui);
        drop_ui_theme(ui);
        drop_ui_compose_create(ui);
        drop_ui_draft_refresh(ui);
        tray_icons(ui);
        DragAcceptFiles(window, TRUE);
        (void)tray(ui, NIM_ADD, 0, 0);
        return 0;
    case WM_MOUSEMOVE:
        if (ui->dragging) {
            ui->divider = MulDiv(GET_X_LPARAM(lparam), 96, (int)ui->dpi);
            layout(ui, 0, 0);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (ui->dragging) {
            ui->dragging = 0;
            ReleaseCapture();
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        ui->dragging = 0;
        break;
    case WM_DROPFILES: {
        drop_ui_draft_drop(ui, (HDROP)wparam);
        return 0;
    }
    case WM_SIZE:
        if (wparam == SIZE_MINIMIZED && ui->tray_added) {
            drop_ui_configuration_show(ui, 0);
            ShowWindow(window, SW_HIDE);
            attention(ui, 0);
            return 0;
        }
        layout(ui, LOWORD(lparam), HIWORD(lparam));
        return 0;
    case WM_SHOWWINDOW:
        PostMessageW(window, DROP_UI_VISIBILITY, 0, 0);
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
        return drop_ui_control_color(ui, (HDC)wparam, (HWND)lparam);
    case WM_ERASEBKGND: {
        RECT rect;
        GetClientRect(window, &rect);
        FillRect((HDC)wparam, &rect, ui->brushes[DROP_UI_BACKGROUND]);
        return 1;
    }
    case WM_DRAWITEM:
        if (drop_ui_draw_button(ui, (const DRAWITEMSTRUCT *)lparam)) return TRUE;
        break;
    case WM_SYSCOLORCHANGE:
    case WM_SETTINGCHANGE:
        drop_ui_theme(ui);
        drop_ui_messages(ui);
        break;
    case WM_ACTIVATE:
        if (LOWORD(wparam) != WA_INACTIVE) taskbar_stop(ui);
        break;
    case WM_TIMER:
        if (wparam == TRAY_TIMER) {
            attention(ui, 0);
            if (ui->tray_flashing) {
                ui->tray_blank = !ui->tray_blank;
                (void)tray(ui, NIM_MODIFY, ui->history->unread, 0);
                notification_state(ui, L"DROP_TEST_TRAY_BLANK", ui->tray_blank);
            }
            return 0;
        }
        break;
    case WM_GETMINMAXINFO:
        drop_ui_minimum(ui, window, (MINMAXINFO *)lparam, 840, 360);
        return 0;
    case WM_DPICHANGED: {
        RECT *rect = (RECT *)lparam;
        drop_ui_dpi(ui, window, HIWORD(wparam));
        SetWindowPos(window, NULL, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout(ui, 0, 0);
        drop_ui_messages(ui);
        InvalidateRect(window, NULL, TRUE);
        return 0;
    }
    case WM_MOUSEWHEEL:
    case WM_VSCROLL:
        drop_ui_scroll(ui, window, message, wparam);
        return 0;
    case WM_CLOSE:
        if (!ui->tray_added && !tray(ui, NIM_ADD, 0, 0)) {
            status(ui, L"托盘不可用，窗口保持打开。真正退出按钮可停止同步。");
            return 0;
        }
        drop_ui_configuration_show(ui, 0);
        ShowWindow(window, SW_HIDE);
        attention(ui, 0);
        return 0;
    case DROP_UI_SHOW:
        show(ui);
        return 0;
    case DROP_UI_VISIBILITY:
        attention(ui, 0);
        return 0;
    case DROP_UI_RESULT:
        display_result(ui, (ui_result *)lparam);
        return 0;
    case DROP_UI_DRAFT_ICONS:
        drop_ui_draft_icons(ui, (void *)lparam);
        return 0;
    case DROP_UI_SELECT_MESSAGE:
        if (!ui->busy) {
            ui->attachment_selection = (int)lparam;
            detail_request(ui, (int)wparam);
        }
        return 0;
    case DROP_UI_TRAY:
        if (LOWORD(lparam) == NIN_BALLOONHIDE || LOWORD(lparam) == NIN_BALLOONTIMEOUT) ui->balloon_pending = 0;
        if (LOWORD(lparam) == NIN_SELECT || LOWORD(lparam) == NIN_KEYSELECT || LOWORD(lparam) == WM_LBUTTONDBLCLK || LOWORD(lparam) == NIN_BALLOONUSERCLICK)
            show(ui);
        else if (LOWORD(lparam) == WM_CONTEXTMENU || LOWORD(lparam) == WM_RBUTTONUP) {
            HMENU menu = CreatePopupMenu();
            POINT point;
            GetCursorPos(&point);
            AppendMenuW(menu, MF_STRING, TRAY_OPEN, L"打开 Drop");
            AppendMenuW(menu, MF_STRING, TRAY_EXIT, L"真正退出（停止同步）");
            SetForegroundWindow(window);
            TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, window, NULL);
            DestroyMenu(menu);
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == UI_USER_SEARCH && HIWORD(wparam) == EN_CHANGE) {
            drop_ui_users(ui);
            return 0;
        }
        switch (LOWORD(wparam)) {
        case UI_ADD_USER: {
            ui_job *job = new_job(JOB_CONTACT);
            char *login = edit_utf8(ui, UI_USER_SEARCH, 64);
            if (job && login && *login) {
                strcpy_s(job->recipient, 65, login);
                (void)submit(ui, job);
            } else {
                drop_ui_job_free(job);
                status(ui, L"在左侧输入准确的登录账号，再点击查找账号。");
            }
            free(login);
            break;
        }
        case UI_RESET_FILTER:
            SetWindowTextW(CONTROL(ui, UI_SEARCH), L"");
            SetWindowTextW(CONTROL(ui, UI_FROM), L"");
            SetWindowTextW(CONTROL(ui, UI_TO), L"");
            history_request(ui, 0, 1);
            break;
        case UI_CONFIGURATION:
            drop_ui_configuration_show(ui, 1);
            break;
        case UI_SAVE_CONFIG:
            settings_save(ui);
            break;
        case TRAY_OPEN:
            show(ui);
            break;
        case TRAY_EXIT:
        case UI_EXIT:
            exit_app(ui);
            break;
        case UI_PROBE:
            credentials(ui, JOB_PROBE);
            break;
        case UI_SIGNIN:
            credentials(ui, JOB_LOGIN);
            break;
        case UI_SEND:
            if (ui->ready && !ui->busy && recipient_available(ui)) send_message(ui);
            break;
        case UI_ATTACH:
            if (!ui->sending) files_choose(ui);
            break;
        case UI_CLEAR:
            files_clear(ui);
            break;
        case UI_REMOVE_ATTACH:
            drop_ui_draft_remove(ui);
            break;
        case UI_SYNC:
            if (ui->ready && !ui->busy) (void)submit(ui, new_job(JOB_NONE));
            break;
        case UI_RESTORE_RETRY:
            if (MessageBoxW(ui->window, L"服务器恢复后，部分发送任务的接收结果可能不确定。请先核对本机记录与服务器历史。确认重新提交全部待核对任务？", L"重新提交待核对任务",
                    MB_YESNO | MB_ICONQUESTION) == IDYES)
                (void)submit(ui, new_job(JOB_RESTORE_RETRY));
            break;
        case UI_LOGOUT:
            (void)submit(ui, new_job(JOB_LOGOUT));
            break;
        case UI_FIND:
            history_request(ui, 0, 1);
            break;
        case UI_PREVIOUS:
            if (ui->page > 0) history_request(ui, ui->page - 1, 0);
            break;
        case UI_NEXT:
            if (ui->more) history_request(ui, ui->page + 1, 0);
            break;
        case UI_EXPORT:
            if (ui->ready && !ui->busy) export_file(ui);
            break;
        case UI_DELETE_MESSAGE:
            delete_message(ui);
            break;
        case UI_NOTIFICATIONS:
        case UI_TRAY_FLASH:
        case UI_AUTOSTART:
            settings_save(ui);
            break;
        case UI_COMPRESSION:
        case UI_LEVEL:
            if (HIWORD(wparam) == CBN_SELCHANGE) settings_save(ui);
            break;
        }
        return 0;
    case WM_NOTIFY: {
        NMHDR *header = (NMHDR *)lparam;
        if (header->idFrom == UI_USERS && header->code == LVN_ITEMCHANGED && !ui->rebuilding && !ui->busy && ui->history) {
            NMLISTVIEW *change = (NMLISTVIEW *)lparam;
            if ((change->uNewState & LVIS_SELECTED) && !(change->uOldState & LVIS_SELECTED)) {
                LVITEMW item = {0};
                item.mask = LVIF_PARAM;
                item.iItem = change->iItem;
                if (!ListView_GetItem(CONTROL(ui, UI_USERS), &item)) return 0;
                int index = (int)item.lParam;
                if (index < 0 || index >= ui->history->peer_count) return 0;
                drop_peer *peer = &ui->history->peers[index];
                strcpy_s(ui->peer, DROP_ID_CAP, peer->id);
                strcpy_s(ui->recipient_id, DROP_ID_CAP, peer->id);
                enable(ui);
                ui->selected_id[0] = 0;
                ++ui->selection;
                drop_ui_detail_free(ui->detail);
                ui->detail = NULL;
                SetWindowTextW(CONTROL(ui, UI_DETAIL), L"");
                drop_ui_files(ui);
                history_request(ui, 0, 0);
            }
        } else if (header->idFrom == UI_FILES && header->code == LVN_ITEMCHANGED && !ui->rebuilding) {
            NMLISTVIEW *change = (NMLISTVIEW *)lparam;
            if (change->uNewState & LVIS_SELECTED) ui->attachment_selection = change->iItem;
        }
        return 0;
    }
    case WM_POWERBROADCAST:
        if (wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND) drop_ui_resume(ui);
        return TRUE;
    case WM_DESTROY:
        KillTimer(window, TRAY_TIMER);
        taskbar_stop(ui);
        if (ui->tray_added) (void)tray(ui, NIM_DELETE, 0, 0);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
/* 网络接口变化回调只唤醒同步，不直接读写界面或数据库。 */
static void CALLBACK network_changed(void *context, PMIB_IPINTERFACE_ROW row, MIB_NOTIFICATION_TYPE type) {
    drop_ui *ui = context;
    (void)row;
    (void)type;
    if (WaitForSingleObject(ui->stop, 0) == WAIT_TIMEOUT) drop_ui_resume(ui);
}
/* 按规范化数据目录保证单实例，初始化原生窗口、网络通知和后台线程，并运行消息循环及统一清理。 */
int drop_ui_run(HINSTANCE instance, const WCHAR *root, int hidden) {
    drop_ui *ui = calloc(1, sizeof(*ui));
    WNDCLASSEXW cls = {0};
    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    HACCEL accelerators = NULL;
    ACCEL keys[] = {{FVIRTKEY | FCONTROL, VK_RETURN, UI_SEND}, {FVIRTKEY, VK_F5, UI_SYNC}};
    MSG message;
    unsigned char hash[32];
    char *utf8 = NULL, hex[65];
    WCHAR wide[65], mutex_name[96];
    DWORD length;
    int result = 1, ole = 0;
    if (!ui) return 1;
    ui->instance = instance;
    ui->divider = 228;
    ui->hidden = hidden;
    if (root)
        length = GetFullPathNameW(root, 32768, ui->root, NULL);
    else {
        length = drop_program_data_directory(L"Drop", ui->root, 32768, 1) ? (DWORD)wcslen(ui->root) : 0;
    }
    if (!length || length >= 32768 || !drop_private_directory(ui->root)) goto done;
    utf8 = malloc(131072);
    if (!utf8 || !drop_to_utf8(ui->root, utf8, 131072)) goto done;
    /* 忽略大小写的 Windows 目录路径映射到一个界面实例和一个同步线程。 */
    CharLowerBuffW(ui->root, (DWORD)wcslen(ui->root));
    crypto_hash_sha256(hash, (const unsigned char *)ui->root, wcslen(ui->root) * sizeof(WCHAR));
    sodium_bin2hex(hex, sizeof(hex), hash, 32);
    drop_to_wide(hex, wide, 65);
    swprintf_s(ui->class_name, 80, L"DropWindow-%.16s", wide);
    swprintf_s(mutex_name, 96, L"Local\\%s", ui->class_name);
    ui->singleton = CreateMutexW(NULL, FALSE, mutex_name);
    if (!ui->singleton) goto done;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(ui->class_name, NULL);
        if (existing) PostMessageW(existing, DROP_UI_SHOW, 0, 0);
        result = 0;
        goto done;
    }
    if (!drop_ui_settings_load(ui)) {
        ui->notifications = 1;
        ui->tray_flash = 1;
        ui->to = INT64_MAX;
    }
    /* manifest 在进程启动前声明逐屏 V2；显式调用只为未使用 manifest 的开发启动提供相同上下文。 */
    (void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    ui->dpi = GetDpiForSystem();
    ui->message_controls = LoadLibraryExW(L"Msftedit.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ui->message_controls) goto done;
    if (FAILED(OleInitialize(NULL))) goto done;
    ole = 1;
    InitCommonControlsEx(&controls);
    ui->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    ui->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    ui->work_pending = CreateEventW(NULL, TRUE, FALSE, NULL);
    InitializeCriticalSection(&ui->lock);
    if (!ui->stop || !ui->wake || !ui->work_pending) goto initialized;
    ui->taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    cls.cbSize = sizeof(cls);
    cls.hInstance = instance;
    cls.lpfnWndProc = window_proc;
    cls.lpszClassName = ui->class_name;
    cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_DROP_APP));
    cls.hbrBackground = NULL;
    cls.hIconSm = (HICON)LoadImageW(instance, MAKEINTRESOURCEW(IDI_DROP_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    if (!RegisterClassExW(&cls)) goto initialized;
    ui->window = CreateWindowExW(WS_EX_CONTROLPARENT, ui->class_name, L"Drop " DROP_VERSION_WIDE, WS_OVERLAPPEDWINDOW | WS_VSCROLL, CW_USEDEFAULT, CW_USEDEFAULT,
        drop_ui_scale(ui->dpi, 980), drop_ui_scale(ui->dpi, 700), NULL, NULL, instance, ui);
    if (!ui->window) goto initialized;
    drop_ui_place(ui, ui->window, 1200, 800);
    accelerators = CreateAcceleratorTableW(keys, sizeof(keys) / sizeof(keys[0]));
    ui->worker = CreateThread(NULL, 0, drop_ui_worker, ui, 0, NULL);
    if (!ui->worker) {
        DestroyWindow(ui->window);
        goto initialized;
    }
    if (NotifyIpInterfaceChange(AF_UNSPEC, network_changed, ui, FALSE, &ui->network_notification) != NO_ERROR) ui->network_notification = NULL;
    if (!hidden || !ui->tray_added) {
        ShowWindow(ui->window, SW_SHOW);
        UpdateWindow(ui->window);
        if (!*ui->url || !*ui->login) drop_ui_configuration_show(ui, 1);
    }
    while (GetMessageW(&message, NULL, 0, 0) > 0) {
        if (accelerators && GetAncestor(message.hwnd, GA_ROOT) == ui->window && TranslateAcceleratorW(ui->window, accelerators, &message)) continue;
        if (!(IsWindowVisible(ui->configuration) && IsDialogMessageW(ui->configuration, &message)) && !IsDialogMessageW(ui->window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    result = 0;
initialized:
    if (ui->stop) SetEvent(ui->stop);
    if (!drop_ui_compose_close(ui)) ExitProcess(0);
    if (ui->network_notification) CancelMibChangeNotify2(ui->network_notification);
    if (ui->worker) CloseHandle(ui->worker);
    if (ui->stop) CloseHandle(ui->stop);
    if (ui->wake) CloseHandle(ui->wake);
    if (ui->work_pending) CloseHandle(ui->work_pending);
    DeleteCriticalSection(&ui->lock);
    drop_ui_job_free(ui->job);
    while (ui->reads) {
        ui_job *next = ui->reads->next;
        drop_ui_job_free(ui->reads);
        ui->reads = next;
    }
    files_clear(ui);
    drop_ui_history_free(ui->history);
    drop_ui_detail_free(ui->detail);
    if (ui->font) DeleteObject(ui->font);
    if (ui->configuration_font) DeleteObject(ui->configuration_font);
    if (ui->tray_blank_icon) DestroyIcon(ui->tray_blank_icon);
done:
    drop_ui_theme_close(ui);
    if (accelerators) DestroyAcceleratorTable(accelerators);
    if (ui->message_controls) FreeLibrary(ui->message_controls);
    if (ui->singleton) CloseHandle(ui->singleton);
    if (ole) OleUninitialize();
    free(utf8);
    free(ui);
    return result;
}
