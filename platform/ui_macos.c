/* AppKit 原生界面以 C17 调用公开运行时；数据库、凭据、附件及消息删除仍由独立 C 后台拥有。 */
#include "runtime.h"
#include "macos_bridge.h"
#include <sodium.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <limits.h>

typedef struct mac_ui {
    id controller, window, settings, document, statusItem, users, files, draft, messages, detail, compose;
    id status, recipient, overview, draftLabel, fileLabel, search, find, from, to, serverURL, login, ca, password;
    id send, deleteButton, exportButton, notify, attention, paths, userIDs, ranges, fileRanges, timer, peer, selected;
    id split, left, right, recipientBand, overviewBand, messageScroll, userScroll, detailScroll, fileScroll, composeScroll, draftScroll;
    drop_runtime *runtime;
    drop_snapshot *snapshot;
    mac_integer selectedRow, selectedFile, page;
    BOOL rebuilding, hidden, sendPending, messagesValid, closing;
    uint64_t pendingFinished, pendingSerial, viewSerial;
} mac_ui;

/* 所有控件和此指针只允许主线程访问；后台仅通过拥有独立内存的任务及快照交换数据。 */
static mac_ui *active_ui;
static void layout(mac_ui *ui);
static BOOL submit(mac_ui *ui, drop_task *task);
static BOOL view_request(mac_ui *ui);
static void update_users(mac_ui *ui);
static void add_urls(mac_ui *ui, id urls);
static void select_message(mac_ui *ui, mac_integer row, mac_integer file);
static void apply_snapshot(mac_ui *ui, drop_snapshot *snapshot);

/* 不把 POSIX wchar_t 当作 UTF-16；临时 NSString 由当前 AppKit 自动释放池持有。 */
static id native_text(const WCHAR *wide) {
    size_t capacity = (wcslen(wide) + 1) * 4;
    char *utf8 = malloc(capacity);
    id text = mac_text("");
    if (utf8 && drop_to_utf8(wide, utf8, capacity)) {
        id converted = mac_text(utf8);
        if (converted) text = converted;
    }
    free(utf8);
    return text;
}
/* 使用 NSString 的原生格式器，保留 %@ 的对象语义；va_list 只在调用期间有效。 */
static id text_format(const char *format, ...) {
    va_list args;
    va_start(args, format);
    id text = MAC_SEND2(id, mac_alloc("NSString"), "initWithFormat:arguments:", id, mac_text(format), va_list, args);
    va_end(args);
    return mac_autorelease(text);
}
static drop_task *new_task(drop_task_kind kind) {
    drop_task *task = calloc(1, sizeof(*task));
    if (task) task->kind = kind;
    return task;
}
/* 仅比较实际绘制及详情依赖的字段；后台统计变化不能重写正文、丢失选区或重置滚动位置。 */
static BOOL same_messages(const drop_history *first, const drop_history *second) {
    if (!first || !second || strcmp(first->self, second->self) || strcmp(first->peer, second->peer) || first->count != second->count ||
        first->deletion_supported != second->deletion_supported)
        return NO;
    for (int i = 0; i < first->count; ++i) {
        const drop_history_row *a = &first->rows[i], *b = &second->rows[i];
        if (strcmp(a->id, b->id) || wcscmp(a->sender, b->sender) || wcscmp(a->time, b->time) || wcscmp(a->preview, b->preview) || a->unread != b->unread ||
            a->deletable != b->deletable || a->pending != b->pending || !a->content || !b->content || a->content->count != b->content->count ||
            wcscmp(a->content->text, b->content->text))
            return NO;
        for (int j = 0; j < a->content->count; ++j) {
            const drop_file_view *x = &a->content->files[j], *y = &b->content->files[j];
            if (strcmp(x->id, y->id) || strcmp(x->sha, y->sha) || wcscmp(x->name, y->name) || x->total != y->total || x->complete != y->complete) return NO;
        }
    }
    return YES;
}
static id app(void) {
    return mac_get(MAC_CLASS("NSApplication"), "sharedApplication");
}
static id color(CGFloat r, CGFloat g, CGFloat b) {
    return MAC_SEND4(id, MAC_CLASS("NSColor"), "colorWithCalibratedRed:green:blue:alpha:", CGFloat, r, CGFloat, g, CGFloat, b, CGFloat, 1);
}
static id font(CGFloat size) {
    return MAC_SEND1(id, MAC_CLASS("NSFont"), "systemFontOfSize:", CGFloat, size);
}
static id label(id text) {
    id view = MAC_SEND1(id, MAC_CLASS("NSTextField"), "labelWithString:", id, text);
    mac_put(view, "setFont:", font(12));
    mac_number(view, "setLineBreakMode:", MAC_LINE_TRUNCATE_TAIL);
    return view;
}
static id entry(const char *placeholder) {
    id view = mac_view("NSTextField", CGRectZero);
    mac_put(view, "setFont:", font(12));
    mac_put(view, "setPlaceholderString:", mac_text(placeholder));
    return view;
}
static id button(mac_ui *ui, const char *title, const char *action, id parent) {
    id view = MAC_SEND3(id, MAC_CLASS("NSButton"), "buttonWithTitle:target:action:", id, mac_text(title), id, ui->controller, SEL, MAC_SEL(action));
    mac_number(view, "setBezelStyle:", MAC_BEZEL_ROUNDED);
    mac_put(view, "setFont:", font(12));
    mac_put(parent, "addSubview:", view);
    return view;
}
static id scroll_view(id view) {
    id scroll = mac_view("NSScrollView", CGRectZero);
    mac_flag(scroll, "setHasVerticalScroller:", YES);
    mac_flag(scroll, "setHasHorizontalScroller:", NO);
    mac_number(scroll, "setBorderType:", MAC_BORDER_BEZEL);
    mac_put(scroll, "setDocumentView:", view);
    return scroll;
}
/* 文本宽度跟随视图，高度由 AppKit 排版；用户选区与滚动位置不受普通状态刷新影响。 */
static void prepare_text(id view, BOOL editable) {
    mac_flag(view, "setEditable:", editable);
    mac_flag(view, "setSelectable:", YES);
    mac_flag(view, "setRichText:", NO);
    mac_put(view, "setFont:", font(13));
    MAC_SEND1(void, view, "setTextContainerInset:", CGSize, CGSizeMake(3, 2));
    mac_flag(view, "setVerticallyResizable:", YES);
    mac_flag(view, "setHorizontallyResizable:", NO);
    mac_number(view, "setAutoresizingMask:", MAC_VIEW_WIDTH);
    id container = mac_get(view, "textContainer");
    mac_flag(container, "setWidthTracksTextView:", YES);
    MAC_SEND1(void, container, "setContainerSize:", CGSize, CGSizeMake(400, DBL_MAX));
}
static id window(CGFloat width, CGFloat height, mac_unsigned style) {
    id view = mac_autorelease(MAC_SEND4(id, mac_alloc("NSWindow"), "initWithContentRect:styleMask:backing:defer:", CGRect, CGRectMake(0, 0, width, height), mac_unsigned, style,
        mac_unsigned, MAC_BACKING_BUFFERED, BOOL, NO));
    mac_flag(view, "setReleasedWhenClosed:", NO);
    return view;
}
static id menu(const char *title) {
    return mac_autorelease(MAC_SEND1(id, mac_alloc("NSMenu"), "initWithTitle:", id, mac_text(title)));
}
static id menu_item(mac_ui *ui, id parent, const char *title, const char *action, const char *key) {
    id item = mac_autorelease(
        MAC_SEND3(id, mac_alloc("NSMenuItem"), "initWithTitle:action:keyEquivalent:", id, mac_text(title), SEL, action ? MAC_SEL(action) : NULL, id, mac_text(key)));
    if (action) mac_put(item, "setTarget:", ui->controller);
    mac_put(parent, "addItem:", item);
    return item;
}
static void table_column(id table, const char *identifier, const char *title, CGFloat width) {
    id column = mac_autorelease(MAC_SEND1(id, mac_alloc("NSTableColumn"), "initWithIdentifier:", id, mac_text(identifier)));
    mac_put(column, "setTitle:", mac_text(title));
    mac_float(column, "setWidth:", width);
    mac_put(table, "addTableColumn:", column);
}
static id table(mac_ui *ui, CGFloat height) {
    id view = mac_view("NSTableView", CGRectZero);
    mac_float(view, "setRowHeight:", height);
    mac_put(view, "setDataSource:", ui->controller);
    mac_put(view, "setDelegate:", ui->controller);
    return view;
}
/* 只在主线程提交并转移任务所有权；新视图入队后，旧会话不再允许删除或导出。 */
static BOOL submit(mac_ui *ui, drop_task *task) {
    BOOL view = task && (task->kind == DROP_TASK_VIEW || task->kind == DROP_TASK_FIND || task->kind == DROP_TASK_LOGIN || task->kind == DROP_TASK_LOGOUT);
    if (view) task->view_serial = ui->viewSerial + 1;
    if (task && drop_runtime_submit(ui->runtime, task)) {
        if (view) {
            ++ui->viewSerial;
            mac_flag(ui->send, "setEnabled:", NO);
            BOOL rebuilding = ui->rebuilding;
            ui->rebuilding = YES;
            ui->messagesValid = NO;
            mac_owned(&ui->selected, mac_text(""));
            ui->selectedRow = ui->selectedFile = -1;
            mac_put(ui->messages, "setString:", mac_text(""));
            mac_put(ui->detail, "setString:", mac_text(""));
            mac_void(ui->ranges, "removeAllObjects");
            mac_void(ui->fileRanges, "removeAllObjects");
            mac_void(ui->files, "reloadData");
            mac_flag(ui->deleteButton, "setEnabled:", NO);
            mac_flag(ui->exportButton, "setEnabled:", NO);
            mac_put(ui->fileLabel, "setStringValue:", mac_text("所选消息的附件（0）"));
            ui->rebuilding = rebuilding;
        }
        return YES;
    }
    drop_task_free(task);
    mac_put(ui->status, "setStringValue:", mac_text("任务队列忙，请稍后重试。"));
    return NO;
}
/* 控件由窗口/视图树拥有；状态单独持有窗口、数组、定时器和选择字符串，清理由事件循环结束统一执行。 */
static void build_window(mac_ui *ui) {
    mac_owned(&ui->paths, mac_new("NSMutableArray"));
    mac_owned(&ui->userIDs, mac_new("NSMutableArray"));
    mac_owned(&ui->ranges, mac_new("NSMutableArray"));
    mac_owned(&ui->fileRanges, mac_new("NSMutableArray"));
    mac_owned(&ui->peer, mac_text(""));
    mac_owned(&ui->selected, mac_text(""));
    ui->selectedRow = ui->selectedFile = -1;
    mac_owned(&ui->window, window(1080, 760, MAC_WINDOW_TITLED | MAC_WINDOW_CLOSABLE | MAC_WINDOW_MINIATURIZABLE | MAC_WINDOW_RESIZABLE));
    mac_put(ui->window, "setTitle:", mac_text("Drop " DROP_VERSION));
    MAC_SEND1(void, ui->window, "setMinSize:", CGSize, CGSizeMake(820, 600));
    mac_put(ui->window, "setDelegate:", ui->controller);
    id root = mac_view("DropView", CGRectMake(0, 0, 1080, 760));
    mac_put(ui->window, "setContentView:", root);
    mac_frame(button(ui, "同步", "synchronize:", root), 4, 4, 66, 28);
    mac_frame(button(ui, "配置与账号", "showSettings:", root), 74, 4, 104, 28);
    ui->exportButton = button(ui, "导出附件", "exportAttachment:", root);
    mac_frame(ui->exportButton, 182, 4, 94, 28);
    ui->deleteButton = button(ui, "删除消息", "deleteMessage:", root);
    mac_frame(ui->deleteButton, 280, 4, 94, 28);
    id quit = button(ui, "真正退出", "quit:", root);
    mac_frame(quit, 978, 4, 98, 28);
    mac_number(quit, "setAutoresizingMask:", MAC_VIEW_MIN_X);
    ui->split = mac_view("NSSplitView", CGRectMake(4, 36, 1072, 696));
    mac_flag(ui->split, "setVertical:", YES);
    mac_number(ui->split, "setDividerStyle:", MAC_SPLIT_THIN);
    mac_put(ui->split, "setDelegate:", ui->controller);
    mac_number(ui->split, "setAutoresizingMask:", MAC_VIEW_WIDTH | MAC_VIEW_HEIGHT);
    ui->left = mac_view("DropView", CGRectMake(0, 0, 216, 696));
    ui->right = mac_view("DropView", CGRectMake(217, 0, 855, 696));
    mac_put(ui->split, "addSubview:", ui->left);
    mac_put(ui->split, "addSubview:", ui->right);
    mac_put(root, "addSubview:", ui->split);
    MAC_SEND2(void, ui->split, "setPosition:ofDividerAtIndex:", CGFloat, 216, mac_integer, 0);
    ui->find = entry("账号 / 查找用户");
    mac_put(ui->left, "addSubview:", ui->find);
    mac_put(button(ui, "查找", "findAccount:", ui->left), "setIdentifier:", mac_text("find-button"));
    ui->users = table(ui, 22);
    table_column(ui->users, "name", "用户 / 账号", 165);
    table_column(ui->users, "unread", "未读", 42);
    ui->userScroll = scroll_view(ui->users);
    mac_put(ui->left, "addSubview:", ui->userScroll);
    ui->search = entry("当前会话正文");
    ui->from = entry("起日 YYYY-MM-DD");
    ui->to = entry("止日 YYYY-MM-DD");
    mac_put(ui->right, "addSubview:", ui->search);
    mac_put(ui->right, "addSubview:", ui->from);
    mac_put(ui->right, "addSubview:", ui->to);
    mac_put(button(ui, "查询", "filter:", ui->right), "setIdentifier:", mac_text("filter-button"));
    mac_put(button(ui, "‹", "previous:", ui->right), "setIdentifier:", mac_text("previous-button"));
    mac_put(button(ui, "›", "next:", ui->right), "setIdentifier:", mac_text("next-button"));
    ui->messages = mac_view("DropMessages", CGRectMake(0, 0, 400, 60));
    prepare_text(ui->messages, NO);
    mac_flag(ui->messages, "setRichText:", YES);
    mac_put(ui->messages, "setDelegate:", ui->controller);
    ui->messageScroll = scroll_view(ui->messages);
    mac_put(ui->right, "addSubview:", ui->messageScroll);
    ui->detail = mac_view("NSTextView", CGRectMake(0, 0, 400, 60));
    prepare_text(ui->detail, NO);
    ui->detailScroll = scroll_view(ui->detail);
    mac_owned(&ui->document, window(760, 500, MAC_WINDOW_TITLED | MAC_WINDOW_CLOSABLE | MAC_WINDOW_RESIZABLE));
    mac_put(ui->document, "setTitle:", mac_text("Drop " DROP_VERSION " · 完整正文（可选择复制）"));
    id document_root = mac_get(ui->document, "contentView");
    MAC_SEND1(void, ui->detailScroll, "setFrame:", CGRect, mac_rect(document_root, "bounds"));
    mac_number(ui->detailScroll, "setAutoresizingMask:", MAC_VIEW_WIDTH | MAC_VIEW_HEIGHT);
    mac_put(document_root, "addSubview:", ui->detailScroll);
    ui->files = table(ui, 22);
    table_column(ui->files, "file", "文件名", 220);
    table_column(ui->files, "size", "大小", 74);
    table_column(ui->files, "state", "状态", 74);
    ui->fileScroll = scroll_view(ui->files);
    mac_put(ui->right, "addSubview:", ui->fileScroll);
    ui->recipient = label(mac_text("发送给：请先登录"));
    mac_put(ui->recipient, "setTextColor:", color(.13, .36, .59));
    ui->recipientBand = mac_view("DropBand", CGRectZero);
    mac_put(ui->recipientBand, "addSubview:", ui->recipient);
    mac_put(ui->right, "addSubview:", ui->recipientBand);
    ui->overview = label(mac_text("当前会话 · 请先登录"));
    mac_put(ui->overview, "setTextColor:", mac_get(ui->recipient, "textColor"));
    ui->overviewBand = mac_view("DropBand", CGRectZero);
    mac_put(ui->overviewBand, "addSubview:", ui->overview);
    mac_put(ui->right, "addSubview:", ui->overviewBand);
    ui->draftLabel = label(mac_text("待发送附件（0）"));
    ui->fileLabel = label(mac_text("所选消息的附件"));
    mac_put(ui->right, "addSubview:", ui->draftLabel);
    mac_put(ui->right, "addSubview:", ui->fileLabel);
    ui->draft = table(ui, 24);
    table_column(ui->draft, "file", "待发送文件", 240);
    table_column(ui->draft, "size", "大小", 74);
    ui->draftScroll = scroll_view(ui->draft);
    mac_put(ui->right, "addSubview:", ui->draftScroll);
    ui->compose = mac_view("DropComposer", CGRectMake(0, 0, 400, 56));
    prepare_text(ui->compose, YES);
    id paste_types[] = {NSPasteboardTypeFileURL};
    mac_put(ui->compose, "registerForDraggedTypes:", mac_array(1, paste_types));
    ui->composeScroll = scroll_view(ui->compose);
    mac_put(ui->right, "addSubview:", ui->composeScroll);
    mac_put(button(ui, "添加附件", "addAttachments:", ui->right), "setIdentifier:", mac_text("add-button"));
    ui->send = button(ui, "发送", "sendMessage:", ui->right);
    mac_put(button(ui, "清空附件", "clearAttachments:", ui->right), "setIdentifier:", mac_text("clear-button"));
    ui->status = label(mac_text("正在打开本机数据…"));
    mac_frame(ui->status, 6, 737, 1068, 20);
    mac_number(ui->status, "setAutoresizingMask:", MAC_VIEW_WIDTH | MAC_VIEW_MIN_Y);
    mac_put(root, "addSubview:", ui->status);
    mac_flag(ui->send, "setEnabled:", NO);
    mac_flag(ui->exportButton, "setEnabled:", NO);
    mac_flag(ui->deleteButton, "setEnabled:", NO);
    layout(ui);
    mac_void(ui->window, "center");
}

/* 固定附件区和输入区高度；仅调整逻辑点坐标，AppKit 负责显示器缩放及控件实际绘制。 */
static void layout(mac_ui *ui) {
    CGRect left = mac_rect(ui->left, "bounds"), right = mac_rect(ui->right, "bounds");
    CGFloat lh = left.size.height, lw = left.size.width, h = right.size.height, w = right.size.width;
    mac_frame(ui->find, 0, 2, fmax(80, lw - 68), 24);
    id children = mac_get(ui->left, "subviews");
    for (mac_unsigned i = 0; i < mac_count(children); ++i) {
        id view = mac_at(children, i);
        if (mac_equal(mac_get(view, "identifier"), mac_text("find-button"))) mac_frame(view, lw - 64, 0, 64, 28);
    }
    mac_frame(ui->userScroll, 0, 31, lw, fmax(100, lh - 31));
    CGFloat searchWidth = fmax(140, w - 365);
    mac_frame(ui->overviewBand, 3, 0, w - 3, 26);
    mac_frame(ui->overview, 5, 4, w - 13, 18);
    mac_frame(ui->search, 3, 34, searchWidth, 24);
    mac_frame(ui->from, searchWidth + 8, 34, 116, 24);
    mac_frame(ui->to, searchWidth + 128, 34, 116, 24);
    children = mac_get(ui->right, "subviews");
    for (mac_unsigned i = 0; i < mac_count(children); ++i) {
        id view = mac_at(children, i), name = mac_get(view, "identifier");
        if (mac_equal(name, mac_text("filter-button"))) mac_frame(view, w - 113, 32, 57, 28);
        if (mac_equal(name, mac_text("previous-button"))) mac_frame(view, w - 57, 32, 28, 28);
        if (mac_equal(name, mac_text("next-button"))) mac_frame(view, w - 29, 32, 28, 28);
        if (mac_equal(name, mac_text("add-button"))) mac_frame(view, w - 94, h - 64, 94, 28);
        if (mac_equal(name, mac_text("clear-button"))) mac_frame(view, w - 94, h - 100, 94, 28);
    }
    mac_frame(ui->messageScroll, 3, 65, w - 3, fmax(100, h - 327));
    mac_frame(ui->draftLabel, 4, h - 256, (w - 6) * .5, 18);
    mac_frame(ui->fileLabel, (w - 6) * .5 + 6, h - 256, (w - 6) * .5, 18);
    mac_frame(ui->draftScroll, 3, h - 234, (w - 6) * .5, 84);
    mac_frame(ui->fileScroll, (w - 6) * .5 + 6, h - 234, (w - 6) * .5, 84);
    mac_frame(ui->recipientBand, 3, h - 142, w - 3, 26);
    mac_frame(ui->recipient, 5, 4, w - 13, 18);
    mac_frame(ui->composeScroll, 3, h - 108, w - 101, 104);
    mac_frame(ui->send, w - 94, h - 32, 94, 28);
}

/* 日期解析必须完整回写一致，避免宽松解析把无效日期转换成另一日；结束日期包含本地当天。 */
static BOOL view_request(mac_ui *ui) {
    drop_task *task = new_task(DROP_TASK_VIEW);
    if (!task) return NO;
    strcpy_s(task->peer, DROP_ID_CAP, mac_utf8(ui->peer));
    task->page = (int)ui->page;
    task->text = strdup(mac_utf8(mac_get(ui->search, "stringValue")));
    id fields[] = {mac_get(ui->from, "stringValue"), mac_get(ui->to, "stringValue")};
    int64_t times[] = {0, INT64_MAX};
    id formatter = mac_new("NSDateFormatter");
    id locale = mac_autorelease(MAC_SEND1(id, mac_alloc("NSLocale"), "initWithLocaleIdentifier:", id, mac_text("en_US_POSIX")));
    mac_put(formatter, "setLocale:", locale);
    mac_put(formatter, "setDateFormat:", mac_text("yyyy-MM-dd"));
    mac_flag(formatter, "setLenient:", NO);
    for (int i = 0; i < 2; ++i) {
        if (!mac_int(fields[i], "length")) continue;
        id date = MAC_SEND1(id, formatter, "dateFromString:", id, fields[i]);
        if (!date || !mac_equal(MAC_SEND1(id, formatter, "stringFromDate:", id, date), fields[i])) {
            mac_put(ui->status, "setStringValue:", mac_text("日期格式：YYYY-MM-DD。"));
            drop_task_free(task);
            return NO;
        }
        if (i) {
            id calendar = mac_get(MAC_CLASS("NSCalendar"), "currentCalendar");
            id next = MAC_SEND4(id, calendar, "dateByAddingUnit:value:toDate:options:", mac_unsigned, MAC_CALENDAR_DAY, mac_integer, 1, id, date, mac_unsigned, 0);
            times[i] = (int64_t)MAC_SEND0(double, next, "timeIntervalSince1970") - 1;
        } else {
            times[i] = (int64_t)MAC_SEND0(double, date, "timeIntervalSince1970");
        }
    }
    task->from = times[0];
    task->to = times[1];
    return submit(ui, task);
}
static id user_text(mac_ui *ui, mac_integer row, id column) {
    if (row < 0 || row >= (mac_integer)mac_count(ui->userIDs) || !ui->snapshot || !ui->snapshot->history) return mac_text("");
    drop_history *history = ui->snapshot->history;
    for (int i = 0; i < history->peer_count; ++i) {
        drop_peer *peer = &history->peers[i];
        if (strcmp(peer->id, mac_utf8(mac_at(ui->userIDs, (mac_unsigned)row)))) continue;
        if (mac_equal(mac_get(column, "identifier"), mac_text("unread"))) return text_format("%d", peer->unread);
        return MAC_SEND1(id, native_text(peer->name), "stringByAppendingString:", id, mac_text(strcmp(peer->id, history->self) ? "" : "（自己）"));
    }
    return mac_text("");
}
/* 行身份使用账号 ID，普通同步更新现有单元格文字，避免重建表格造成单次选择失效或可见用户名消失。 */
static void update_users(mac_ui *ui) {
    id desired = mac_new("NSMutableArray");
    drop_history *history = ui->snapshot ? ui->snapshot->history : NULL;
    if (history) {
        for (int i = 0; i < history->peer_count; ++i)
            mac_add(desired, mac_text(history->peers[i].id));
    }
    if (!MAC_SEND1(BOOL, ui->userIDs, "isEqualToArray:", id, desired)) {
        mac_void(ui->users, "beginUpdates");
        for (mac_integer i = (mac_integer)mac_count(ui->userIDs) - 1; i >= 0; --i) {
            if (MAC_SEND1(BOOL, desired, "containsObject:", id, mac_at(ui->userIDs, (mac_unsigned)i))) continue;
            MAC_SEND1(void, ui->userIDs, "removeObjectAtIndex:", mac_unsigned, (mac_unsigned)i);
            MAC_SEND2(void, ui->users, "removeRowsAtIndexes:withAnimation:", id, mac_index((mac_unsigned)i), mac_unsigned, 0);
        }
        for (mac_unsigned i = 0; i < mac_count(desired); ++i) {
            id value = mac_at(desired, i);
            mac_unsigned old = MAC_SEND1(mac_unsigned, ui->userIDs, "indexOfObject:", id, value);
            if (old == (mac_unsigned)LONG_MAX) {
                MAC_SEND2(void, ui->userIDs, "insertObject:atIndex:", id, value, mac_unsigned, i);
                MAC_SEND2(void, ui->users, "insertRowsAtIndexes:withAnimation:", id, mac_index(i), mac_unsigned, 0);
            } else if (old != i) {
                MAC_SEND1(void, ui->userIDs, "removeObjectAtIndex:", mac_unsigned, old);
                MAC_SEND2(void, ui->userIDs, "insertObject:atIndex:", id, value, mac_unsigned, i);
                MAC_SEND2(void, ui->users, "moveRowAtIndex:toIndex:", mac_integer, (mac_integer)old, mac_integer, (mac_integer)i);
            }
        }
        mac_void(ui->users, "endUpdates");
    }
    id columns = mac_get(ui->users, "tableColumns");
    for (mac_integer row = 0; row < (mac_integer)mac_count(ui->userIDs); ++row) {
        for (mac_integer column = 0; column < mac_int(ui->users, "numberOfColumns"); ++column) {
            id cell = MAC_SEND3(id, ui->users, "viewAtColumn:row:makeIfNecessary:", mac_integer, column, mac_integer, row, BOOL, NO);
            id text = user_text(ui, row, mac_at(columns, (mac_unsigned)column)), field = mac_get(cell, "textField");
            if (cell && !mac_equal(mac_get(field, "stringValue"), text)) mac_put(field, "setStringValue:", text);
        }
    }
    mac_unsigned selected = MAC_SEND1(mac_unsigned, ui->userIDs, "indexOfObject:", id, ui->peer);
    if (selected == (mac_unsigned)LONG_MAX) {
        if (mac_int(ui->users, "selectedRow") >= 0) mac_put(ui->users, "deselectAll:", nil);
    } else if (mac_int(ui->users, "selectedRow") != (mac_integer)selected) {
        MAC_SEND2(void, ui->users, "selectRowIndexes:byExtendingSelection:", id, mac_index(selected), BOOL, NO);
    }
}
/* 消息及附件均使用稳定身份；读回执由后台持久处理，界面刷新期间不重复提交。 */
static void select_message(mac_ui *ui, mac_integer row, mac_integer file) {
    if (!ui->snapshot || ui->snapshot->view_serial != ui->viewSerial || !ui->snapshot->history || row < 0 || row >= ui->snapshot->history->count) return;
    drop_history_row *message = &ui->snapshot->history->rows[row];
    ui->selectedRow = row;
    ui->selectedFile = file;
    mac_owned(&ui->selected, mac_text(message->id));
    mac_put(ui->detail, "setString:", message->content ? native_text(message->content->text) : mac_text(""));
    mac_put(ui->fileLabel, "setStringValue:", text_format("所选消息的附件（%d）", message->content ? message->content->count : 0));
    mac_void(ui->files, "reloadData");
    if (file >= 0 && message->content && file < message->content->count) {
        MAC_SEND2(void, ui->files, "selectRowIndexes:byExtendingSelection:", id, mac_index((mac_unsigned)file), BOOL, NO);
    }
    mac_flag(ui->deleteButton, "setEnabled:", message->deletable && ui->snapshot->history->deletion_supported);
    mac_flag(ui->exportButton, "setEnabled:", message->content && message->content->count);
    if (message->unread && !message->pending && !ui->rebuilding) {
        drop_task *task = new_task(DROP_TASK_READ);
        if (task) strcpy_s(task->message, DROP_ID_CAP, message->id);
        submit(ui, task);
    }
}
static mac_integer table_rows(id self, SEL command, id table_view) {
    (void)self;
    (void)command;
    mac_ui *ui = active_ui;
    if (table_view == ui->draft) return (mac_integer)mac_count(ui->paths);
    if (table_view == ui->users) return (mac_integer)mac_count(ui->userIDs);
    if (!ui->snapshot || !ui->snapshot->history || ui->selectedRow < 0 || ui->selectedRow >= ui->snapshot->history->count) return 0;
    drop_detail *detail = ui->snapshot->history->rows[ui->selectedRow].content;
    return detail ? detail->count : 0;
}
/* 原生表格取值入口；标签先加入视图树，再设置单元格弱出口，保证文字控件在自动释放池清空后仍有效。 */
static id table_cell(id self, SEL command, id table_view, id column, mac_integer row) {
    (void)self;
    (void)command;
    mac_ui *ui = active_ui;
    id text = mac_text(""), identifier = mac_get(column, "identifier");
    CGFloat width = MAC_SEND0(CGFloat, column, "width");
    if (table_view == ui->draft) {
        id path = mac_at(ui->paths, (mac_unsigned)row);
        if (mac_equal(identifier, mac_text("file"))) {
            id cell = mac_view("NSTableCellView", CGRectMake(0, 0, width, 24));
            id icon = mac_view("NSImageView", CGRectMake(3, 4, 16, 16));
            mac_put(icon, "setImage:", MAC_SEND1(id, mac_get(MAC_CLASS("NSWorkspace"), "sharedWorkspace"), "iconForFile:", id, path));
            id name = label(mac_get(path, "lastPathComponent"));
            mac_frame(name, 24, 3, fmax(1, width - 26), 18);
            mac_number(name, "setAutoresizingMask:", MAC_VIEW_WIDTH);
            mac_put(name, "setToolTip:", mac_get(path, "lastPathComponent"));
            mac_put(cell, "addSubview:", icon);
            mac_put(cell, "addSubview:", name);
            mac_put(cell, "setTextField:", name);
            mac_put(cell, "setImageView:", icon);
            return cell;
        }
        struct stat st;
        uint64_t size = !lstat(MAC_SEND0(const char *, path, "fileSystemRepresentation"), &st) && st.st_size >= 0 ? (uint64_t)st.st_size : 0;
        text = text_format("%llu", (unsigned long long)size);
    } else if (table_view == ui->users) {
        id cell = MAC_SEND2(id, table_view, "makeViewWithIdentifier:owner:", id, identifier, id, ui->controller);
        if (!cell) {
            cell = mac_view("NSTableCellView", CGRectMake(0, 0, width, MAC_SEND0(CGFloat, table_view, "rowHeight")));
            mac_put(cell, "setIdentifier:", identifier);
            id field = label(mac_text(""));
            mac_frame(field, 3, 2, fmax(1, width - 6), 18);
            mac_number(field, "setAutoresizingMask:", MAC_VIEW_WIDTH);
            mac_put(cell, "addSubview:", field);
            mac_put(cell, "setTextField:", field);
        }
        mac_put(mac_get(cell, "textField"), "setStringValue:", user_text(ui, row, column));
        return cell;
    } else {
        if (row < 0 || row >= table_rows(ui->controller, NULL, table_view)) return label(text);
        drop_file_view *file = &ui->snapshot->history->rows[ui->selectedRow].content->files[row];
        if (mac_equal(identifier, mac_text("file")))
            text = native_text(file->name);
        else if (mac_equal(identifier, mac_text("size")))
            text = text_format("%llu", (unsigned long long)file->total);
        else
            text = mac_text(file->complete ? "已缓存" : "待下载");
    }
    return label(text);
}
static void table_selection(id self, SEL command, id notification) {
    (void)self;
    (void)command;
    mac_ui *ui = active_ui;
    id object = mac_get(notification, "object");
    if (object == ui->draft || ui->rebuilding || !ui->snapshot || !ui->snapshot->history) return;
    if (object == ui->users) {
        mac_integer row = mac_int(ui->users, "selectedRow");
        if (row < 0 || row >= (mac_integer)mac_count(ui->userIDs) || mac_equal(ui->peer, mac_at(ui->userIDs, (mac_unsigned)row))) return;
        /* 提交失败时需要恢复旧字符串，临时持有旧值，避免替换拥有槽位后形成悬空引用。 */
        id previous = mac_retain(ui->peer);
        mac_integer page = ui->page;
        mac_owned(&ui->peer, mac_at(ui->userIDs, (mac_unsigned)row));
        ui->page = 0;
        if (!view_request(ui)) {
            mac_owned(&ui->peer, previous);
            ui->page = page;
            ui->rebuilding = YES;
            update_users(ui);
            ui->rebuilding = NO;
        }
        mac_release(previous);
    } else {
        ui->selectedFile = mac_int(ui->files, "selectedRow");
    }
}
static void text_selection(id self, SEL command, id notification) {
    (void)self;
    (void)command;
    mac_ui *ui = active_ui;
    if (ui->rebuilding || mac_get(notification, "object") != ui->messages) return;
    mac_unsigned offset = MAC_SEND0(mac_range, ui->messages, "selectedRange").location;
    for (mac_unsigned i = 0; i < mac_count(ui->ranges); ++i) {
        mac_range range = MAC_SEND0(mac_range, mac_at(ui->ranges, i), "rangeValue");
        if (offset < range.location || offset - range.location >= range.length) continue;
        mac_integer file = -1;
        id ranges = mac_at(ui->fileRanges, i);
        for (mac_unsigned j = 0; j < mac_count(ranges); ++j) {
            range = MAC_SEND0(mac_range, mac_at(ranges, j), "rangeValue");
            if (offset >= range.location && offset - range.location < range.length) file = (mac_integer)j;
        }
        select_message(ui, (mac_integer)i, file);
        break;
    }
}

static id attributed(id text, id attributes) {
    return mac_autorelease(MAC_SEND2(id, mac_alloc("NSAttributedString"), "initWithString:attributes:", id, text, id, attributes));
}
/* 消费快照所有权；发送完成及通知独立处理，迟到视图不能把账号、页码和正文退回。 */
static void apply_snapshot(mac_ui *ui, drop_snapshot *snapshot) {
    drop_history *history = snapshot->history;
    if (snapshot->attention && history && history->new_notifications && mac_bool(ui->window, "isVisible")) {
        MAC_SEND1(mac_integer, app(), "requestUserAttention:", mac_unsigned, MAC_ATTENTION_INFORMATIONAL);
    }
    if (snapshot->notify && history && history->new_notifications && !mac_bool(ui->window, "isVisible") &&
        mac_get(mac_get(MAC_CLASS("NSBundle"), "mainBundle"), "bundleIdentifier")) {
        id content = mac_new("UNMutableNotificationContent");
        mac_put(content, "setTitle:", mac_text("Drop 收到新消息"));
        mac_put(content, "setBody:", mac_text("消息和附件正在同步到本机。"));
        id request = MAC_SEND3(id, MAC_CLASS("UNNotificationRequest"), "requestWithIdentifier:content:trigger:", id, mac_text("drop-new-message"), id, content, id, nil);
        MAC_SEND2(void, mac_get(MAC_CLASS("UNUserNotificationCenter"), "currentNotificationCenter"), "addNotificationRequest:withCompletionHandler:", id, request, id, nil);
    }
    if (ui->sendPending && snapshot->send_finished > ui->pendingFinished) {
        ui->sendPending = NO;
        mac_flag(ui->compose, "setEditable:", YES);
        if (snapshot->send_serial > ui->pendingSerial) {
            mac_put(ui->compose, "setString:", mac_text(""));
            mac_void(ui->paths, "removeAllObjects");
            add_urls(ui, mac_array(0, NULL));
        }
    }
    if (snapshot->view_serial < ui->viewSerial) {
        drop_snapshot_free(snapshot);
        return;
    }
    drop_snapshot *previous = ui->snapshot;
    BOOL changed = !ui->messagesValid || !same_messages(previous ? previous->history : NULL, history);
    BOOL changedView =
        !ui->messagesValid || !previous || !previous->history || !history || strcmp(previous->history->peer, history->peer) || strcmp(previous->history->self, history->self);
    ui->snapshot = snapshot;
    ui->rebuilding = YES;
    if (history) mac_owned(&ui->peer, mac_text(history->peer));
    update_users(ui);
    id recipient = mac_text("发送给：请选择账号"), overview = mac_text("消息");
    if (history) {
        for (int i = 0; i < history->peer_count; ++i) {
            if (strcmp(history->peers[i].id, history->peer)) continue;
            recipient = text_format("发送给：%@%@", native_text(history->peers[i].name), mac_text(!strcmp(history->peer, history->self) ? " · 自己的设备" : ""));
            overview = text_format("%@ · 第 %ld 页", native_text(history->peers[i].name), (long)ui->page + 1);
        }
    }
    if (!mac_equal(mac_get(ui->recipient, "stringValue"), recipient)) mac_put(ui->recipient, "setStringValue:", recipient);
    if (!mac_equal(mac_get(ui->overview, "stringValue"), overview)) mac_put(ui->overview, "setStringValue:", overview);
    if (changed) {
        id text = mac_autorelease(MAC_SEND1(id, mac_alloc("NSMutableAttributedString"), "initWithString:", id, mac_text("")));
        mac_void(ui->ranges, "removeAllObjects");
        mac_void(ui->fileRanges, "removeAllObjects");
        ui->selectedRow = -1;
        if (history) {
            id paragraph = mac_new("NSMutableParagraphStyle");
            mac_float(paragraph, "setParagraphSpacing:", 0);
            mac_float(paragraph, "setLineSpacing:", 0);
            id keys[] = {NSFontAttributeName, NSForegroundColorAttributeName, NSParagraphStyleAttributeName};
            id values[] = {font(13), mac_get(MAC_CLASS("NSColor"), "textColor"), paragraph};
            id normal = mac_dictionary(3, keys, values);
            values[0] = MAC_SEND1(id, MAC_CLASS("NSFont"), "boldSystemFontOfSize:", CGFloat, 13);
            values[1] = color(.19, .35, .53);
            id header = mac_dictionary(3, keys, values);
            /* 数据库按最新页倒序查询；反向绘制，并按原始行下标存储命中范围，保持删除及附件身份一致。 */
            for (int i = history->count - 1; i >= 0; --i) {
                drop_history_row *row = &history->rows[i];
                mac_unsigned start = (mac_unsigned)mac_int(text, "length");
                id heading = text_format("%@  %@ · %@\n", native_text(row->sender), native_text(row->time), mac_text(row->unread ? "未读" : "本机已保存"));
                mac_put(text, "appendAttributedString:", attributed(heading, header));
                id body = native_text(row->preview);
                if (mac_int(body, "length")) mac_put(text, "appendAttributedString:", attributed(MAC_SEND1(id, body, "stringByAppendingString:", id, mac_text("\n")), normal));
                id file_ranges = mac_new("NSMutableArray");
                if (row->content) {
                    for (int j = 0; j < row->content->count; ++j) {
                        drop_file_view *file = &row->content->files[j];
                        mac_unsigned file_start = (mac_unsigned)mac_int(text, "length");
                        id line = text_format(
                            "    附件：%@ · %llu 字节 · %@\n", native_text(file->name), (unsigned long long)file->total, mac_text(file->complete ? "已缓存到本机" : "待下载"));
                        mac_put(text, "appendAttributedString:", attributed(line, header));
                        mac_add(file_ranges,
                            MAC_SEND1(id, MAC_CLASS("NSValue"), "valueWithRange:", mac_range, mac_make_range(file_start, (mac_unsigned)mac_int(text, "length") - file_start)));
                    }
                }
                MAC_SEND2(void, ui->fileRanges, "insertObject:atIndex:", id, file_ranges, mac_unsigned, 0);
                id range = MAC_SEND1(id, MAC_CLASS("NSValue"), "valueWithRange:", mac_range, mac_make_range(start, (mac_unsigned)mac_int(text, "length") - start));
                MAC_SEND2(void, ui->ranges, "insertObject:atIndex:", id, range, mac_unsigned, 0);
                if (mac_equal(ui->selected, mac_text(row->id))) ui->selectedRow = i;
            }
        }
        mac_range selection = MAC_SEND0(mac_range, ui->messages, "selectedRange");
        id clip = mac_get(ui->messageScroll, "contentView");
        CGRect clip_bounds = mac_rect(clip, "bounds");
        CGPoint position = clip_bounds.origin;
        BOOL bottom = CGRectGetMaxY(clip_bounds) >= CGRectGetMaxY(mac_rect(ui->messages, "bounds")) - 2;
        mac_put(mac_get(ui->messages, "textStorage"), "setAttributedString:", text);
        mac_unsigned length = (mac_unsigned)mac_int(text, "length");
        if (!changedView && selection.location <= length && selection.length <= length - selection.location)
            MAC_SEND1(void, ui->messages, "setSelectedRange:", mac_range, selection);
        mac_put(mac_get(ui->messages, "layoutManager"), "ensureLayoutForTextContainer:", mac_get(ui->messages, "textContainer"));
        if (changedView || (bottom && !selection.length))
            MAC_SEND1(void, ui->messages, "scrollRangeToVisible:", mac_range, mac_make_range(length, 0));
        else
            MAC_SEND1(void, clip, "scrollToPoint:", CGPoint, position);
        mac_put(ui->messageScroll, "reflectScrolledClipView:", clip);
        mac_flag(ui->deleteButton, "setEnabled:", NO);
        mac_flag(ui->exportButton, "setEnabled:", NO);
        if (ui->selectedRow >= 0)
            select_message(ui, ui->selectedRow, ui->selectedFile);
        else {
            mac_put(ui->detail, "setString:", mac_text(""));
            mac_void(ui->files, "reloadData");
        }
        ui->messagesValid = YES;
    }
    mac_flag(ui->send, "setEnabled:", snapshot->ready && mac_int(ui->peer, "length") == 32 && !ui->sendPending);
    mac_put(ui->status, "setStringValue:",
        text_format("%@ · 待发送 %d · 下载 %d · 待删除 %d", mac_text(snapshot->status), history ? history->pending_count : 0, history ? history->downloads : 0,
            history ? history->pending_deletes : 0));
    mac_put(mac_get(ui->statusItem, "button"), "setTitle:", snapshot->attention && history && history->unread ? text_format("Drop %d", history->unread) : mac_text("Drop"));
    ui->rebuilding = NO;
    drop_snapshot_free(previous);
}

/* 普通文件才可进入草稿；不跟随符号链接，不在选择阶段写入用户目录。 */
static void add_urls(mac_ui *ui, id urls) {
    if (ui->sendPending) return;
    for (mac_unsigned i = 0; i < mac_count(urls); ++i) {
        id url = mac_at(urls, i), path = mac_get(url, "path");
        struct stat st;
        if (!mac_bool(url, "isFileURL") || mac_count(ui->paths) >= DROP_MAX_ATTACHMENTS || lstat(MAC_SEND0(const char *, path, "fileSystemRepresentation"), &st) ||
            !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size > UINT64_C(10737418240)) {
            mac_put(ui->status, "setStringValue:", mac_text("请选择普通文件，最多 32 个，每个不超过 10 GiB。"));
            continue;
        }
        if (!MAC_SEND1(BOOL, ui->paths, "containsObject:", id, path)) mac_add(ui->paths, path);
    }
    mac_void(ui->draft, "reloadData");
    mac_put(ui->draftLabel, "setStringValue:", text_format("待发送附件（%lu）", mac_count(ui->paths)));
    id remove_menu = menu("待发送附件");
    for (mac_unsigned i = 0; i < mac_count(ui->paths); ++i) {
        id path = mac_at(ui->paths, i);
        id item = menu_item(ui, remove_menu, "", "removeAttachment:", "");
        mac_put(item, "setTitle:", MAC_SEND1(id, mac_text("移除 "), "stringByAppendingString:", id, mac_get(path, "lastPathComponent")));
        mac_put(item, "setRepresentedObject:", path);
    }
    mac_put(ui->draft, "setMenu:", remove_menu);
}

typedef enum mac_sheet_kind { MAC_SHEET_ADD, MAC_SHEET_DELETE, MAC_SHEET_REVIEW, MAC_SHEET_EXPORT, MAC_SHEET_LOGOUT } mac_sheet_kind;
typedef struct mac_sheet mac_sheet;
typedef struct mac_block_descriptor {
    unsigned long reserved, size;
    void (*copy)(mac_sheet *destination, const mac_sheet *source);
    void (*dispose)(mac_sheet *block);
    const char *signature;
} mac_block_descriptor;
struct mac_sheet {
    void *isa;
    int flags, reserved;
    void (*invoke)(mac_sheet *block, mac_integer result);
    const mac_block_descriptor *descriptor;
    mac_ui *ui;
    id panel;
    mac_sheet_kind kind;
    char message[DROP_ID_CAP], object[DROP_ID_CAP];
};
extern void *_NSConcreteStackBlock[32];
extern void *_NSConcreteGlobalBlock[32];

/* AppKit 异步面板使用公开 Blocks ABI 的 C 结构，不启用 Objective-C 或 Blocks 语法。
 * 系统复制回调时 retain 面板，释放回调时对称 release；稳定消息/附件 ID 按值复制，绝不保存快照或文本的借用指针。 */
static void sheet_copy(mac_sheet *destination, const mac_sheet *source) {
    destination->panel = mac_retain(source->panel);
}
static void sheet_dispose(mac_sheet *block) {
    mac_release(block->panel);
}
static void sheet_complete(mac_sheet *block, mac_integer result) {
    mac_ui *ui = block->ui;
    if (ui->closing) return;
    if (block->kind == MAC_SHEET_ADD) {
        if (result == MAC_MODAL_OK) add_urls(ui, mac_get(block->panel, "URLs"));
        return;
    }
    BOOL confirmed = result == (block->kind == MAC_SHEET_EXPORT ? MAC_MODAL_OK : MAC_ALERT_SECOND_BUTTON);
    if (!confirmed) return;
    drop_task_kind kind = DROP_TASK_DELETE;
    if (block->kind == MAC_SHEET_REVIEW) kind = DROP_TASK_REVIEW;
    if (block->kind == MAC_SHEET_EXPORT) kind = DROP_TASK_EXPORT;
    if (block->kind == MAC_SHEET_LOGOUT) kind = DROP_TASK_LOGOUT;
    drop_task *task = new_task(kind);
    if (task && kind == DROP_TASK_DELETE) strcpy_s(task->message, DROP_ID_CAP, block->message);
    if (task && kind == DROP_TASK_EXPORT) {
        strcpy_s(task->message, DROP_ID_CAP, block->message);
        strcpy_s(task->object, DROP_ID_CAP, block->object);
        id path = mac_get(mac_get(block->panel, "URL"), "path");
        const char *destination = MAC_SEND0(const char *, path, "fileSystemRepresentation");
        if (!destination) {
            drop_task_free(task);
            mac_put(ui->status, "setStringValue:", mac_text("导出位置不可用。"));
            return;
        }
        task->destination = strdup(destination);
    }
    submit(ui, task);
}
static const mac_block_descriptor sheet_descriptor = {0, sizeof(mac_sheet), sheet_copy, sheet_dispose, "v@?q"};

/* 调用方的栈块只负责发起；AppKit 复制后持有至回调结束。未完成确认不能触发删除、导出或注销。 */
static void begin_sheet(mac_ui *ui, id panel, mac_sheet_kind kind, const char *message, const char *object) {
    mac_sheet block = {0};
    block.isa = _NSConcreteStackBlock;
    block.flags = (1 << 25) | (1 << 30);
    block.invoke = sheet_complete;
    block.descriptor = &sheet_descriptor;
    block.ui = ui;
    block.panel = panel;
    block.kind = kind;
    if (message) strcpy_s(block.message, DROP_ID_CAP, message);
    if (object) strcpy_s(block.object, DROP_ID_CAP, object);
    MAC_SEND2(void, panel, "beginSheetModalForWindow:completionHandler:", id, ui->window, const void *, &block);
}
static void confirm(mac_ui *ui, mac_sheet_kind kind, const char *title, const char *description, const char *accept, const char *message) {
    id alert = mac_new("NSAlert");
    mac_put(alert, "setMessageText:", mac_text(title));
    mac_put(alert, "setInformativeText:", mac_text(description));
    MAC_SEND1(id, alert, "addButtonWithTitle:", id, mac_text("取消"));
    MAC_SEND1(id, alert, "addButtonWithTitle:", id, mac_text(accept));
    begin_sheet(ui, alert, kind, message, NULL);
}
/* 仅明文登录任务短暂拥有密码副本，控件立即清空；后台沿既有秘密存储及内存清零流程处理。 */
static void login_account(mac_ui *ui) {
    drop_task *task = new_task(DROP_TASK_LOGIN);
    if (task) {
        task->url = strdup(mac_utf8(mac_get(ui->serverURL, "stringValue")));
        task->login = strdup(mac_utf8(mac_get(ui->login, "stringValue")));
        task->password = strdup(mac_utf8(mac_get(ui->password, "stringValue")));
        task->ca = strdup(mac_utf8(mac_get(ui->ca, "stringValue")));
    }
    mac_put(ui->password, "setStringValue:", mac_text(""));
    submit(ui, task);
}
static void show_settings(mac_ui *ui) {
    if (!ui->settings) {
        mac_owned(&ui->settings, window(560, 350, MAC_WINDOW_TITLED | MAC_WINDOW_CLOSABLE));
        mac_put(ui->settings, "setTitle:", mac_text("Drop · 配置与账号"));
        id root = mac_view("DropView", CGRectMake(0, 0, 560, 350));
        mac_put(ui->settings, "setContentView:", root);
        const char *labels[] = {"服务器地址（http:// 使用 PAKE；https:// 使用 TLS）", "账号", "密码（仅登录时使用）", "私有 CA 文件（可留空）"};
        ui->serverURL = entry("https://HOST:PORT");
        ui->login = entry("账号");
        ui->password = mac_view("NSSecureTextField", CGRectZero);
        ui->ca = entry("私有 CA 路径");
        id fields[] = {ui->serverURL, ui->login, ui->password, ui->ca};
        for (int i = 0; i < 4; ++i) {
            id name = label(mac_text(labels[i]));
            mac_frame(name, 12, 12 + i * 58, 536, 18);
            mac_put(root, "addSubview:", name);
            mac_frame(fields[i], 12, 33 + i * 58, 536, 24);
            mac_put(root, "addSubview:", fields[i]);
        }
        mac_frame(button(ui, "登录并保存", "loginAccount:", root), 12, 252, 116, 28);
        mac_frame(button(ui, "注销", "logoutAccount:", root), 136, 252, 76, 28);
        ui->notify = MAC_SEND3(id, MAC_CLASS("NSButton"), "checkboxWithTitle:target:action:", id, mac_text("系统通知"), id, nil, SEL, NULL);
        mac_frame(ui->notify, 12, 290, 130, 24);
        ui->attention = MAC_SEND3(id, MAC_CLASS("NSButton"), "checkboxWithTitle:target:action:", id, mac_text("菜单栏未读提示"), id, nil, SEL, NULL);
        mac_frame(ui->attention, 146, 290, 170, 24);
        mac_put(root, "addSubview:", ui->notify);
        mac_put(root, "addSubview:", ui->attention);
        mac_frame(button(ui, "保存提醒设置", "savePreferences:", root), 332, 287, 156, 28);
        mac_void(ui->settings, "center");
    }
    if (ui->snapshot && !mac_bool(ui->settings, "isVisible")) {
        mac_put(ui->serverURL, "setStringValue:", mac_text(ui->snapshot->url));
        mac_put(ui->login, "setStringValue:", mac_text(ui->snapshot->login));
        mac_put(ui->ca, "setStringValue:", mac_text(ui->snapshot->ca));
        mac_number(ui->notify, "setState:", ui->snapshot->notify ? 1 : 0);
        mac_number(ui->attention, "setState:", ui->snapshot->attention ? 1 : 0);
    }
    mac_put(ui->settings, "makeKeyAndOrderFront:", nil);
    mac_flag(app(), "activateIgnoringOtherApps:", YES);
}
/* C 后台确认发送持久化成功前，草稿仍留在界面且不可修改；提交失败不会清空文字或附件。 */
static void send_message(mac_ui *ui) {
    id text = mac_get(ui->compose, "string");
    if (!mac_bool(ui->send, "isEnabled") || (!mac_int(text, "length") && !mac_count(ui->paths))) return;
    drop_task *task = new_task(DROP_TASK_SEND);
    if (task) {
        task->text = strdup(mac_utf8(text));
        strcpy_s(task->peer, DROP_ID_CAP, mac_utf8(ui->peer));
        task->count = (int)mac_count(ui->paths);
        if (ui->snapshot && ui->snapshot->history) strcpy_s(task->account, DROP_ID_CAP, ui->snapshot->history->self);
        for (int i = 0; i < task->count; ++i)
            task->paths[i] = strdup(MAC_SEND0(const char *, mac_at(ui->paths, (mac_unsigned)i), "fileSystemRepresentation"));
    }
    if (submit(ui, task)) {
        ui->sendPending = YES;
        ui->pendingFinished = ui->snapshot ? ui->snapshot->send_finished : 0;
        ui->pendingSerial = ui->snapshot ? ui->snapshot->send_serial : 0;
        mac_flag(ui->compose, "setEditable:", NO);
        mac_flag(ui->send, "setEnabled:", NO);
    }
}
/* 保存面板打开前复制消息及附件身份；用户确认后才由后台导出，不依赖可能已被替换的快照。 */
static void export_attachment(mac_ui *ui) {
    if (!ui->snapshot || ui->snapshot->view_serial != ui->viewSerial || !ui->snapshot->history || ui->selectedRow < 0 || ui->selectedRow >= ui->snapshot->history->count) return;
    drop_history_row *message = &ui->snapshot->history->rows[ui->selectedRow];
    if (!message->content || ui->selectedFile < 0 || ui->selectedFile >= message->content->count || !message->content->files[ui->selectedFile].complete) {
        mac_put(ui->status, "setStringValue:", mac_text("请选择已完整保存的附件。"));
        return;
    }
    WCHAR safe[260];
    drop_ui_safe_filename(message->content->files[ui->selectedFile].name, safe);
    id panel = mac_get(MAC_CLASS("NSSavePanel"), "savePanel");
    mac_put(panel, "setTitle:", mac_text("导出所选附件"));
    mac_put(panel, "setNameFieldStringValue:", native_text(safe));
    begin_sheet(ui, panel, MAC_SHEET_EXPORT, message->id, message->content->files[ui->selectedFile].id);
}
/* 菜单和工具栏共享 C 动作入口；这里只操作界面线程状态，涉及账户/文件的工作全部提交给后台。 */
static void action(id self, SEL command, id sender) {
    (void)self;
    mac_ui *ui = active_ui;
    const char *name = sel_getName(command);
    if (!strcmp(name, "showWindow:")) {
        mac_put(ui->window, "makeKeyAndOrderFront:", nil);
        mac_flag(app(), "activateIgnoringOtherApps:", YES);
    } else if (!strcmp(name, "quit:"))
        mac_put(app(), "terminate:", nil);
    else if (!strcmp(name, "synchronize:"))
        submit(ui, new_task(DROP_TASK_SYNC));
    else if (!strcmp(name, "findAccount:")) {
        drop_task *task = new_task(DROP_TASK_FIND);
        if (task) task->login = strdup(mac_utf8(mac_get(ui->find, "stringValue")));
        submit(ui, task);
    } else if (!strcmp(name, "filter:") || !strcmp(name, "previous:") || !strcmp(name, "next:")) {
        if (!strcmp(name, "filter:")) ui->page = 0;
        if (!strcmp(name, "previous:") && ui->page) --ui->page;
        if (!strcmp(name, "next:") && ui->snapshot && ui->snapshot->history && ui->snapshot->history->more) ++ui->page;
        view_request(ui);
    } else if (!strcmp(name, "showFullText:")) {
        if (ui->selectedRow < 0) return;
        if (!mac_bool(ui->document, "isVisible")) mac_void(ui->document, "center");
        mac_put(ui->document, "makeKeyAndOrderFront:", nil);
    } else if (!strcmp(name, "removeAttachment:")) {
        if (ui->sendPending) return;
        mac_put(ui->paths, "removeObject:", mac_get(sender, "representedObject"));
        add_urls(ui, mac_array(0, NULL));
    } else if (!strcmp(name, "addAttachments:")) {
        id panel = mac_get(MAC_CLASS("NSOpenPanel"), "openPanel");
        mac_flag(panel, "setCanChooseDirectories:", NO);
        mac_flag(panel, "setAllowsMultipleSelection:", YES);
        mac_put(panel, "setTitle:", mac_text("添加附件"));
        begin_sheet(ui, panel, MAC_SHEET_ADD, NULL, NULL);
    } else if (!strcmp(name, "clearAttachments:")) {
        if (ui->sendPending) return;
        mac_void(ui->paths, "removeAllObjects");
        add_urls(ui, mac_array(0, NULL));
    } else if (!strcmp(name, "sendMessage:"))
        send_message(ui);
    else if (!strcmp(name, "deleteMessage:")) {
        if (!mac_bool(ui->deleteButton, "isEnabled") || !mac_int(ui->selected, "length")) return;
        confirm(ui, MAC_SHEET_DELETE, "删除所选消息？", "删除将同步至服务器及双方账号的所有设备。用户原文件、已导出文件和旧备份保留。", "删除", mac_utf8(ui->selected));
    } else if (!strcmp(name, "reviewMessages:"))
        confirm(ui, MAC_SHEET_REVIEW, "重新提交已复核的待发送任务？", "请先检查服务器恢复后的记录，确认需要重新发送。", "重新提交", NULL);
    else if (!strcmp(name, "exportAttachment:"))
        export_attachment(ui);
    else if (!strcmp(name, "showSettings:"))
        show_settings(ui);
    else if (!strcmp(name, "loginAccount:"))
        login_account(ui);
    else if (!strcmp(name, "savePreferences:")) {
        drop_task *task = new_task(DROP_TASK_PREFERENCES);
        if (task) {
            task->notify = mac_int(ui->notify, "state") == 1;
            task->attention = mac_int(ui->attention, "state") == 1;
        }
        submit(ui, task);
    } else if (!strcmp(name, "logoutAccount:"))
        confirm(ui, MAC_SHEET_LOGOUT, "注销当前账号？", "清除本机登录凭据，历史正文和附件保留。", "注销", NULL);
}

static BOOL flipped(id self, SEL command) {
    (void)self;
    (void)command;
    return YES;
}
static void draw_view(id self, SEL command, CGRect dirty) {
    (void)command;
    id background = !strcmp(class_getName(object_getClass(self)), "DropBand") ? color(.86, .91, .97) : mac_get(MAC_CLASS("NSColor"), "windowBackgroundColor");
    mac_void(background, "setFill");
    NSRectFill(dirty);
}
/* 普通文字保留 NSTextView 行为；文件 URL 先交给草稿列表，不能让富文本粘贴替代附件身份。 */
static id pasteboard_urls(id pasteboard) {
    id classes[] = {MAC_CLASS("NSURL")};
    id keys[] = {NSPasteboardURLReadingFileURLsOnlyKey};
    id values[] = {MAC_SEND1(id, MAC_CLASS("NSNumber"), "numberWithBool:", BOOL, YES)};
    return MAC_SEND2(id, pasteboard, "readObjectsForClasses:options:", id, mac_array(1, classes), id, mac_dictionary(1, keys, values));
}
/* super 调用必须从实际父类开始，不能重新分发到自己的 C 回调。 */
static void super_event(id self, SEL command, id event) {
    struct objc_super base = {self, class_getSuperclass(object_getClass(self))};
    ((void (*)(struct objc_super *, SEL, id))objc_msgSendSuper)(&base, command, event);
}
static void paste(id self, SEL command, id sender) {
    id urls = pasteboard_urls(mac_get(MAC_CLASS("NSPasteboard"), "generalPasteboard"));
    if (mac_count(urls))
        add_urls(active_ui, urls);
    else
        super_event(self, command, sender);
}
static mac_unsigned dragging(id self, SEL command, id sender) {
    if (mac_count(pasteboard_urls(mac_get(sender, "draggingPasteboard")))) return 1;
    struct objc_super base = {self, class_getSuperclass(object_getClass(self))};
    return ((mac_unsigned (*)(struct objc_super *, SEL, id))objc_msgSendSuper)(&base, command, sender);
}
static BOOL drop_files(id self, SEL command, id sender) {
    id urls = pasteboard_urls(mac_get(sender, "draggingPasteboard"));
    if (mac_count(urls)) {
        add_urls(active_ui, urls);
        return YES;
    }
    struct objc_super base = {self, class_getSuperclass(object_getClass(self))};
    return ((BOOL (*)(struct objc_super *, SEL, id))objc_msgSendSuper)(&base, command, sender);
}
static void key_down(id self, SEL command, id event) {
    unsigned short key = MAC_SEND0(unsigned short, event, "keyCode");
    if (self == active_ui->compose && (MAC_SEND0(mac_unsigned, event, "modifierFlags") & (1UL << 20)) && key == 36)
        action(active_ui->controller, MAC_SEL("sendMessage:"), self);
    else if (self == active_ui->messages && (key == 51 || key == 117))
        action(active_ui->controller, MAC_SEL("deleteMessage:"), self);
    else
        super_event(self, command, event);
}
static void mouse_down(id self, SEL command, id event) {
    super_event(self, command, event);
    if (mac_int(event, "clickCount") == 2) action(active_ui->controller, MAC_SEL("showFullText:"), self);
}
static id message_menu(id self, SEL command, id event) {
    struct objc_super base = {self, class_getSuperclass(object_getClass(self))};
    id original = ((id (*)(struct objc_super *, SEL, id))objc_msgSendSuper)(&base, command, event);
    id context = mac_autorelease(mac_get(original, "copy"));
    if (!context) context = menu("消息");
    mac_put(context, "addItem:", mac_get(MAC_CLASS("NSMenuItem"), "separatorItem"));
    menu_item(active_ui, context, "查看完整正文", "showFullText:", "");
    menu_item(active_ui, context, "删除所选消息", "deleteMessage:", "");
    menu_item(active_ui, context, "导出所选附件", "exportAttachment:", "");
    return context;
}
static BOOL validate_menu(id self, SEL command, id item) {
    (void)self;
    (void)command;
    SEL target = MAC_SEND0(SEL, item, "action");
    if (sel_isEqual(target, MAC_SEL("deleteMessage:"))) return mac_bool(active_ui->deleteButton, "isEnabled");
    if (sel_isEqual(target, MAC_SEL("exportAttachment:"))) return mac_bool(active_ui->exportButton, "isEnabled");
    return YES;
}
static void resize(id self, SEL command, id notification) {
    (void)self;
    (void)command;
    (void)notification;
    layout(active_ui);
}
static CGFloat split_min(id self, SEL command, id split, CGFloat proposed, mac_integer index) {
    (void)self;
    (void)command;
    (void)split;
    (void)proposed;
    (void)index;
    return 160;
}
static CGFloat split_max(id self, SEL command, id split, CGFloat proposed, mac_integer index) {
    (void)self;
    (void)command;
    (void)proposed;
    (void)index;
    return mac_rect(split, "bounds").size.width - 550;
}
static BOOL close_window(id self, SEL command, id view) {
    (void)self;
    (void)command;
    mac_put(view, "orderOut:", nil);
    return NO;
}
static BOOL keep_running(id self, SEL command, id application) {
    (void)self;
    (void)command;
    (void)application;
    return NO;
}
static void poll(id self, SEL command, id timer) {
    (void)self;
    (void)command;
    (void)timer;
    if (drop_process_stop_requested()) {
        action(active_ui->controller, MAC_SEL("quit:"), nil);
        return;
    }
    drop_snapshot *snapshot = drop_runtime_take(active_ui->runtime);
    if (snapshot) apply_snapshot(active_ui, snapshot);
}
/* 关闭标志先发布给所有界面回调，再停止并等待后台；释放快照之前后台已经不再生成新事件。 */
static void stop(id self, SEL command, id notification) {
    (void)self;
    (void)command;
    (void)notification;
    mac_ui *ui = active_ui;
    if (ui->closing) return;
    ui->closing = YES;
    mac_void(ui->timer, "invalidate");
    drop_runtime_stop(ui->runtime);
    ui->runtime = NULL;
    drop_snapshot_free(ui->snapshot);
    ui->snapshot = NULL;
    mac_put(mac_get(MAC_CLASS("NSStatusBar"), "systemStatusBar"), "removeStatusItem:", ui->statusItem);
}
typedef struct mac_auth_block {
    void *isa;
    int flags, reserved;
    void (*invoke)(const struct mac_auth_block *block, BOOL allowed, id error);
    const struct mac_auth_descriptor *descriptor;
} mac_auth_block;
typedef struct mac_auth_descriptor {
    unsigned long reserved, size;
    const char *signature;
} mac_auth_descriptor;
static void auth_complete(const mac_auth_block *block, BOOL allowed, id error) {
    (void)block;
    (void)allowed;
    (void)error;
}
#if __OBJC_BOOL_IS_BOOL
#define MAC_BOOL_ENCODING "B"
#else
#define MAC_BOOL_ENCODING "c"
#endif
static const mac_auth_descriptor auth_descriptor = {0, sizeof(mac_auth_block), "v@?" MAC_BOOL_ENCODING "@"};
/* 无捕获的全局 C 回调具有进程生命周期，可供通知框架在自己的线程调用；它不访问任何界面或后台状态。 */
static const mac_auth_block auth_block = {_NSConcreteGlobalBlock, (1 << 28) | (1 << 30), 0, auth_complete, &auth_descriptor};

static void launched(id self, SEL command, id notification) {
    (void)self;
    (void)command;
    (void)notification;
    mac_ui *ui = active_ui;
    build_window(ui);
    mac_owned(&ui->statusItem, MAC_SEND1(id, mac_get(MAC_CLASS("NSStatusBar"), "systemStatusBar"), "statusItemWithLength:", CGFloat, -1));
    id status_button = mac_get(ui->statusItem, "button");
    mac_put(status_button, "setTitle:", mac_text("Drop"));
    id icon = MAC_SEND1(id, MAC_CLASS("NSImage"), "imageNamed:", id, mac_text("Drop"));
    if (!icon) icon = MAC_SEND1(id, MAC_CLASS("NSImage"), "imageNamed:", id, NSImageNameActionTemplate);
    MAC_SEND1(void, icon, "setSize:", CGSize, CGSizeMake(18, 18));
    mac_put(status_button, "setImage:", icon);
    id tray = menu("Drop");
    menu_item(ui, tray, "打开消息", "showWindow:", "");
    menu_item(ui, tray, "同步", "synchronize:", "");
    menu_item(ui, tray, "配置与账号", "showSettings:", "");
    menu_item(ui, tray, "真正退出", "quit:", "");
    mac_put(ui->statusItem, "setMenu:", tray);
    id main = menu("Drop"), app_item = menu_item(ui, main, "Drop", NULL, ""), app_menu = menu("Drop");
    menu_item(ui, app_menu, "配置与账号…", "showSettings:", ",");
    mac_put(app_menu, "addItem:", mac_get(MAC_CLASS("NSMenuItem"), "separatorItem"));
    menu_item(ui, app_menu, "退出 Drop", "quit:", "q");
    mac_put(app_item, "setSubmenu:", app_menu);
    id edit_item = menu_item(ui, main, "编辑", NULL, ""), edit = menu("编辑");
    const char *edit_titles[] = {"复制", "粘贴", "全选"}, *edit_actions[] = {"copy:", "paste:", "selectAll:"}, *edit_keys[] = {"c", "v", "a"};
    for (int i = 0; i < 3; ++i) {
        id item = menu_item(ui, edit, edit_titles[i], edit_actions[i], edit_keys[i]);
        mac_put(item, "setTarget:", nil);
    }
    mac_put(edit_item, "setSubmenu:", edit);
    id conversation_item = menu_item(ui, main, "会话", NULL, ""), conversation = menu("会话");
    menu_item(ui, conversation, "同步", "synchronize:", "");
    menu_item(ui, conversation, "导出所选附件", "exportAttachment:", "");
    menu_item(ui, conversation, "查看完整正文", "showFullText:", "");
    menu_item(ui, conversation, "删除所选消息", "deleteMessage:", "");
    menu_item(ui, conversation, "恢复后重新提交已复核任务", "reviewMessages:", "");
    mac_put(conversation_item, "setSubmenu:", conversation);
    mac_put(app(), "setMainMenu:", main);
    mac_owned(&ui->timer,
        MAC_SEND5(id, MAC_CLASS("NSTimer"), "scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:", double, .2, id, ui->controller, SEL, MAC_SEL("poll:"), id, nil,
            BOOL, YES));
    if (!ui->hidden) action(ui->controller, MAC_SEL("showWindow:"), nil);
#ifdef DROP_GUI_CHECKS
    extern void drop_macos_probe_start(void);
    drop_macos_probe_start();
#endif
    if (mac_get(mac_get(MAC_CLASS("NSBundle"), "mainBundle"), "bundleIdentifier")) {
        MAC_SEND2(void, mac_get(MAC_CLASS("UNUserNotificationCenter"), "currentNotificationCenter"), "requestAuthorizationWithOptions:completionHandler:", mac_unsigned,
            MAC_NOTIFICATION_SOUND | MAC_NOTIFICATION_ALERT, const void *, &auth_block);
    }
}
/* 自定义类仅注册 C 函数；已有系统方法沿用父类类型编码，其余委托方法使用 SDK 的协议声明进行复核。 */
static BOOL add_method(Class type, const char *name, IMP implementation, const char *fallback) {
    SEL selector = MAC_SEL(name);
    Method inherited = class_getInstanceMethod(class_getSuperclass(type), selector);
    const char *encoding = inherited ? method_getTypeEncoding(inherited) : fallback;
    const char *protocols[] = {
        "NSApplicationDelegate", "NSWindowDelegate", "NSTableViewDataSource", "NSTableViewDelegate", "NSTextViewDelegate", "NSSplitViewDelegate", "NSMenuItemValidation"};
    for (size_t i = 0; !inherited && i < sizeof(protocols) / sizeof(protocols[0]); ++i) {
        Protocol *protocol = objc_getProtocol(protocols[i]);
        if (!protocol) continue;
        struct objc_method_description method = protocol_getMethodDescription(protocol, selector, NO, YES);
        if (!method.name) method = protocol_getMethodDescription(protocol, selector, YES, YES);
        if (method.types) {
            encoding = method.types;
            break;
        }
    }
    return class_addMethod(type, selector, implementation, encoding);
}
#define MAC_METHOD(type, selector, function, signature) (add_method(type, selector, (IMP)(function), signature))

/* 注册失败时不运行半完成的界面；每个进程只创建一个主线程界面实例。 */
static BOOL register_classes(void) {
    Class view = objc_allocateClassPair(objc_getClass("NSView"), "DropView", 0);
    if (!view || !MAC_METHOD(view, "isFlipped", flipped, MAC_BOOL_ENCODING "@:") || !MAC_METHOD(view, "drawRect:", draw_view, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}")) return NO;
    objc_registerClassPair(view);
    Class band = objc_allocateClassPair(view, "DropBand", 0);
    if (!band || !MAC_METHOD(band, "drawRect:", draw_view, "v@:{CGRect={CGPoint=dd}{CGSize=dd}}")) return NO;
    objc_registerClassPair(band);
    Class composer = objc_allocateClassPair(objc_getClass("NSTextView"), "DropComposer", 0);
    if (!composer || !MAC_METHOD(composer, "paste:", paste, "v@:@") || !MAC_METHOD(composer, "draggingEntered:", dragging, "Q@:@") ||
        !MAC_METHOD(composer, "performDragOperation:", drop_files, MAC_BOOL_ENCODING "@:@") || !MAC_METHOD(composer, "keyDown:", key_down, "v@:@"))
        return NO;
    objc_registerClassPair(composer);
    Class messages = objc_allocateClassPair(objc_getClass("NSTextView"), "DropMessages", 0);
    if (!messages || !MAC_METHOD(messages, "mouseDown:", mouse_down, "v@:@") || !MAC_METHOD(messages, "keyDown:", key_down, "v@:@") ||
        !MAC_METHOD(messages, "menuForEvent:", message_menu, "@@:@"))
        return NO;
    objc_registerClassPair(messages);
    Class controller = objc_allocateClassPair(objc_getClass("NSObject"), "DropController", 0);
    if (!controller) return NO;
    const char *protocols[] = {"NSApplicationDelegate", "NSWindowDelegate", "NSTableViewDataSource", "NSTableViewDelegate", "NSTextViewDelegate", "NSSplitViewDelegate"};
    for (size_t i = 0; i < sizeof(protocols) / sizeof(protocols[0]); ++i) {
        Protocol *protocol = objc_getProtocol(protocols[i]);
        if (protocol) class_addProtocol(controller, protocol);
    }
    const char *actions[] = {"showWindow:", "quit:", "synchronize:", "findAccount:", "filter:", "previous:", "next:", "showFullText:", "removeAttachment:", "addAttachments:",
        "clearAttachments:", "sendMessage:", "deleteMessage:", "reviewMessages:", "exportAttachment:", "showSettings:", "loginAccount:", "savePreferences:", "logoutAccount:"};
    for (size_t i = 0; i < sizeof(actions) / sizeof(actions[0]); ++i) {
        if (!MAC_METHOD(controller, actions[i], action, "v@:@")) return NO;
    }
    if (!MAC_METHOD(controller, "applicationDidFinishLaunching:", launched, "v@:@") || !MAC_METHOD(controller, "applicationWillTerminate:", stop, "v@:@") ||
        !MAC_METHOD(controller, "applicationShouldTerminateAfterLastWindowClosed:", keep_running, MAC_BOOL_ENCODING "@:@") ||
        !MAC_METHOD(controller, "windowShouldClose:", close_window, MAC_BOOL_ENCODING "@:@") || !MAC_METHOD(controller, "windowDidResize:", resize, "v@:@") ||
        !MAC_METHOD(controller, "splitViewDidResizeSubviews:", resize, "v@:@") || !MAC_METHOD(controller, "splitView:constrainMinCoordinate:ofSubviewAt:", split_min, "d@:@dq") ||
        !MAC_METHOD(controller, "splitView:constrainMaxCoordinate:ofSubviewAt:", split_max, "d@:@dq") || !MAC_METHOD(controller, "numberOfRowsInTableView:", table_rows, "q@:@") ||
        !MAC_METHOD(controller, "tableView:viewForTableColumn:row:", table_cell, "@@:@@q") || !MAC_METHOD(controller, "tableViewSelectionDidChange:", table_selection, "v@:@") ||
        !MAC_METHOD(controller, "textViewDidChangeSelection:", text_selection, "v@:@") || !MAC_METHOD(controller, "validateMenuItem:", validate_menu, MAC_BOOL_ENCODING "@:@") ||
        !MAC_METHOD(controller, "poll:", poll, "v@:@"))
        return NO;
    objc_registerClassPair(controller);
    return YES;
}

#ifdef DROP_GUI_CHECKS
#include "../checks/macos_probe_c.inc"
#endif

int drop_native_ui(const WCHAR *root, int hidden) {
    if (active_ui) return 1;
    id pool = mac_get(mac_alloc("NSAutoreleasePool"), "init");
    mac_ui ui = {0};
    active_ui = &ui;
    ui.runtime = drop_runtime_start(root);
    ui.hidden = hidden != 0;
    if (!ui.runtime || !register_classes()) {
        fprintf(stderr, "Drop data directory or native interface unavailable.\n");
        if (ui.runtime) drop_runtime_stop(ui.runtime);
        active_ui = NULL;
        mac_void(pool, "drain");
        return 1;
    }
    /* NSApplication.delegate 不拥有控制器；此引用必须跨越整个事件循环，退出前先解除所有委托。 */
    ui.controller = mac_get(mac_alloc("DropController"), "init");
    mac_number(app(), "setActivationPolicy:", 0);
    mac_put(app(), "setDelegate:", ui.controller);
    mac_void(app(), "finishLaunching");
    mac_void(app(), "run");
    stop(ui.controller, NULL, nil);
    mac_put(app(), "setDelegate:", nil);
    mac_put(ui.window, "setDelegate:", nil);
    mac_put(ui.split, "setDelegate:", nil);
    mac_put(ui.users, "setDelegate:", nil);
    mac_put(ui.users, "setDataSource:", nil);
    mac_put(ui.files, "setDelegate:", nil);
    mac_put(ui.files, "setDataSource:", nil);
    mac_put(ui.draft, "setDelegate:", nil);
    mac_put(ui.draft, "setDataSource:", nil);
    mac_put(ui.messages, "setDelegate:", nil);
    id owned[] = {ui.window, ui.settings, ui.document, ui.statusItem, ui.paths, ui.userIDs, ui.ranges, ui.fileRanges, ui.timer, ui.peer, ui.selected, ui.controller};
    for (size_t i = 0; i < sizeof(owned) / sizeof(owned[0]); ++i)
        mac_release(owned[i]);
    active_ui = NULL;
    mac_void(pool, "drain");
    return 0;
}
