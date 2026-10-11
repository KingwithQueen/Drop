/* 主窗口和配置窗口的逐屏 DPI、字体及小屏滚动；仅在界面线程使用。 */
#include "ui_internal.h"
#include "resource.h"
#include <stdio.h>

#define CONTROL(ui, id) ((ui)->controls[(id) - UI_URL])

#if DROP_DIAGNOSTICS
/* 测试消息在窗口线程生成建议矩形，只验证本实例的真实 DPI 处理路径。 */
void drop_ui_test_dpi(HWND window, UINT dpi, int width, int height) {
    RECT rect;
    if (dpi < 96 || dpi > 384 || width < 320 || width > 4096 || height < 240 || height > 4096) return;
    GetWindowRect(window, &rect);
    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
    SendMessageW(window, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), (LPARAM)&rect);
}
#endif

int drop_ui_scale(UINT dpi, int value) {
    return MulDiv(value, (int)(dpi ? dpi : USER_DEFAULT_SCREEN_DPI), USER_DEFAULT_SCREEN_DPI);
}

int drop_ui_configuration_control(int id) {
    return (id >= UI_URL && id <= UI_SIGNIN) || id == UI_LOGOUT || id == UI_NOTIFICATIONS || id == UI_TRAY_FLASH || id == UI_AUTOSTART || id == UI_COMPRESSION || id == UI_LEVEL ||
        (id >= UI_SAVE_CONFIG && id <= UI_CONFIG_STATUS) || (id >= UI_CONNECTION_LABEL && id <= UI_CONFIG_HINT);
}

/* 逐个替换字体，包含列表的列标题；不让系统把旧尺寸的文字位图放大。 */
static BOOL CALLBACK font_child(HWND window, LPARAM font) {
    SendMessageW(window, WM_SETFONT, (WPARAM)font, TRUE);
    return TRUE;
}

/* 字体按目标像素重新栅格化，控件全部切换完毕后再销毁旧字体；图标从多尺寸资源选择。 */
void drop_ui_dpi(drop_ui *ui, HWND window, UINT dpi) {
    int configuration = window == ui->configuration;
    HFONT *target = configuration ? &ui->configuration_font : &ui->font, old = *target;
    HFONT font = CreateFontW(-drop_ui_scale(dpi, configuration ? 15 : 14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    HICON icon;
    if (!font) return;
    *target = font;
    if (configuration)
        ui->configuration_dpi = dpi;
    else
        ui->dpi = dpi;
    EnumChildWindows(window, font_child, (LPARAM)font);
    if (!configuration && CONTROL(ui, UI_USERS)) SendMessageW(ListView_GetHeader(CONTROL(ui, UI_USERS)), WM_SETFONT, (WPARAM)font, TRUE);
    if (!configuration && CONTROL(ui, UI_FILES)) SendMessageW(ListView_GetHeader(CONTROL(ui, UI_FILES)), WM_SETFONT, (WPARAM)font, TRUE);
    if (old) DeleteObject(old);
    icon =
        (HICON)LoadImageW(ui->instance, MAKEINTRESOURCEW(IDI_DROP_APP), IMAGE_ICON, GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi), LR_SHARED);
    SendMessageW(window, WM_SETICON, ICON_SMALL, (LPARAM)icon);
    icon = (HICON)LoadImageW(ui->instance, MAKEINTRESOURCEW(IDI_DROP_APP), IMAGE_ICON, GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CYICON, dpi), LR_SHARED);
    SendMessageW(window, WM_SETICON, ICON_BIG, (LPARAM)icon);
#if DROP_DIAGNOSTICS
    SetPropW(window, L"DROP_TEST_DPI", (HANDLE)(UINT_PTR)dpi);
#endif
}

/* 分别转换矩形两端，避免在 125% 或 175% 下累加每段尺寸的舍入误差。 */
void drop_ui_move(drop_ui *ui, int id, int x, int y, int width, int height) {
    int configuration = drop_ui_configuration_control(id), scroll = configuration ? ui->configuration_scroll : ui->scroll;
    UINT dpi = configuration ? ui->configuration_dpi : ui->dpi;
    int left = drop_ui_scale(dpi, x), top = drop_ui_scale(dpi, y) - scroll;
    int right = drop_ui_scale(dpi, x + width), bottom = drop_ui_scale(dpi, y + height) - scroll;
    MoveWindow(CONTROL(ui, id), left, top, right > left ? right - left : 1, bottom > top ? bottom - top : 1, TRUE);
}

/* 内容不足一屏时隐藏滚动条，超出时保留逻辑内容高度并限制滚动位置。 */
void drop_ui_viewport(drop_ui *ui, HWND window, int minimum_height, int *width, int *height) {
    int configuration = window == ui->configuration, content;
    int *scroll = configuration ? &ui->configuration_scroll : &ui->scroll;
    UINT dpi = configuration ? ui->configuration_dpi : ui->dpi;
    RECT client;
    SCROLLINFO info = {sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS, 0, 0, 0, 0, 0};
    GetClientRect(window, &client);
    content = drop_ui_scale(dpi, minimum_height);
    info.nMax = content - 1;
    info.nPage = (UINT)client.bottom;
    info.nPos = *scroll;
    SetScrollInfo(window, SB_VERT, &info, TRUE);
    *scroll = GetScrollPos(window, SB_VERT);
    /* 滚动条出现后客户区宽度会变化，必须重新读取，不能沿用旧宽度。 */
    GetClientRect(window, &client);
    *width = MulDiv(client.right, USER_DEFAULT_SCREEN_DPI, (int)dpi);
    *height = MulDiv(client.bottom, USER_DEFAULT_SCREEN_DPI, (int)dpi);
    if (*height < minimum_height) *height = minimum_height;
}

/* 滚轮、页滚动和拖动滑块共用 Windows 限制后的像素位置，再重新安排当前窗口控件。 */
void drop_ui_scroll(drop_ui *ui, HWND window, UINT message, WPARAM wparam) {
    int configuration = window == ui->configuration, step;
    int *position = configuration ? &ui->configuration_scroll : &ui->scroll;
    UINT dpi = configuration ? ui->configuration_dpi : ui->dpi;
    SCROLLINFO info = {sizeof(info), SIF_ALL, 0, 0, 0, 0, 0};
    RECT client;
    GetScrollInfo(window, SB_VERT, &info);
    step = drop_ui_scale(dpi, 36);
    if (message == WM_MOUSEWHEEL)
        info.nPos -= MulDiv(GET_WHEEL_DELTA_WPARAM(wparam), step, WHEEL_DELTA);
    else {
        switch (LOWORD(wparam)) {
        case SB_LINEUP:
            info.nPos -= step;
            break;
        case SB_LINEDOWN:
            info.nPos += step;
            break;
        case SB_PAGEUP:
            info.nPos -= (int)info.nPage;
            break;
        case SB_PAGEDOWN:
            info.nPos += (int)info.nPage;
            break;
        case SB_THUMBTRACK:
            info.nPos = info.nTrackPos;
            break;
        case SB_TOP:
            info.nPos = 0;
            break;
        case SB_BOTTOM:
            info.nPos = info.nMax;
            break;
        default:
            return;
        }
    }
    info.fMask = SIF_POS;
    SetScrollInfo(window, SB_VERT, &info, TRUE);
    *position = GetScrollPos(window, SB_VERT);
    GetClientRect(window, &client);
    SendMessageW(window, WM_SIZE, SIZE_RESTORED, MAKELPARAM(client.right, client.bottom));
    InvalidateRect(window, NULL, TRUE);
}

/* 工作区使用当前窗口所属的屏幕，窗口边框按相同 DPI 计算；任务栏区域不用于放置窗口。 */
static RECT work_area(HWND window) {
    MONITORINFO monitor = {sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
    return monitor.rcWork;
}

/* 初次显示在当前屏幕工作区居中，必要时缩短客户区并由滚动布局保留底部操作。 */
void drop_ui_place(drop_ui *ui, HWND window, int width, int height) {
    UINT dpi = window == ui->configuration ? ui->configuration_dpi : ui->dpi;
    RECT area = work_area(window), rect = {0, 0, drop_ui_scale(dpi, width), drop_ui_scale(dpi, height)};
    AdjustWindowRectExForDpi(&rect, (DWORD)GetWindowLongPtrW(window, GWL_STYLE), GetMenu(window) != NULL, (DWORD)GetWindowLongPtrW(window, GWL_EXSTYLE), dpi);
    width = rect.right - rect.left;
    height = rect.bottom - rect.top;
    if (width > area.right - area.left) width = area.right - area.left;
    if (height > area.bottom - area.top) height = area.bottom - area.top;
    SetWindowPos(window, NULL, area.left + (area.right - area.left - width) / 2, area.top + (area.bottom - area.top - height) / 2, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

/* 最小尺寸考虑当前 DPI 和窗口边框，并受工作区限制，不强迫高缩放屏幕容纳过大的窗口。 */
void drop_ui_minimum(drop_ui *ui, HWND window, MINMAXINFO *info, int width, int height) {
    UINT dpi = window == ui->configuration ? ui->configuration_dpi : ui->dpi;
    RECT area = work_area(window), rect = {0, 0, drop_ui_scale(dpi, width), drop_ui_scale(dpi, height)};
    AdjustWindowRectExForDpi(
        &rect, (DWORD)GetWindowLongPtrW(window, GWL_STYLE), GetMenu(window) != NULL, (DWORD)GetWindowLongPtrW(window, GWL_EXSTYLE), dpi ? dpi : USER_DEFAULT_SCREEN_DPI);
    info->ptMinTrackSize.x = rect.right - rect.left;
    info->ptMinTrackSize.y = rect.bottom - rect.top;
    if (info->ptMinTrackSize.x > area.right - area.left) info->ptMinTrackSize.x = area.right - area.left;
    if (info->ptMinTrackSize.y > area.bottom - area.top) info->ptMinTrackSize.y = area.bottom - area.top;
}

/* 配置的纵向内容固定为 550 逻辑像素，短屏通过滚动访问保存和关闭按钮。 */
static void configuration_layout(drop_ui *ui) {
    int width, height, left = 20, right, half;
    drop_ui_viewport(ui, ui->configuration, 550, &width, &height);
    right = width - 20;
    half = (right - left - 16) / 2;
    drop_ui_move(ui, UI_CONNECTION_LABEL, left, 16, right - left, 24);
    drop_ui_move(ui, UI_URL_LABEL, left, 46, right - left, 20);
    drop_ui_move(ui, UI_URL, left, 68, right - left, 28);
    drop_ui_move(ui, UI_CA_LABEL, left, 104, right - left, 20);
    drop_ui_move(ui, UI_CA, left, 126, right - left, 28);
    drop_ui_move(ui, UI_LOGIN_LABEL, left, 162, half, 20);
    drop_ui_move(ui, UI_PASSWORD_LABEL, left + half + 16, 162, half, 20);
    drop_ui_move(ui, UI_LOGIN, left, 184, half, 28);
    drop_ui_move(ui, UI_PASSWORD, left + half + 16, 184, half, 28);
    drop_ui_move(ui, UI_PROBE, left, 224, 110, 30);
    drop_ui_move(ui, UI_LOGOUT, right - 230, 224, 110, 30);
    drop_ui_move(ui, UI_SIGNIN, right - 110, 224, 110, 30);
    drop_ui_move(ui, UI_CONFIG_HINT, left, 266, right - left, 40);
    drop_ui_move(ui, UI_PREFERENCES_LABEL, left, 314, right - left, 24);
    drop_ui_move(ui, UI_NOTIFICATIONS, left, 346, half, 24);
    drop_ui_move(ui, UI_TRAY_FLASH, left + half + 16, 346, half, 24);
    drop_ui_move(ui, UI_AUTOSTART, left, 380, right - left, 24);
    drop_ui_move(ui, UI_COMPRESSION, left, 418, half, 200);
    drop_ui_move(ui, UI_LEVEL, left + half + 16, 418, half, 360);
    drop_ui_move(ui, UI_SAVE_CONFIG, right - 230, height - 92, 110, 30);
    drop_ui_move(ui, UI_CONFIG_CLOSE, right - 110, height - 92, 110, 30);
    drop_ui_move(ui, UI_CONFIG_STATUS, left, height - 52, right - left, 44);
}

/* 配置命令回到主窗口统一提交后台任务；每个顶层窗口独立处理 DPI 改变。 */
static LRESULT CALLBACK configuration_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    drop_ui *ui = (drop_ui *)GetWindowLongPtrW(window, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        ui = ((CREATESTRUCTW *)lparam)->lpCreateParams;
        ui->configuration = window;
        ui->configuration_dpi = GetDpiForWindow(window);
        SetWindowLongPtrW(window, GWLP_USERDATA, (LONG_PTR)ui);
    }
    if (!ui) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
#if DROP_DIAGNOSTICS
    case DROP_UI_TEST_DPI:
        drop_ui_test_dpi(window, (UINT)wparam, LOWORD(lparam), HIWORD(lparam));
        return 0;
#endif
    case WM_CREATE:
        drop_ui_dpi(ui, window, ui->configuration_dpi);
        return 0;
    case WM_SIZE:
        configuration_layout(ui);
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkColor((HDC)wparam, GetSysColor(COLOR_WINDOW));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_DPICHANGED: {
        RECT *rect = (RECT *)lparam;
        drop_ui_dpi(ui, window, HIWORD(wparam));
        SetWindowPos(window, NULL, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        configuration_layout(ui);
        InvalidateRect(window, NULL, TRUE);
        return 0;
    }
    case WM_GETMINMAXINFO:
        drop_ui_minimum(ui, window, (MINMAXINFO *)lparam, 520, 320);
        return 0;
    case WM_MOUSEWHEEL:
    case WM_VSCROLL:
        drop_ui_scroll(ui, window, message, wparam);
        return 0;
    case WM_COMMAND:
        if (LOWORD(wparam) == UI_CONFIG_CLOSE || LOWORD(wparam) == IDCANCEL) {
            drop_ui_configuration_show(ui, 0);
            return 0;
        }
        if (LOWORD(wparam) == IDOK) wparam = MAKEWPARAM(UI_SIGNIN, BN_CLICKED);
        return SendMessageW(ui->window, message, wparam, lparam);
    case WM_CLOSE:
        drop_ui_configuration_show(ui, 0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

/* 配置窗口属于本实例主窗口，关闭时隐藏；主窗口销毁时由 Windows 一并销毁。 */
int drop_ui_configuration_create(drop_ui *ui) {
    WNDCLASSEXW cls = {0};
    WCHAR name[96];
    swprintf_s(name, 96, L"%s-Settings", ui->class_name);
    cls.cbSize = sizeof(cls);
    cls.hInstance = ui->instance;
    cls.lpfnWndProc = configuration_proc;
    cls.lpszClassName = name;
    cls.hCursor = LoadCursorW(NULL, IDC_ARROW);
    cls.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    if (!RegisterClassExW(&cls)) return 0;
    ui->configuration = CreateWindowExW(WS_EX_CONTROLPARENT, name, L"Drop " DROP_VERSION_WIDE L" · 配置", WS_OVERLAPPEDWINDOW | WS_VSCROLL, CW_USEDEFAULT, CW_USEDEFAULT,
        drop_ui_scale(ui->dpi, 560), drop_ui_scale(ui->dpi, 600), ui->window, NULL, ui->instance, ui);
    return ui->configuration != NULL;
}

/* 重开配置沿用主屏位置；清空关闭窗口中的未提交密码，保留公开编辑项供继续填写。 */
void drop_ui_configuration_show(drop_ui *ui, int show) {
    if (!show) {
        SetWindowTextW(CONTROL(ui, UI_PASSWORD), L"");
        ShowWindow(ui->configuration, SW_HIDE);
        return;
    }
    /* 首次显示及重新打开时沿用主窗口所在屏幕，之后允许用户独立拖动配置窗口。 */
    if (!IsWindowVisible(ui->configuration)) {
        RECT main;
        GetWindowRect(ui->window, &main);
        SetWindowPos(ui->configuration, NULL, main.left, main.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        drop_ui_dpi(ui, ui->configuration, GetDpiForWindow(ui->configuration));
        drop_ui_place(ui, ui->configuration, 560, 550);
    }
    configuration_layout(ui);
    ShowWindow(ui->configuration, SW_RESTORE);
    SetForegroundWindow(ui->configuration);
    SetFocus(CONTROL(ui, *ui->url ? UI_LOGIN : UI_URL));
}
