/* 待发送附件的原生列表、剪贴板及文件图标后台；附件内容在发送时由同步核心另存独立副本。 */
#include "ui_internal.h"
#include <objbase.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define CONTROL(ui, id) ((ui)->controls[(id) - UI_URL])

/* 图标请求只携带扩展名与代次，后台不能访问可能已经移除的界面路径。 */
struct ui_icon_job {
    unsigned generation;
    int count;
    WCHAR extensions[DROP_MAX_ATTACHMENTS][64];
};
/* 每个返回图标由结果拥有，应用或丢弃后均须 DestroyIcon；不借用系统图像列表。 */
typedef struct ui_icon_result {
    unsigned generation;
    int count;
    HICON icons[DROP_MAX_ATTACHMENTS];
} ui_icon_result;

/* 从 Windows 文件类型取得图标，不读取文件内容；耗时 Shell 查询独立于界面和网络同步线程。 */
static DWORD WINAPI icon_worker(void *data) {
    drop_ui *ui = data;
    HANDLE events[] = {ui->stop, ui->icon_event};
    HRESULT initialized = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) return 1;
    while (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1) {
        ui_icon_job *job;
        EnterCriticalSection(&ui->lock);
        job = ui->icon_job;
        ui->icon_job = NULL;
        LeaveCriticalSection(&ui->lock);
        if (!job) continue;
        ui_icon_result *result = calloc(1, sizeof(*result));
        if (result) {
            result->generation = job->generation;
            result->count = job->count;
            for (int i = 0; i < job->count && WaitForSingleObject(ui->stop, 0) == WAIT_TIMEOUT; ++i) {
                SHFILEINFOW info = {0};
                if (SHGetFileInfoW(
                        *job->extensions[i] ? job->extensions[i] : L"file", FILE_ATTRIBUTE_NORMAL, &info, sizeof(info), SHGFI_USEFILEATTRIBUTES | SHGFI_ICON | SHGFI_LARGEICON))
                    result->icons[i] = info.hIcon;
            }
            if (!PostMessageW(ui->window, DROP_UI_DRAFT_ICONS, 0, (LPARAM)result)) drop_ui_draft_icons(NULL, result);
        }
        free(job);
    }
    CoUninitialize();
    return 0;
}

/* 按目标 DPI 建立本实例的图像列表；Shell 结果未到达时先显示共享的通用文件图标。 */
static HIMAGELIST image_list(drop_ui *ui, const ui_icon_result *result) {
    int size = drop_ui_scale(ui->dpi, 22);
    HIMAGELIST list = ImageList_Create(size, size, ILC_COLOR32 | ILC_MASK, DROP_MAX_ATTACHMENTS, 1);
    HICON fallback = (HICON)LoadImageW(NULL, IDI_APPLICATION, IMAGE_ICON, size, size, LR_SHARED);
    if (list)
        for (int i = 0; i < ui->file_count; ++i)
            ImageList_AddIcon(list, result && result->icons[i] ? result->icons[i] : fallback);
    return list;
}

/* 只有代次和附件数量仍匹配时应用图标，选择和文件行不重建；所有返回图标随后释放。 */
void drop_ui_draft_icons(drop_ui *ui, void *data) {
    ui_icon_result *result = data;
    if (!result) return;
    if (ui && result->generation == ui->icon_generation && result->count == ui->file_count && IsWindow(CONTROL(ui, UI_DRAFT_FILES))) {
        HIMAGELIST list = image_list(ui, result);
        if (list) {
            ListView_SetImageList(CONTROL(ui, UI_DRAFT_FILES), list, LVSIL_SMALL);
            if (ui->draft_icons) ImageList_Destroy(ui->draft_icons);
            ui->draft_icons = list;
            ListView_RedrawItems(CONTROL(ui, UI_DRAFT_FILES), 0, ui->file_count - 1);
        }
    }
    for (int i = 0; i < result->count; ++i)
        if (result->icons[i]) DestroyIcon(result->icons[i]);
    free(result);
}

/* 界面保留独立绝对路径，按路径去重；目录及不存在文件不会成为看似已添加的附件。 */
int drop_ui_draft_add(drop_ui *ui, const WCHAR *path) {
    WCHAR full[32768];
    if (ui->sending || ui->file_count >= DROP_MAX_ATTACHMENTS || !path || !*path) return 0;
    DWORD length = GetFullPathNameW(path, 32768, full, NULL), attributes;
    if (!length || length >= 32768) return 0;
    attributes = GetFileAttributesW(full);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    for (int i = 0; i < ui->file_count; ++i)
        if (!_wcsicmp(ui->paths[i], full)) return 0;
    WCHAR *copy = _wcsdup(full);
    if (!copy) return 0;
    ui->paths[ui->file_count++] = copy;
    return 1;
}

/* 文件名和大小立即显示；图标请求覆盖尚未执行的旧请求，返回结果按代次核对。 */
void drop_ui_draft_refresh(drop_ui *ui) {
    HWND list = CONTROL(ui, UI_DRAFT_FILES);
    WCHAR label[96];
    if (!IsWindow(list)) return;
    ++ui->icon_generation;
    ui_icon_job *job = calloc(1, sizeof(*job));
    if (job) {
        job->generation = ui->icon_generation;
        job->count = ui->file_count;
    }
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    HIMAGELIST images = image_list(ui, NULL);
    ListView_SetImageList(list, images, LVSIL_SMALL);
    if (ui->draft_icons) ImageList_Destroy(ui->draft_icons);
    ui->draft_icons = images;
#if DROP_DIAGNOSTICS
    int icon_width = 0, icon_height = 0;
    if (images) ImageList_GetIconSize(images, &icon_width, &icon_height);
    SetPropW(ui->window, L"DROP_TEST_DRAFT_ICON_WIDTH", (HANDLE)(UINT_PTR)icon_width);
#endif
    for (int i = 0; i < ui->file_count; ++i) {
        const WCHAR *base = wcsrchr(ui->paths[i], L'\\');
        base = base ? base + 1 : ui->paths[i];
        const WCHAR *extension = wcsrchr(base, L'.');
        if (job && extension) wcsncpy_s(job->extensions[i], 64, extension, _TRUNCATE);
        LVITEMW item = {0};
        item.mask = LVIF_TEXT | LVIF_IMAGE;
        item.iItem = i;
        item.iImage = i;
        item.pszText = (LPWSTR)base;
        ListView_InsertItem(list, &item);
        WIN32_FILE_ATTRIBUTE_DATA info;
        WCHAR size[64] = L"文件不可用";
        if (GetFileAttributesExW(ui->paths[i], GetFileExInfoStandard, &info))
            swprintf_s(size, 64, L"%llu 字节", (unsigned long long)(((uint64_t)info.nFileSizeHigh << 32) | info.nFileSizeLow));
        ListView_SetItemText(list, i, 1, size);
    }
    if (ui->file_count) ListView_SetItemState(list, ui->file_count - 1, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, NULL, TRUE);
    swprintf_s(label, 96, L"待发送附件（%d）", ui->file_count);
    SetWindowTextW(CONTROL(ui, UI_DRAFT_LABEL), label);
    swprintf_s(label, 96, L"添加附件（%d）", ui->file_count);
    SetWindowTextW(CONTROL(ui, UI_ATTACH), label);
    EnableWindow(CONTROL(ui, UI_REMOVE_ATTACH), !ui->sending && ui->file_count > 0);
    if (job && ui->icon_worker) {
        EnterCriticalSection(&ui->lock);
        free(ui->icon_job);
        ui->icon_job = job;
        LeaveCriticalSection(&ui->lock);
        SetEvent(ui->icon_event);
    } else
        free(job);
    ShowWindow(list, ui->file_count ? SW_SHOW : SW_HIDE);
    ShowWindow(CONTROL(ui, UI_DRAFT_LABEL), ui->file_count ? SW_SHOW : SW_HIDE);
    SendMessageW(ui->window, WM_SIZE, 0, 0);
}

/* 仅移除界面中所选的待发送路径；发送已开始时禁止改变草稿，移除后重新编号预览和图标代次。 */
void drop_ui_draft_remove(drop_ui *ui) {
    int selected = ListView_GetNextItem(CONTROL(ui, UI_DRAFT_FILES), -1, LVNI_SELECTED);
    if (ui->sending || selected < 0 || selected >= ui->file_count) return;
    free(ui->paths[selected]);
    --ui->file_count;
    memmove(&ui->paths[selected], &ui->paths[selected + 1], (size_t)(ui->file_count - selected) * sizeof(*ui->paths));
    ui->paths[ui->file_count] = NULL;
    drop_ui_draft_refresh(ui);
}

/* 借用 Shell 文件列表逐项复制路径；剪贴板和拖放分别负责各自句柄的释放。 */
static void add_drop(drop_ui *ui, HDROP files) {
    UINT count = DragQueryFileW(files, 0xffffffff, NULL, 0);
    for (UINT i = 0; i < count && ui->file_count < DROP_MAX_ATTACHMENTS && !ui->sending; ++i) {
        WCHAR path[32768];
        UINT length = DragQueryFileW(files, i, NULL, 0);
        if (length && length < 32768 && DragQueryFileW(files, i, path, 32768) == length) (void)drop_ui_draft_add(ui, path);
    }
    drop_ui_draft_refresh(ui);
}

void drop_ui_draft_drop(drop_ui *ui, HDROP files) {
    add_drop(ui, files);
    DragFinish(files);
}

/* 仅消费文件剪贴板，普通文字仍由编辑控件粘贴；CF_HDROP 归剪贴板所有，不能 DragFinish。 */
static int paste_files(drop_ui *ui) {
    if (!IsClipboardFormatAvailable(CF_HDROP)) return 0;
    if (!ui->sending && OpenClipboard(ui->window)) {
        HDROP files = (HDROP)GetClipboardData(CF_HDROP);
        if (files) add_drop(ui, files);
        CloseClipboard();
    }
    return 1;
}

/* 编辑框接收文件粘贴和拖入，保留普通文字编辑；附件列表还支持 Delete 移除所选文件。 */
static LRESULT CALLBACK compose_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR subclass, DWORD_PTR data) {
    drop_ui *ui = (drop_ui *)data;
    (void)subclass;
    if (message == WM_PASTE && paste_files(ui)) return 0;
    if (message == WM_KEYDOWN && ((wparam == L'V' && GetKeyState(VK_CONTROL) < 0) || (wparam == VK_INSERT && GetKeyState(VK_SHIFT) < 0)) && paste_files(ui)) return 0;
    if (message == WM_CHAR && wparam == 22 && IsClipboardFormatAvailable(CF_HDROP)) return 0;
    if (message == WM_KEYDOWN && wparam == VK_DELETE && window == CONTROL(ui, UI_DRAFT_FILES)) {
        drop_ui_draft_remove(ui);
        return 0;
    }
    if (message == WM_DROPFILES) {
        drop_ui_draft_drop(ui, (HDROP)wparam);
        return 0;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

/* 绑定两个输入入口并启动独立图标线程；线程或事件创建失败时仍可使用通用图标添加和发送文件。 */
void drop_ui_compose_create(drop_ui *ui) {
    const int controls[] = {UI_BODY, UI_DRAFT_FILES};
    for (int i = 0; i < 2; ++i) {
        HWND window = CONTROL(ui, controls[i]);
        SetWindowSubclass(window, compose_proc, 1, (DWORD_PTR)ui);
        DragAcceptFiles(window, TRUE);
    }
    ui->icon_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (ui->icon_event) ui->icon_worker = CreateThread(NULL, 0, icon_worker, ui, 0, NULL);
}

/* 必须先通知全应用 stop；等待正在使用界面状态的线程退出，再回收队列、窗口消息和图像列表。 */
int drop_ui_compose_close(drop_ui *ui) {
    MSG message;
    if (ui->icon_worker && WaitForSingleObject(ui->icon_worker, 3000) != WAIT_OBJECT_0) return 0;
    if (ui->icon_worker) CloseHandle(ui->icon_worker);
    if (ui->icon_event) CloseHandle(ui->icon_event);
    ui->icon_worker = ui->icon_event = NULL;
    free(ui->icon_job);
    ui->icon_job = NULL;
    while (PeekMessageW(&message, ui->window, DROP_UI_DRAFT_ICONS, DROP_UI_DRAFT_ICONS, PM_REMOVE))
        drop_ui_draft_icons(NULL, (void *)message.lParam);
    if (ui->draft_icons) {
        if (IsWindow(CONTROL(ui, UI_DRAFT_FILES))) ListView_SetImageList(CONTROL(ui, UI_DRAFT_FILES), NULL, LVSIL_SMALL);
        ImageList_Destroy(ui->draft_icons);
        ui->draft_icons = NULL;
    }
    return 1;
}
