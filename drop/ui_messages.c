/* 原生消息文本、参与者和附件列表；仅由界面线程调用，网络与数据库快照由后台构造。 */
#include "ui_internal.h"
#include <richedit.h>
#include <windowsx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CONTROL(ui, id) ((ui)->controls[(id) - UI_URL])

/* RichEdit 的内部位置按 UTF-16 字符计数，不使用带 CRLF 展开的窗口文本长度。 */
static LONG text_length(HWND window) {
    GETTEXTLENGTHEX request = {GTL_PRECISE | GTL_NUMCHARS, 1200};
    return (LONG)SendMessageW(window, EM_GETTEXTLENGTHEX, (WPARAM)&request, 0);
}
/* 追加一段纯文本并设置颜色、粗体和可点击属性；不把消息内容解释为 RTF 或网址。 */
static void append(HWND window, const WCHAR *text, COLORREF color, int bold, int link) {
    LONG end = text_length(window);
    CHARFORMAT2W format = {0};
    SendMessageW(window, EM_SETSEL, end, end);
    format.cbSize = sizeof(format);
    format.dwMask = CFM_COLOR | CFM_BOLD | CFM_LINK;
    format.crTextColor = color;
    format.dwEffects = (bold ? CFE_BOLD : 0) | (link ? CFE_LINK : 0);
    SendMessageW(window, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&format);
    SendMessageW(window, EM_REPLACESEL, FALSE, (LPARAM)text);
}
/* 将字符位置映射到所属消息和附件，点击仅选中，不触发文件执行或隐式导出。 */
static void select_position(drop_ui *ui, LONG position) {
    if (!ui->history || ui->rebuilding) return;
    for (int i = 0; i < ui->history->count; ++i) {
        if (position < ui->message_start[i] || position >= ui->message_end[i]) continue;
        int file = -1;
        for (int j = 0; j < ui->history->rows[i].content->count; ++j) {
            if (position >= ui->attachment_start[i][j] && position < ui->attachment_end[i][j]) file = j;
        }
        SendMessageW(ui->window, DROP_UI_SELECT_MESSAGE, (WPARAM)i, (LPARAM)file);
        return;
    }
}
/* 原生只读编辑控件保留文本选择与复制；鼠标和 Home/End 将选择关联到完整详情。 */
static LRESULT CALLBACK message_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR subclass, DWORD_PTR data) {
    drop_ui *ui = (drop_ui *)data;
    LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    (void)subclass;
    if (message == WM_LBUTTONUP) {
        POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        select_position(ui, (LONG)SendMessageW(window, EM_CHARFROMPOS, 0, (LPARAM)&point));
    } else if (message == WM_KEYDOWN && ui->history && ui->history->count && (wparam == VK_HOME || wparam == VK_END)) {
        int row = wparam == VK_HOME ? ui->history->count - 1 : 0;
        select_position(ui, ui->message_start[row]);
    }
    return result;
}
/* 配置原生 RichEdit 的只读、自动换行与字体，禁用自动网址识别，收到的正文仅是文本。 */
void drop_ui_message_control(drop_ui *ui) {
    HWND window = CONTROL(ui, UI_HISTORY);
    SendMessageW(window, EM_EXLIMITTEXT, 0, 64 * 1024 * 1024);
    SendMessageW(window, EM_AUTOURLDETECT, FALSE, 0);
    SendMessageW(window, EM_SETBKGNDCOLOR, 0, GetSysColor(COLOR_WINDOW));
    SetWindowSubclass(window, message_proc, 1, (DWORD_PTR)ui);
}
/* 每条消息保留发送者、秒级时间及附件元数据；长正文有明确提示，并可在下方读取全部内容。 */
void drop_ui_messages(drop_ui *ui) {
    HWND window = CONTROL(ui, UI_HISTORY);
    POINT scroll = {0};
    CHARRANGE selection = {0};
    WCHAR header[320], preview[321], file_text[1200];
    SCROLLINFO info = {sizeof(info), SIF_ALL, 0, 0, 0, 0, 0};
    GetScrollInfo(window, SB_VERT, &info);
    int follow = ui->message_end[0] == 0 || info.nPos + (int)info.nPage >= info.nMax;
    ui->rebuilding = 1;
    SendMessageW(window, EM_GETSCROLLPOS, 0, (LPARAM)&scroll);
    SendMessageW(window, EM_EXGETSEL, 0, (LPARAM)&selection);
    SendMessageW(window, WM_SETREDRAW, FALSE, 0);
    SendMessageW(window, EM_SETREADONLY, FALSE, 0);
    SetWindowTextW(window, L"");
    SendMessageW(window, WM_SETFONT, (WPARAM)ui->font, FALSE);
    if (ui->history) {
        for (int i = ui->history->count - 1; i >= 0; --i) {
            drop_history_row *row = &ui->history->rows[i];
            drop_detail *content = row->content;
            const WCHAR *saved = row->pending ? L"恢复后待核对" : wcsstr(row->state, L"附件待完成") ? L"附件待下载" : L"本机已保存";
            ui->message_start[i] = text_length(window);
            swprintf_s(header, 320, L"%s  %s  ·  %s%s\r\n", row->sender, row->time, saved,
                row->mine ? (wcsstr(row->state, L"账号已读") ? L" · 账号已读" : L" · 账号未读") : (row->unread ? L" · 未读" : L""));
            append(window, header, row->mine ? RGB(32, 79, 124) : RGB(70, 75, 82), 1, 0);
            wcsncpy_s(preview, 321, content->text, _TRUNCATE);
            size_t length = wcslen(preview);
            if (length && preview[length - 1] >= 0xd800 && preview[length - 1] <= 0xdbff) preview[length - 1] = 0;
            if (*preview) append(window, preview, GetSysColor(COLOR_WINDOWTEXT), 0, 0);
            if (wcslen(content->text) > 320) append(window, L"…（选中消息后在下方查看完整正文）", RGB(95, 100, 108), 0, 0);
            if (*preview) append(window, L"\r\n", GetSysColor(COLOR_WINDOWTEXT), 0, 0);
            for (int j = 0; j < content->count; ++j) {
                drop_file_view *file = &content->files[j];
                ui->attachment_start[i][j] = text_length(window);
                swprintf_s(file_text, 1200, L"    附件：%s  ·  %llu 字节  ·  %s\r\n", file->name, (unsigned long long)file->total,
                    file->complete     ? L"已完整保存"
                        : row->pending ? L"待核对 / 重新提交"
                                       : L"待下载");
                append(window, file_text, RGB(24, 87, 151), 0, 1);
                ui->attachment_end[i][j] = text_length(window);
            }
            ui->message_end[i] = text_length(window);
        }
    }
    if (!ui->history || !ui->history->count) append(window, ui->ready ? L"当前筛选下没有消息。" : L"请在配置窗口登录。", RGB(95, 100, 108), 0, 0);
    SendMessageW(window, EM_SETREADONLY, TRUE, 0);
    SendMessageW(window, EM_EMPTYUNDOBUFFER, 0, 0);
    SendMessageW(window, EM_EXSETSEL, 0, (LPARAM)&selection);
    SendMessageW(window, EM_SETSCROLLPOS, 0, (LPARAM)&scroll);
    if (follow) SendMessageW(window, EM_SCROLL, SB_BOTTOM, 0);
    SendMessageW(window, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(window, NULL, TRUE);
    ui->rebuilding = 0;
#if DROP_DIAGNOSTICS
    SetPropW(ui->window, L"DROP_TEST_MESSAGE_COUNT", (HANDLE)(UINT_PTR)(ui->history ? ui->history->count : 0));
    CHARFORMAT2W format = {0};
    format.cbSize = sizeof(format);
    SendMessageW(window, EM_GETCHARFORMAT, SCF_DEFAULT, (LPARAM)&format);
    SetPropW(ui->window, L"DROP_TEST_MESSAGE_TWIPS", (HANDLE)(UINT_PTR)format.yHeight);
    SetPropW(ui->window, L"DROP_TEST_MESSAGE_FONT", (HANDLE)(UINT_PTR)(!wcscmp(format.szFaceName, L"Microsoft YaHei UI")));
#endif
}
/* 按当前本地搜索重建参与者列表，恢复账号 ID 对应的选择，自己始终置顶且可见。 */
void drop_ui_users(drop_ui *ui) {
    HWND window = CONTROL(ui, UI_USERS);
    WCHAR search[65], text[192], unread[32];
    GetWindowTextW(CONTROL(ui, UI_USER_SEARCH), search, 65);
    ui->rebuilding = 1;
    SendMessageW(window, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(window);
    if (ui->history) {
        for (int i = -1; i < ui->history->peer_count; ++i) {
            int index = i == -1 ? 0 : i == 0 ? 1 : i + 1;
            drop_peer *peer = i == -1 ? &ui->history->peers[0] : i == 0 ? NULL : &ui->history->peers[i];
            LVITEMW item = {0};
            if (peer && i > 0 && *search && !wcsstr(peer->name, search) && !wcsstr(peer->login, search)) continue;
            if (i == -1)
                swprintf_s(text, 192, L"%s（自己）", peer->name);
            else
                wcscpy_s(text, 192, peer ? peer->name : L"全部消息");
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = ListView_GetItemCount(window);
            item.pszText = text;
            item.lParam = index;
            int inserted = ListView_InsertItem(window, &item);
            swprintf_s(unread, 32, L"%d", peer ? peer->unread : ui->history->unread);
            ListView_SetItemText(window, inserted, 1, unread);
            if ((!peer && !*ui->peer) || (peer && !strcmp(peer->id, ui->peer))) ListView_SetItemState(window, inserted, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
    }
    SendMessageW(window, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(window, NULL, TRUE);
    ui->rebuilding = 0;
}
/* 原生报告列表显示所选消息的附件；附件名在消息中同时显示，此处负责明确选中和导出。 */
void drop_ui_files(drop_ui *ui) {
    HWND window = CONTROL(ui, UI_FILES);
    ui->rebuilding = 1;
    SendMessageW(window, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(window);
    if (ui->detail) {
        for (int i = 0; i < ui->detail->count; ++i) {
            LVITEMW item = {0};
            WCHAR size[48];
            drop_file_view *file = &ui->detail->files[i];
            item.mask = LVIF_TEXT;
            item.iItem = i;
            item.pszText = file->name;
            ListView_InsertItem(window, &item);
            swprintf_s(size, 48, L"%llu 字节", (unsigned long long)file->total);
            ListView_SetItemText(window, i, 1, size);
            ListView_SetItemText(window, i, 2, file->complete ? L"已保存" : L"待完成");
        }
        if (ui->attachment_selection < 0 || ui->attachment_selection >= ui->detail->count) ui->attachment_selection = 0;
        if (ui->detail->count) {
            ListView_SetItemState(window, ui->attachment_selection, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(window, ui->attachment_selection, FALSE);
        }
    }
    SendMessageW(window, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(window, NULL, TRUE);
    ui->rebuilding = 0;
}
