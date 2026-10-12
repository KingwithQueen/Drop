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
/* 选中文字的位置归属于已绘制消息的标题、正文或附件，不借用可能已替换的历史快照。 */
typedef struct selection_anchor {
    char id[DROP_ID_CAP];
    LONG offset;
    int section, file;
} selection_anchor;
/* 将原控件字符位置转换为稳定消息身份和区段偏移，支持跨消息的文字选择。 */
static selection_anchor selection_save(drop_ui *ui, LONG position) {
    selection_anchor anchor = {0};
    for (int i = 0; i < DROP_HISTORY_PAGE; ++i) {
        if (!*ui->rendered_ids[i] || position < ui->message_start[i] || position > ui->message_end[i]) continue;
        strcpy_s(anchor.id, DROP_ID_CAP, ui->rendered_ids[i]);
        anchor.section = position < ui->message_body[i] ? 0 : 1;
        LONG start = anchor.section ? ui->message_body[i] : ui->message_start[i];
        for (int j = 0; j < DROP_MAX_ATTACHMENTS; ++j) {
            if (!ui->attachment_end[i][j] || position < ui->attachment_start[i][j] || position >= ui->attachment_end[i][j]) continue;
            anchor.section = 2;
            anchor.file = j;
            start = ui->attachment_start[i][j];
            break;
        }
        anchor.offset = position - start;
        break;
    }
    return anchor;
}
/* 按新绘制区段恢复选择端点，区段缩短时夹到其末尾，原消息离开当前页时沿用控件的安全夹取。 */
static LONG selection_restore(drop_ui *ui, const selection_anchor *anchor, LONG fallback) {
    if (!*anchor->id) return fallback;
    for (int i = 0; i < DROP_HISTORY_PAGE; ++i) {
        if (strcmp(ui->rendered_ids[i], anchor->id)) continue;
        LONG start = anchor->section ? ui->message_body[i] : ui->message_start[i];
        LONG end = anchor->section ? ui->message_end[i] : ui->message_body[i];
        if (anchor->section == 2 && ui->attachment_end[i][anchor->file]) {
            start = ui->attachment_start[i][anchor->file];
            end = ui->attachment_end[i][anchor->file];
        }
        return start + anchor->offset > end ? end : start + anchor->offset;
    }
    return fallback;
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
    if (message == WM_KEYDOWN && wparam == VK_DELETE) {
        SendMessageW(ui->window, WM_COMMAND, UI_DELETE_MESSAGE, 0);
        return 0;
    }
    if (message == WM_CONTEXTMENU) {
        POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (point.x == -1 && point.y == -1) {
            GetCursorPos(&point);
        } else {
            POINT local = point;
            ScreenToClient(window, &local);
            select_position(ui, (LONG)SendMessageW(window, EM_CHARFROMPOS, 0, (LPARAM)&local));
        }
        HMENU menu = CreatePopupMenu();
        if (menu) {
            AppendMenuW(menu, MF_STRING, 33000, L"复制所选文字");
            AppendMenuW(menu, MF_STRING, UI_VIEW_MESSAGE, L"查看完整正文");
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(menu, MF_STRING | (IsWindowEnabled(CONTROL(ui, UI_DELETE_MESSAGE)) ? MF_ENABLED : MF_GRAYED), UI_DELETE_MESSAGE, L"删除此消息（同步所有设备）");
            int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, ui->window, NULL);
            DestroyMenu(menu);
            if (command == 33000) SendMessageW(window, WM_COPY, 0, 0);
            if (command == UI_DELETE_MESSAGE) SendMessageW(ui->window, WM_COMMAND, UI_DELETE_MESSAGE, 0);
            if (command == UI_VIEW_MESSAGE) SendMessageW(ui->window, WM_COMMAND, UI_VIEW_MESSAGE, 0);
        }
        return 0;
    }
    LRESULT result = DefSubclassProc(window, message, wparam, lparam);
    (void)subclass;
    if (message == WM_LBUTTONUP || message == WM_LBUTTONDBLCLK) {
        POINT point = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        select_position(ui, (LONG)SendMessageW(window, EM_CHARFROMPOS, 0, (LPARAM)&point));
        if (message == WM_LBUTTONDBLCLK) SendMessageW(ui->window, WM_COMMAND, UI_VIEW_MESSAGE, 0);
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
    SendMessageW(window, EM_SETBKGNDCOLOR, 0, ui->colors[DROP_UI_SURFACE]);
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
    selection_anchor first = selection_save(ui, selection.cpMin), last = selection_save(ui, selection.cpMax);
    SendMessageW(window, WM_SETREDRAW, FALSE, 0);
    SendMessageW(window, EM_SETREADONLY, FALSE, 0);
    SetWindowTextW(window, L"");
    SendMessageW(window, WM_SETFONT, (WPARAM)ui->font, FALSE);
    memset(ui->rendered_ids, 0, sizeof(ui->rendered_ids));
    memset(ui->attachment_end, 0, sizeof(ui->attachment_end));
    if (ui->history) {
        for (int i = ui->history->count - 1; i >= 0; --i) {
            drop_history_row *row = &ui->history->rows[i];
            drop_detail *content = row->content;
            const WCHAR *saved = row->pending ? L"恢复后待核对" : wcsstr(row->state, L"附件待完成") ? L"附件待下载" : L"本机已保存";
            ui->message_start[i] = text_length(window);
            strcpy_s(ui->rendered_ids[i], DROP_ID_CAP, row->id);
            swprintf_s(header, 320, L"%s  %s  ·  %s%s\r\n", row->sender, row->time, saved,
                row->mine ? (wcsstr(row->state, L"账号已读") ? L" · 账号已读" : L" · 账号未读") : (row->unread ? L" · 未读" : L""));
            append(window, header, ui->colors[row->mine ? DROP_UI_ACCENT : DROP_UI_MUTED], 1, 0);
            ui->message_body[i] = text_length(window);
            wcsncpy_s(preview, 321, content->text, _TRUNCATE);
            size_t length = wcslen(preview);
            if (length && preview[length - 1] >= 0xd800 && preview[length - 1] <= 0xdbff) preview[length - 1] = 0;
            if (*preview) append(window, preview, ui->colors[DROP_UI_TEXT], 0, 0);
            if (wcslen(content->text) > 320) append(window, L"…（选中消息后在下方查看完整正文）", ui->colors[DROP_UI_MUTED], 0, 0);
            if (*preview) append(window, L"\r\n", ui->colors[DROP_UI_TEXT], 0, 0);
            for (int j = 0; j < content->count; ++j) {
                drop_file_view *file = &content->files[j];
                ui->attachment_start[i][j] = text_length(window);
                swprintf_s(file_text, 1200, L"    附件：%s  ·  %llu 字节  ·  %s\r\n", file->name, (unsigned long long)file->total,
                    file->complete     ? L"已缓存到本机"
                        : row->pending ? L"待核对 / 重新提交"
                                       : L"待下载");
                append(window, file_text, ui->colors[DROP_UI_ACCENT], 0, 1);
                ui->attachment_end[i][j] = text_length(window);
            }
            ui->message_end[i] = text_length(window);
        }
    }
    if (!ui->history || !ui->history->count) append(window, ui->ready ? L"当前会话筛选下没有消息。" : L"请在配置窗口登录。", ui->colors[DROP_UI_MUTED], 0, 0);
    SendMessageW(window, EM_SETREADONLY, TRUE, 0);
    SendMessageW(window, EM_EMPTYUNDOBUFFER, 0, 0);
    selection.cpMin = selection_restore(ui, &first, selection.cpMin);
    selection.cpMax = selection_restore(ui, &last, selection.cpMax);
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
/* 仅在列表内容实际变化时暂停重绘，避免每个同步快照都使原生列表擦除背景。 */
static void users_change(HWND window, int *changed) {
    if (!*changed) SendMessageW(window, WM_SETREDRAW, FALSE, 0);
    *changed = 1;
}
/* 对用户列表逐项比较和更新；保留稳定行、滚动及选择，未读变化只改计数列，自己始终置顶。 */
void drop_ui_users(drop_ui *ui) {
    HWND window = CONTROL(ui, UI_USERS);
    WCHAR search[65], text[192], unread[32], previous[192];
    int count = 0, changed = 0;
    GetWindowTextW(CONTROL(ui, UI_USER_SEARCH), search, 65);
    ui->rebuilding = 1;
    if (ui->history) {
        for (int i = 0; i < ui->history->peer_count; ++i) {
            int index = i;
            drop_peer *peer = &ui->history->peers[i];
            LVITEMW item = {0};
            if (i > 0 && *search && !wcsstr(peer->name, search) && !wcsstr(peer->login, search)) continue;
            if (i == 0)
                swprintf_s(text, 192, L"%s（自己）", peer->name);
            else
                wcscpy_s(text, 192, peer->name);
            item.mask = LVIF_PARAM;
            item.iItem = count;
            item.pszText = text;
            item.lParam = index;
            if (count >= ListView_GetItemCount(window)) {
                users_change(window, &changed);
                item.mask |= LVIF_TEXT;
                ListView_InsertItem(window, &item);
            } else {
                LVITEMW old = {0};
                old.mask = LVIF_PARAM;
                old.iItem = count;
                if (!ListView_GetItem(window, &old) || old.lParam != item.lParam) {
                    users_change(window, &changed);
                    ListView_SetItem(window, &item);
                }
                ListView_GetItemText(window, count, 0, previous, 192);
                if (wcscmp(text, previous)) {
                    users_change(window, &changed);
                    ListView_SetItemText(window, count, 0, text);
                }
            }
            swprintf_s(unread, 32, L"%d", peer->unread);
            ListView_GetItemText(window, count, 1, previous, 192);
            if (wcscmp(unread, previous)) {
                users_change(window, &changed);
                ListView_SetItemText(window, count, 1, unread);
            }
            UINT state = !strcmp(peer->id, ui->peer) ? LVIS_SELECTED | LVIS_FOCUSED : 0;
            if (ListView_GetItemState(window, count, LVIS_SELECTED | LVIS_FOCUSED) != state) {
                users_change(window, &changed);
                ListView_SetItemState(window, count, state, LVIS_SELECTED | LVIS_FOCUSED);
            }
            ++count;
        }
    }
    while (ListView_GetItemCount(window) > count) {
        users_change(window, &changed);
        ListView_DeleteItem(window, ListView_GetItemCount(window) - 1);
    }
    if (changed) {
        SendMessageW(window, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(window, NULL, FALSE);
    }
    ui->rebuilding = 0;
}
/* 原生报告列表显示所选消息的附件；附件名在消息中同时显示，此处负责明确选中和导出。 */
void drop_ui_files(drop_ui *ui) {
    HWND window = CONTROL(ui, UI_FILES);
    WCHAR heading[96];
    swprintf_s(heading, 96, L"所选消息的附件（%d）", ui->detail ? ui->detail->count : 0);
    SetWindowTextW(CONTROL(ui, UI_DETAIL_LABEL), heading);
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
            ListView_SetItemText(window, i, 2, file->complete ? L"本机缓存" : L"待下载");
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
