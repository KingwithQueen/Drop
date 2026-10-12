/* 本实例的原生控件配色及发送按钮绘制；不修改系统主题，画刷在窗口关闭后统一释放。 */
#include "ui_internal.h"
#include <richedit.h>
#define CONTROL(ui, id) ((ui)->controls[(id) - UI_URL])

/* 根据系统高对比度状态建立配色并更新列表和正文，旧画刷在控件切换后释放。 */
void drop_ui_theme(drop_ui *ui) {
    static const COLORREF normal[DROP_UI_COLOR_COUNT] = {RGB(236, 241, 247), RGB(255, 255, 255), RGB(229, 236, 245), RGB(219, 232, 247), RGB(37, 48, 62), RGB(84, 102, 122),
        RGB(34, 91, 151), RGB(181, 199, 219), RGB(255, 255, 255)};
    static const int system[DROP_UI_COLOR_COUNT] = {
        COLOR_BTNFACE, COLOR_WINDOW, COLOR_WINDOW, COLOR_BTNFACE, COLOR_WINDOWTEXT, COLOR_WINDOWTEXT, COLOR_HIGHLIGHT, COLOR_WINDOWFRAME, COLOR_HIGHLIGHTTEXT};
    HIGHCONTRASTW contrast = {sizeof(contrast), 0, NULL};
    int high = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON);
    for (int i = 0; i < DROP_UI_COLOR_COUNT; ++i) {
        HBRUSH old = ui->brushes[i];
        ui->colors[i] = high ? GetSysColor(system[i]) : normal[i];
        ui->brushes[i] = CreateSolidBrush(ui->colors[i]);
        if (old) DeleteObject(old);
    }
    const int lists[] = {UI_USERS, UI_FILES, UI_DRAFT_FILES};
    for (int i = 0; i < 3; ++i) {
        HWND list = CONTROL(ui, lists[i]);
        if (!list) continue;
        COLORREF background = ui->colors[lists[i] == UI_USERS ? DROP_UI_PANEL : DROP_UI_SURFACE];
        ListView_SetBkColor(list, background);
        ListView_SetTextBkColor(list, background);
        ListView_SetTextColor(list, ui->colors[DROP_UI_TEXT]);
        InvalidateRect(list, NULL, TRUE);
    }
    if (CONTROL(ui, UI_HISTORY)) SendMessageW(CONTROL(ui, UI_HISTORY), EM_SETBKGNDCOLOR, 0, ui->colors[DROP_UI_SURFACE]);
    if (ui->window) InvalidateRect(ui->window, NULL, TRUE);
    if (ui->configuration) InvalidateRect(ui->configuration, NULL, TRUE);
}

void drop_ui_theme_close(drop_ui *ui) {
    for (int i = 0; i < DROP_UI_COLOR_COUNT; ++i) {
        if (ui->brushes[i]) DeleteObject(ui->brushes[i]);
        ui->brushes[i] = NULL;
    }
}

/* 只读正文沿用白色内容区，发送对象及会话标题使用浅蓝底，其余标签融入窗口背景。 */
LRESULT drop_ui_control_color(drop_ui *ui, HDC dc, HWND control) {
    int id = GetDlgCtrlID(control), background = DROP_UI_BACKGROUND, text = DROP_UI_TEXT;
    if (id == UI_BODY || id == UI_DETAIL || id == UI_URL || id == UI_CA || id == UI_LOGIN || id == UI_PASSWORD || id == UI_SEARCH || id == UI_FROM || id == UI_TO ||
        id == UI_USER_SEARCH)
        background = DROP_UI_SURFACE;
    else if (id == UI_RECIPIENT || id == UI_OVERVIEW) {
        background = DROP_UI_TINT;
        text = DROP_UI_ACCENT;
    } else if (id == UI_DRAFT_LABEL)
        text = DROP_UI_ACCENT;
    else if (id == UI_SEND_HINT || id == UI_HISTORY_LABEL || id == UI_CONFIG_HINT || id == UI_DETAIL_LABEL)
        text = DROP_UI_MUTED;
    SetTextColor(dc, ui->colors[text]);
    SetBkColor(dc, ui->colors[background]);
    return (LRESULT)(ui->brushes[background] ? ui->brushes[background] : GetSysColorBrush(COLOR_WINDOW));
}

/* 只绘制发送按钮，按下偏移和焦点框仍遵循原生按钮状态；没有焦点时不绘制多余边框。 */
int drop_ui_draw_button(drop_ui *ui, const DRAWITEMSTRUCT *draw) {
    if (draw->CtlType != ODT_BUTTON || draw->CtlID != UI_SEND) return 0;
    RECT rect = draw->rcItem;
    WCHAR text[64];
    int disabled = (draw->itemState & ODS_DISABLED) != 0;
    FillRect(draw->hDC, &rect, ui->brushes[disabled ? DROP_UI_PANEL : DROP_UI_ACCENT]);
    SetBkMode(draw->hDC, TRANSPARENT);
    SetTextColor(draw->hDC, ui->colors[disabled ? DROP_UI_MUTED : DROP_UI_ON_ACCENT]);
    HGDIOBJ old = SelectObject(draw->hDC, ui->font);
    GetWindowTextW(draw->hwndItem, text, 64);
    if (draw->itemState & ODS_SELECTED) OffsetRect(&rect, 1, 1);
    DrawTextW(draw->hDC, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(draw->hDC, old);
    if ((draw->itemState & ODS_FOCUS) && !(draw->itemState & ODS_NOFOCUSRECT)) {
        InflateRect(&rect, -drop_ui_scale(ui->dpi, 3), -drop_ui_scale(ui->dpi, 3));
        DrawFocusRect(draw->hDC, &rect);
    }
    return 1;
}
