/* AppKit 只负责原生控件、剪贴板和菜单栏；凭据、事务、附件及消息删除均交给 C 后台核心。 */
#import <AppKit/AppKit.h>
#import <UserNotifications/UserNotifications.h>
#include "runtime.h"
#include <sodium.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>

/* C 宽字符按明确 UTF-8 转码，不能把 POSIX wchar_t 当作 NSString 的 UTF-16 内存。 */
static NSString *native_text(const WCHAR *wide) {
    size_t capacity = (wcslen(wide) + 1) * 4;
    char *utf8 = malloc(capacity);
    NSString *text = @"";
    if (utf8 && drop_to_utf8(wide, utf8, capacity)) {
        NSString *converted = [NSString stringWithUTF8String:utf8];
        if (converted) text = converted;
    }
    free(utf8);
    return text;
}
static drop_task *new_task(drop_task_kind kind) {
    drop_task *task = calloc(1, sizeof(*task));
    if (task) task->kind = kind;
    return task;
}
@interface DropView : NSView
@end
@implementation DropView
- (BOOL)isFlipped {
    return YES;
}
- (void)drawRect:(NSRect)dirty {
    [[NSColor windowBackgroundColor] setFill];
    NSRectFill(dirty);
}
@end

/* 色带只绘制背景，标签保留原生单行高度后放在色带中央，避免文本随色带高度贴顶。 */
@interface DropBand : DropView
@end
@implementation DropBand
- (void)drawRect:(NSRect)dirty {
    [[NSColor colorWithCalibratedRed:0.86 green:0.91 blue:0.97 alpha:1] setFill];
    NSRectFill(dirty);
}
@end

@class DropController;
@interface DropComposer : NSTextView
@property(nonatomic, weak) DropController *owner;
@end
@interface DropMessages : NSTextView
@property(nonatomic, weak) DropController *owner;
@end

@interface DropController : NSObject <NSApplicationDelegate, NSWindowDelegate, NSTableViewDataSource, NSTableViewDelegate, NSTextViewDelegate, NSSplitViewDelegate>
@property(nonatomic, strong) NSWindow *window, *settings, *document;
@property(nonatomic, strong) NSStatusItem *statusItem;
@property(nonatomic, strong) NSTableView *users, *files, *draft;
@property(nonatomic, strong) NSTextView *messages, *detail;
@property(nonatomic, strong) DropComposer *compose;
@property(nonatomic, strong) NSTextField *status, *recipient, *overview, *draftLabel, *fileLabel, *search, *find, *from, *to, *serverURL, *login, *ca;
@property(nonatomic, strong) NSSecureTextField *password;
@property(nonatomic, strong) NSButton *send, *deleteButton, *exportButton;
@property(nonatomic, strong) NSButton *notify, *attention;
@property(nonatomic, strong) NSMutableArray<NSString *> *paths;
@property(nonatomic, strong) NSMutableArray<NSValue *> *ranges;
@property(nonatomic, strong) NSMutableArray<NSArray<NSValue *> *> *fileRanges;
@property(nonatomic, strong) NSTimer *timer;
@property(nonatomic, strong) NSSplitView *split;
@property(nonatomic, strong) NSView *left, *right, *recipientBand, *overviewBand;
@property(nonatomic, strong) NSScrollView *messageScroll, *userScroll, *detailScroll, *fileScroll, *composeScroll, *draftScroll;
@property(nonatomic) drop_runtime *runtime;
@property(nonatomic) drop_snapshot *snapshot;
@property(nonatomic) NSInteger selectedRow, selectedFile, page;
@property(nonatomic) BOOL rebuilding, hidden;
@property(nonatomic) BOOL sendPending;
@property(nonatomic) uint64_t pendingFinished, pendingSerial;
@property(nonatomic, copy) NSString *peer, *selected;
- (void)sendMessage:(id)sender;
- (void)addURLs:(NSArray<NSURL *> *)urls;
- (void)deleteMessage:(id)sender;
- (void)exportAttachment:(id)sender;
- (void)showFullText:(id)sender;
@end

@implementation DropComposer
/* 文件粘贴由待发送预览接管，普通文字继续使用原生编辑行为。 */
- (void)paste:(id)sender {
    NSArray *urls = [[NSPasteboard generalPasteboard] readObjectsForClasses:@[ [NSURL class] ] options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    if (urls.count)
        [self.owner addURLs:urls];
    else
        [super paste:sender];
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    return [[sender draggingPasteboard] canReadObjectForClasses:@[ [NSURL class] ] options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}] ? NSDragOperationCopy
                                                                                                                                            : [super draggingEntered:sender];
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    NSArray *urls = [[sender draggingPasteboard] readObjectsForClasses:@[ [NSURL class] ] options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    if (urls.count) {
        [self.owner addURLs:urls];
        return YES;
    }
    return [super performDragOperation:sender];
}
- (void)keyDown:(NSEvent *)event {
    if ((event.modifierFlags & NSEventModifierFlagCommand) && event.keyCode == 36)
        [self.owner sendMessage:self];
    else
        [super keyDown:event];
}
@end
@implementation DropMessages
- (void)mouseDown:(NSEvent *)event {
    [super mouseDown:event];
    if (event.clickCount == 2) [self.owner showFullText:self];
}
- (void)keyDown:(NSEvent *)event {
    if (event.keyCode == 51 || event.keyCode == 117)
        [self.owner deleteMessage:self];
    else
        [super keyDown:event];
}
- (NSMenu *)menuForEvent:(NSEvent *)event {
    NSMenu *menu = [[super menuForEvent:event] copy];
    if (!menu) menu = [[NSMenu alloc] initWithTitle:@"消息"];
    [menu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *body = [[NSMenuItem alloc] initWithTitle:@"查看完整正文" action:@selector(showFullText:) keyEquivalent:@""];
    body.target = self.owner;
    [menu addItem:body];
    NSMenuItem *remove = [[NSMenuItem alloc] initWithTitle:@"删除所选消息" action:@selector(deleteMessage:) keyEquivalent:@""];
    remove.target = self.owner;
    [menu addItem:remove];
    NSMenuItem *export = [[NSMenuItem alloc] initWithTitle:@"导出所选附件" action:@selector(exportAttachment:) keyEquivalent:@""];
    export.target = self.owner;
    [menu addItem:export];
    return menu;
}
@end

#ifdef DROP_GUI_CHECKS
static void gui_probe_start(DropController *controller);
#endif
@implementation DropController
- (NSTextField *)label:(NSString *)text {
    NSTextField *label = [NSTextField labelWithString:text];
    label.font = [NSFont systemFontOfSize:12];
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    return label;
}
- (NSTextField *)entry:(NSString *)placeholder {
    NSTextField *entry = [[NSTextField alloc] initWithFrame:NSZeroRect];
    entry.font = [NSFont systemFontOfSize:12];
    entry.placeholderString = placeholder;
    return entry;
}
- (NSButton *)button:(NSString *)title action:(SEL)action parent:(NSView *)parent {
    NSButton *button = [NSButton buttonWithTitle:title target:self action:action];
    button.bezelStyle = NSBezelStyleRounded;
    button.font = [NSFont systemFontOfSize:12];
    [parent addSubview:button];
    return button;
}
- (NSScrollView *)scroll:(NSView *)view {
    NSScrollView *scroll = [[NSScrollView alloc] initWithFrame:NSZeroRect];
    scroll.hasVerticalScroller = YES;
    scroll.hasHorizontalScroller = NO;
    scroll.borderType = NSBezelBorder;
    scroll.documentView = view;
    return scroll;
}
- (NSTextView *)textView:(BOOL)editable {
    NSTextView *view = [[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, 400, 60)];
    view.editable = editable;
    view.selectable = YES;
    view.richText = NO;
    view.font = [NSFont systemFontOfSize:13];
    view.textContainerInset = NSMakeSize(3, 2);
    view.verticallyResizable = YES;
    view.horizontallyResizable = NO;
    view.autoresizingMask = NSViewWidthSizable;
    view.textContainer.widthTracksTextView = YES;
    view.textContainer.containerSize = NSMakeSize(400, CGFLOAT_MAX);
    return view;
}
/* 界面任务不能借用 NSString 的临时 UTF-8 内存；每个字段复制后由 C 后台统一释放。 */
- (BOOL)submit:(drop_task *)task {
    if (task && drop_runtime_submit(self.runtime, task)) return YES;
    drop_task_free(task);
    self.status.stringValue = @"任务队列忙，请稍后重试。";
    return NO;
}
- (void)applicationDidFinishLaunching:(NSNotification *)notification {
    (void)notification;
    self.paths = [NSMutableArray array];
    self.ranges = [NSMutableArray array];
    self.fileRanges = [NSMutableArray array];
    self.peer = @"";
    self.selected = @"";
    self.selectedRow = -1;
    self.selectedFile = -1;
    self.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1080, 760)
                                              styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
    self.window.title = @"Drop " @DROP_VERSION;
    self.window.minSize = NSMakeSize(820, 600);
    self.window.delegate = self;
    self.window.releasedWhenClosed = NO;
    self.window.contentView = [[DropView alloc] initWithFrame:NSMakeRect(0, 0, 1080, 760)];
    NSView *root = self.window.contentView;
    NSButton *sync = [self button:@"同步" action:@selector(synchronize:) parent:root];
    sync.frame = NSMakeRect(4, 4, 66, 28);
    NSButton *settings = [self button:@"配置与账号" action:@selector(showSettings:) parent:root];
    settings.frame = NSMakeRect(74, 4, 104, 28);
    self.exportButton = [self button:@"导出附件" action:@selector(exportAttachment:) parent:root];
    self.exportButton.frame = NSMakeRect(182, 4, 94, 28);
    self.deleteButton = [self button:@"删除消息" action:@selector(deleteMessage:) parent:root];
    self.deleteButton.frame = NSMakeRect(280, 4, 94, 28);
    NSButton *quit = [self button:@"真正退出" action:@selector(quit:) parent:root];
    quit.frame = NSMakeRect(978, 4, 98, 28);
    quit.autoresizingMask = NSViewMinXMargin;
    self.split = [[NSSplitView alloc] initWithFrame:NSMakeRect(4, 36, 1072, 696)];
    self.split.vertical = YES;
    self.split.dividerStyle = NSSplitViewDividerStyleThin;
    self.split.delegate = self;
    self.split.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    self.left = [[DropView alloc] initWithFrame:NSMakeRect(0, 0, 216, 696)];
    self.right = [[DropView alloc] initWithFrame:NSMakeRect(217, 0, 855, 696)];
    [self.split addSubview:self.left];
    [self.split addSubview:self.right];
    [root addSubview:self.split];
    [self.split setPosition:216 ofDividerAtIndex:0];
    self.find = [self entry:@"账号 / 查找用户"];
    [self.left addSubview:self.find];
    [self button:@"查找" action:@selector(findAccount:) parent:self.left].identifier = @"find-button";
    self.users = [[NSTableView alloc] initWithFrame:NSZeroRect];
    self.users.rowHeight = 22;
    self.users.dataSource = self;
    self.users.delegate = self;
    NSTableColumn *name = [[NSTableColumn alloc] initWithIdentifier:@"name"];
    name.title = @"用户 / 账号";
    name.width = 165;
    [self.users addTableColumn:name];
    NSTableColumn *unread = [[NSTableColumn alloc] initWithIdentifier:@"unread"];
    unread.title = @"未读";
    unread.width = 42;
    [self.users addTableColumn:unread];
    self.userScroll = [self scroll:self.users];
    [self.left addSubview:self.userScroll];
    self.search = [self entry:@"当前会话正文"];
    self.from = [self entry:@"起日 YYYY-MM-DD"];
    self.to = [self entry:@"止日 YYYY-MM-DD"];
    [self.right addSubview:self.search];
    [self.right addSubview:self.from];
    [self.right addSubview:self.to];
    [self button:@"查询" action:@selector(filter:) parent:self.right].identifier = @"filter-button";
    [self button:@"‹" action:@selector(previous:) parent:self.right].identifier = @"previous-button";
    [self button:@"›" action:@selector(next:) parent:self.right].identifier = @"next-button";
    self.messages = [[DropMessages alloc] initWithFrame:NSMakeRect(0, 0, 400, 60)];
    ((DropMessages *)self.messages).owner = self;
    self.messages.editable = NO;
    self.messages.selectable = YES;
    self.messages.font = [NSFont systemFontOfSize:13];
    self.messages.textContainerInset = NSMakeSize(3, 2);
    self.messages.verticallyResizable = YES;
    self.messages.autoresizingMask = NSViewWidthSizable;
    self.messages.textContainer.widthTracksTextView = YES;
    self.messages.textContainer.containerSize = NSMakeSize(400, CGFLOAT_MAX);
    self.messages.richText = YES;
    self.messages.delegate = self;
    self.messageScroll = [self scroll:self.messages];
    [self.right addSubview:self.messageScroll];
    self.detail = [self textView:NO];
    self.detailScroll = [self scroll:self.detail];
    self.document = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 760, 500)
                                                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    self.document.title = @"Drop " DROP_VERSION @" · 完整正文（可选择复制）";
    self.document.releasedWhenClosed = NO;
    self.detailScroll.frame = self.document.contentView.bounds;
    self.detailScroll.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [self.document.contentView addSubview:self.detailScroll];
    self.files = [[NSTableView alloc] initWithFrame:NSZeroRect];
    self.files.rowHeight = 22;
    self.files.dataSource = self;
    self.files.delegate = self;
    for (NSString *identifier in @[ @"file", @"size", @"state" ]) {
        NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:identifier];
        column.title = [@{@"file" : @"文件名", @"size" : @"大小", @"state" : @"状态"} objectForKey:identifier];
        column.width = [identifier isEqual:@"file"] ? 220 : 74;
        [self.files addTableColumn:column];
    }
    self.fileScroll = [self scroll:self.files];
    [self.right addSubview:self.fileScroll];
    self.recipient = [self label:@"发送给：请先登录"];
    self.recipient.textColor = [NSColor colorWithCalibratedRed:0.13 green:0.36 blue:0.59 alpha:1];
    self.recipientBand = [[DropBand alloc] initWithFrame:NSZeroRect];
    [self.recipientBand addSubview:self.recipient];
    [self.right addSubview:self.recipientBand];
    self.overview = [self label:@"当前会话 · 请先登录"];
    self.overview.textColor = self.recipient.textColor;
    self.overviewBand = [[DropBand alloc] initWithFrame:NSZeroRect];
    [self.overviewBand addSubview:self.overview];
    [self.right addSubview:self.overviewBand];
    self.draftLabel = [self label:@"待发送附件（0）"];
    self.fileLabel = [self label:@"所选消息的附件"];
    [self.right addSubview:self.draftLabel];
    [self.right addSubview:self.fileLabel];
    self.draft = [[NSTableView alloc] initWithFrame:NSZeroRect];
    self.draft.rowHeight = 24;
    self.draft.dataSource = self;
    self.draft.delegate = self;
    for (NSString *identifier in @[ @"file", @"size" ]) {
        NSTableColumn *column = [[NSTableColumn alloc] initWithIdentifier:identifier];
        column.title = [identifier isEqual:@"file"] ? @"待发送文件" : @"大小";
        column.width = [identifier isEqual:@"file"] ? 240 : 74;
        [self.draft addTableColumn:column];
    }
    self.draftScroll = [self scroll:self.draft];
    [self.right addSubview:self.draftScroll];
    self.compose = [[DropComposer alloc] initWithFrame:NSMakeRect(0, 0, 400, 56)];
    self.compose.owner = self;
    self.compose.font = [NSFont systemFontOfSize:13];
    self.compose.richText = NO;
    self.compose.textContainerInset = NSMakeSize(3, 2);
    self.compose.autoresizingMask = NSViewWidthSizable;
    [self.compose registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];
    self.composeScroll = [self scroll:self.compose];
    [self.right addSubview:self.composeScroll];
    [self button:@"添加附件" action:@selector(addAttachments:) parent:self.right].identifier = @"add-button";
    self.send = [self button:@"发送" action:@selector(sendMessage:) parent:self.right];
    [self button:@"清空附件" action:@selector(clearAttachments:) parent:self.right].identifier = @"clear-button";
    self.status = [self label:@"正在打开本机数据…"];
    self.status.frame = NSMakeRect(6, 737, 1068, 20);
    self.status.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [root addSubview:self.status];
    self.send.enabled = NO;
    self.exportButton.enabled = NO;
    self.deleteButton.enabled = NO;
    [self layout];
    [self.window center];
    self.statusItem = [[NSStatusBar systemStatusBar] statusItemWithLength:NSVariableStatusItemLength];
    self.statusItem.button.title = @"Drop";
    NSImage *icon = [NSImage imageNamed:@"Drop"];
    if (!icon) icon = [NSImage imageNamed:NSImageNameActionTemplate];
    icon.size = NSMakeSize(18, 18);
    self.statusItem.button.image = icon;
    NSMenu *tray = [[NSMenu alloc] initWithTitle:@"Drop"];
    for (NSArray *item in @[
             @[ @"打开消息", NSStringFromSelector(@selector(showWindow:)) ], @[ @"同步", NSStringFromSelector(@selector(synchronize:)) ],
             @[ @"配置与账号", NSStringFromSelector(@selector(showSettings:)) ], @[ @"真正退出", NSStringFromSelector(@selector(quit:)) ]
         ]) {
        NSMenuItem *menu = [[NSMenuItem alloc] initWithTitle:item[0] action:NSSelectorFromString(item[1]) keyEquivalent:@""];
        menu.target = self;
        [tray addItem:menu];
    }
    self.statusItem.menu = tray;
    NSMenu *main = [[NSMenu alloc] initWithTitle:@"Drop"];
    NSMenuItem *appItem = [[NSMenuItem alloc] initWithTitle:@"Drop" action:nil keyEquivalent:@""];
    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"Drop"];
    NSMenuItem *preferences = [[NSMenuItem alloc] initWithTitle:@"配置与账号…" action:@selector(showSettings:) keyEquivalent:@","];
    preferences.target = self;
    [appMenu addItem:preferences];
    [appMenu addItem:[NSMenuItem separatorItem]];
    NSMenuItem *exit = [[NSMenuItem alloc] initWithTitle:@"退出 Drop" action:@selector(quit:) keyEquivalent:@"q"];
    exit.target = self;
    [appMenu addItem:exit];
    appItem.submenu = appMenu;
    [main addItem:appItem];
    NSMenuItem *editItem = [[NSMenuItem alloc] initWithTitle:@"编辑" action:nil keyEquivalent:@""];
    NSMenu *edit = [[NSMenu alloc] initWithTitle:@"编辑"];
    [edit addItemWithTitle:@"复制" action:@selector(copy:) keyEquivalent:@"c"];
    [edit addItemWithTitle:@"粘贴" action:@selector(paste:) keyEquivalent:@"v"];
    [edit addItemWithTitle:@"全选" action:@selector(selectAll:) keyEquivalent:@"a"];
    editItem.submenu = edit;
    [main addItem:editItem];
    NSMenuItem *conversationItem = [[NSMenuItem alloc] initWithTitle:@"会话" action:nil keyEquivalent:@""];
    NSMenu *conversation = [[NSMenu alloc] initWithTitle:@"会话"];
    for (NSArray *item in @[
             @[ @"同步", NSStringFromSelector(@selector(synchronize:)) ], @[ @"导出所选附件", NSStringFromSelector(@selector(exportAttachment:)) ],
             @[ @"查看完整正文", NSStringFromSelector(@selector(showFullText:)) ], @[ @"删除所选消息", NSStringFromSelector(@selector(deleteMessage:)) ],
             @[ @"恢复后重新提交已复核任务", NSStringFromSelector(@selector(reviewMessages:)) ]
         ]) {
        NSMenuItem *command = [[NSMenuItem alloc] initWithTitle:item[0] action:NSSelectorFromString(item[1]) keyEquivalent:@""];
        command.target = self;
        [conversation addItem:command];
    }
    conversationItem.submenu = conversation;
    [main addItem:conversationItem];
    NSApp.mainMenu = main;
    self.timer = [NSTimer scheduledTimerWithTimeInterval:.2 target:self selector:@selector(poll:) userInfo:nil repeats:YES];
    if (!self.hidden) [self showWindow:nil];
#ifdef DROP_GUI_CHECKS
    gui_probe_start(self);
#endif
    if ([NSBundle mainBundle].bundleIdentifier)
        [[UNUserNotificationCenter currentNotificationCenter] requestAuthorizationWithOptions:UNAuthorizationOptionAlert | UNAuthorizationOptionSound
                                                                            completionHandler:^(BOOL allowed, NSError *error) {
                                                                              (void)allowed;
                                                                              (void)error;
                                                                            }];
}
/* 控件尺寸以逻辑点布局，AppKit 根据屏幕比例绘制；分栏拖动和窗口改变后只更新位置。 */
- (void)layout {
    CGFloat lh = self.left.bounds.size.height, lw = self.left.bounds.size.width, h = self.right.bounds.size.height, w = self.right.bounds.size.width;
    self.find.frame = NSMakeRect(0, 2, MAX(80, lw - 68), 24);
    for (NSView *view in self.left.subviews)
        if ([view.identifier isEqual:@"find-button"]) view.frame = NSMakeRect(lw - 64, 0, 64, 28);
    self.userScroll.frame = NSMakeRect(0, 31, lw, MAX(100, lh - 31));
    CGFloat searchWidth = MAX(140, w - 365);
    self.overviewBand.frame = NSMakeRect(3, 0, w - 3, 26);
    self.overview.frame = NSMakeRect(5, 4, w - 13, 18);
    self.search.frame = NSMakeRect(3, 34, searchWidth, 24);
    self.from.frame = NSMakeRect(searchWidth + 8, 34, 116, 24);
    self.to.frame = NSMakeRect(searchWidth + 128, 34, 116, 24);
    for (NSView *view in self.right.subviews) {
        if ([view.identifier isEqual:@"filter-button"]) view.frame = NSMakeRect(w - 113, 32, 57, 28);
        if ([view.identifier isEqual:@"previous-button"]) view.frame = NSMakeRect(w - 57, 32, 28, 28);
        if ([view.identifier isEqual:@"next-button"]) view.frame = NSMakeRect(w - 29, 32, 28, 28);
        if ([view.identifier isEqual:@"add-button"]) view.frame = NSMakeRect(w - 94, h - 64, 94, 28);
        if ([view.identifier isEqual:@"clear-button"]) view.frame = NSMakeRect(w - 94, h - 100, 94, 28);
    }
    self.messageScroll.frame = NSMakeRect(3, 65, w - 3, MAX(100, h - 327));
    self.draftLabel.frame = NSMakeRect(4, h - 256, (w - 6) * .5, 18);
    self.fileLabel.frame = NSMakeRect((w - 6) * .5 + 6, h - 256, (w - 6) * .5, 18);
    self.draftScroll.frame = NSMakeRect(3, h - 234, (w - 6) * .5, 84);
    self.fileScroll.frame = NSMakeRect((w - 6) * .5 + 6, h - 234, (w - 6) * .5, 84);
    self.recipientBand.frame = NSMakeRect(3, h - 142, w - 3, 26);
    self.recipient.frame = NSMakeRect(5, 4, w - 13, 18);
    self.composeScroll.frame = NSMakeRect(3, h - 108, w - 101, 104);
    self.send.frame = NSMakeRect(w - 94, h - 32, 94, 28);
}
- (void)windowDidResize:(NSNotification *)notification {
    (void)notification;
    [self layout];
}
- (void)splitViewDidResizeSubviews:(NSNotification *)notification {
    (void)notification;
    [self layout];
}
- (CGFloat)splitView:(NSSplitView *)split constrainMinCoordinate:(CGFloat)proposed ofSubviewAt:(NSInteger)index {
    (void)split;
    (void)proposed;
    (void)index;
    return 160;
}
- (CGFloat)splitView:(NSSplitView *)split constrainMaxCoordinate:(CGFloat)proposed ofSubviewAt:(NSInteger)index {
    (void)proposed;
    (void)index;
    return split.bounds.size.width - 550;
}
- (BOOL)windowShouldClose:(NSWindow *)window {
    [window orderOut:nil];
    return NO;
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app {
    (void)app;
    return NO;
}
- (void)showWindow:(id)sender {
    (void)sender;
    [self.window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}
- (void)quit:(id)sender {
    (void)sender;
    [NSApp terminate:nil];
}
- (void)applicationWillTerminate:(NSNotification *)notification {
    (void)notification;
    [self.timer invalidate];
    drop_runtime_stop(self.runtime);
    self.runtime = NULL;
    drop_snapshot_free(self.snapshot);
    self.snapshot = NULL;
    [[NSStatusBar systemStatusBar] removeStatusItem:self.statusItem];
}
- (void)synchronize:(id)sender {
    (void)sender;
    [self submit:new_task(DROP_TASK_SYNC)];
}
- (void)findAccount:(id)sender {
    (void)sender;
    drop_task *task = new_task(DROP_TASK_FIND);
    if (task) task->login = strdup(self.find.stringValue.UTF8String);
    [self submit:task];
}
- (void)viewRequest {
    drop_task *task = new_task(DROP_TASK_VIEW);
    if (!task) return;
    strcpy_s(task->peer, DROP_ID_CAP, self.peer.UTF8String);
    task->page = (int)self.page;
    task->text = strdup(self.search.stringValue.UTF8String);
    NSArray *fields = @[ self.from.stringValue, self.to.stringValue ];
    int64_t times[2] = {0, INT64_MAX};
    NSDateFormatter *formatter = [[NSDateFormatter alloc] init];
    formatter.locale = [[NSLocale alloc] initWithLocaleIdentifier:@"en_US_POSIX"];
    formatter.dateFormat = @"yyyy-MM-dd";
    formatter.lenient = NO;
    for (NSInteger i = 0; i < 2; ++i)
        if ([fields[i] length]) {
            NSDate *date = [formatter dateFromString:fields[i]];
            if (!date || ![[formatter stringFromDate:date] isEqual:fields[i]]) {
                self.status.stringValue = @"日期格式：YYYY-MM-DD。";
                drop_task_free(task);
                return;
            }
            if (i) {
                NSDate *next = [[NSCalendar currentCalendar] dateByAddingUnit:NSCalendarUnitDay value:1 toDate:date options:0];
                times[i] = (int64_t)next.timeIntervalSince1970 - 1;
            } else
                times[i] = (int64_t)date.timeIntervalSince1970;
        }
    task->from = times[0];
    task->to = times[1];
    [self submit:task];
}
- (void)filter:(id)sender {
    (void)sender;
    self.page = 0;
    [self viewRequest];
}
- (void)previous:(id)sender {
    (void)sender;
    if (self.page) --self.page;
    [self viewRequest];
}
- (void)next:(id)sender {
    (void)sender;
    if (self.snapshot && self.snapshot->history && self.snapshot->history->more) {
        ++self.page;
        [self viewRequest];
    }
}
- (NSInteger)numberOfRowsInTableView:(NSTableView *)table {
    if (table == self.draft) return (NSInteger)self.paths.count;
    if (!self.snapshot || !self.snapshot->history) return 0;
    if (table == self.users) return self.snapshot->history->peer_count;
    if (self.selectedRow < 0 || self.selectedRow >= self.snapshot->history->count) return 0;
    drop_detail *detail = self.snapshot->history->rows[self.selectedRow].content;
    return detail ? detail->count : 0;
}
- (NSView *)tableView:(NSTableView *)table viewForTableColumn:(NSTableColumn *)column row:(NSInteger)row {
    NSString *text = @"";
    if (table == self.draft) {
        NSString *path = self.paths[(NSUInteger)row];
        if ([column.identifier isEqual:@"file"]) {
            NSTableCellView *cell = [[NSTableCellView alloc] initWithFrame:NSMakeRect(0, 0, column.width, 24)];
            NSImageView *icon = [[NSImageView alloc] initWithFrame:NSMakeRect(3, 4, 16, 16)];
            icon.image = [[NSWorkspace sharedWorkspace] iconForFile:path];
            NSTextField *name = [self label:path.lastPathComponent];
            name.frame = NSMakeRect(24, 3, MAX(1, column.width - 26), 18);
            name.autoresizingMask = NSViewWidthSizable;
            name.toolTip = path.lastPathComponent;
            [cell addSubview:icon];
            [cell addSubview:name];
            cell.textField = name;
            cell.imageView = icon;
            return cell;
        }
        struct stat st;
        uint64_t size = !lstat(path.fileSystemRepresentation, &st) && st.st_size >= 0 ? (uint64_t)st.st_size : 0;
        text = [NSString stringWithFormat:@"%llu", (unsigned long long)size];
    } else if (table == self.users) {
        drop_peer *peer = &self.snapshot->history->peers[row];
        text = [column.identifier isEqual:@"unread"] ? [NSString stringWithFormat:@"%d", peer->unread] : [native_text(peer->name) stringByAppendingString:row ? @"" : @"（自己）"];
    } else {
        drop_file_view *file = &self.snapshot->history->rows[self.selectedRow].content->files[row];
        if ([column.identifier isEqual:@"file"])
            text = native_text(file->name);
        else if ([column.identifier isEqual:@"size"])
            text = [NSString stringWithFormat:@"%llu", (unsigned long long)file->total];
        else
            text = file->complete ? @"已缓存" : @"待下载";
    }
    return [self label:text];
}
- (void)tableViewSelectionDidChange:(NSNotification *)notification {
    if (notification.object == self.draft) return;
    if (self.rebuilding || !self.snapshot || !self.snapshot->history) return;
    if (notification.object == self.users) {
        NSInteger row = self.users.selectedRow;
        if (row < 0 || row >= self.snapshot->history->peer_count) return;
        self.peer = [NSString stringWithUTF8String:self.snapshot->history->peers[row].id];
        self.page = 0;
        self.send.enabled = NO;
        [self viewRequest];
    } else
        self.selectedFile = self.files.selectedRow;
}
- (void)selectMessage:(NSInteger)row file:(NSInteger)file {
    if (!self.snapshot || !self.snapshot->history || row < 0 || row >= self.snapshot->history->count) return;
    drop_history_row *message = &self.snapshot->history->rows[row];
    self.selectedRow = row;
    self.selectedFile = file;
    self.selected = [NSString stringWithUTF8String:message->id];
    self.detail.string = message->content ? native_text(message->content->text) : @"";
    self.fileLabel.stringValue = [NSString stringWithFormat:@"所选消息的附件（%d）", message->content ? message->content->count : 0];
    [self.files reloadData];
    if (file >= 0 && message->content && file < message->content->count) [self.files selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)file] byExtendingSelection:NO];
    self.deleteButton.enabled = message->deletable && self.snapshot->history->deletion_supported;
    self.exportButton.enabled = message->content && message->content->count;
    if (message->unread && !message->pending && !self.rebuilding) {
        drop_task *task = new_task(DROP_TASK_READ);
        if (task) strcpy_s(task->message, DROP_ID_CAP, message->id);
        [self submit:task];
    }
}
/* 明确打开本机完整正文；不创建重复的主界面正文面板，也不执行附件或重新访问服务器。 */
- (void)showFullText:(id)sender {
    (void)sender;
    if (self.selectedRow < 0) return;
    if (!self.document.visible) [self.document center];
    [self.document makeKeyAndOrderFront:nil];
}
- (void)textViewDidChangeSelection:(NSNotification *)notification {
    if (self.rebuilding || notification.object != self.messages) return;
    NSUInteger offset = self.messages.selectedRange.location;
    for (NSUInteger i = 0; i < self.ranges.count; ++i) {
        if (!NSLocationInRange(offset, self.ranges[i].rangeValue)) continue;
        NSInteger file = -1;
        for (NSUInteger j = 0; j < self.fileRanges[i].count; ++j)
            if (NSLocationInRange(offset, self.fileRanges[i][j].rangeValue)) file = (NSInteger)j;
        [self selectMessage:(NSInteger)i file:file];
        break;
    }
}
- (void)addURLs:(NSArray<NSURL *> *)urls {
    if (self.sendPending) return;
    for (NSURL *url in urls) {
        struct stat st;
        NSString *path = url.path;
        if (!url.isFileURL || self.paths.count >= DROP_MAX_ATTACHMENTS || lstat(path.fileSystemRepresentation, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
            (uint64_t)st.st_size > UINT64_C(10737418240)) {
            self.status.stringValue = @"请选择普通文件，最多 32 个，每个不超过 10 GiB。";
            continue;
        }
        if (![self.paths containsObject:path]) [self.paths addObject:path];
    }
    [self.draft reloadData];
    self.draftLabel.stringValue = [NSString stringWithFormat:@"待发送附件（%lu）", (unsigned long)self.paths.count];
    NSMenu *removeMenu = [[NSMenu alloc] initWithTitle:@"待发送附件"];
    for (NSString *path in self.paths) {
        NSMenuItem *remove = [[NSMenuItem alloc] initWithTitle:[@"移除 " stringByAppendingString:path.lastPathComponent] action:@selector(removeAttachment:) keyEquivalent:@""];
        remove.target = self;
        remove.representedObject = path;
        [removeMenu addItem:remove];
    }
    self.draft.menu = removeMenu;
}
- (void)removeAttachment:(NSMenuItem *)item {
    if (self.sendPending) return;
    [self.paths removeObject:item.representedObject];
    [self addURLs:@[]];
}
- (void)addAttachments:(id)sender {
    (void)sender;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = NO;
    panel.allowsMultipleSelection = YES;
    panel.title = @"添加附件";
    [panel beginSheetModalForWindow:self.window
                  completionHandler:^(NSModalResponse result) {
                    if (result == NSModalResponseOK) [self addURLs:panel.URLs];
                  }];
}
- (void)clearAttachments:(id)sender {
    (void)sender;
    if (self.sendPending) return;
    [self.paths removeAllObjects];
    [self addURLs:@[]];
}
- (void)sendMessage:(id)sender {
    (void)sender;
    if (!self.send.enabled || (!self.compose.string.length && !self.paths.count)) return;
    drop_task *task = new_task(DROP_TASK_SEND);
    if (task) {
        task->text = strdup(self.compose.string.UTF8String);
        strcpy_s(task->peer, DROP_ID_CAP, self.peer.UTF8String);
        task->count = (int)self.paths.count;
        if (self.snapshot && self.snapshot->history) strcpy_s(task->account, DROP_ID_CAP, self.snapshot->history->self);
        for (int i = 0; i < task->count; ++i)
            task->paths[i] = strdup(self.paths[(NSUInteger)i].fileSystemRepresentation);
    }
    if ([self submit:task]) {
        /* 先等待 C 核心持久保存成功，复制或存储失败时保留完整草稿。 */
        self.sendPending = YES;
        self.pendingFinished = self.snapshot ? self.snapshot->send_finished : 0;
        self.pendingSerial = self.snapshot ? self.snapshot->send_serial : 0;
        self.compose.editable = NO;
        self.send.enabled = NO;
    }
}
/* 原生确认框只传递稳定消息身份，刷新历史后不会删除另一行。 */
- (void)deleteMessage:(id)sender {
    (void)sender;
    if (!self.deleteButton.enabled || !self.selected.length) return;
    NSString *message = [self.selected copy];
    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = @"删除所选消息？";
    alert.informativeText = @"删除将同步至服务器及双方账号的所有设备。用户原文件、已导出文件和旧备份保留。";
    [alert addButtonWithTitle:@"取消"];
    [alert addButtonWithTitle:@"删除"];
    [alert beginSheetModalForWindow:self.window
                  completionHandler:^(NSModalResponse result) {
                    if (result == NSAlertSecondButtonReturn) {
                        drop_task *task = new_task(DROP_TASK_DELETE);
                        if (task) strcpy_s(task->message, DROP_ID_CAP, message.UTF8String);
                        [self submit:task];
                    }
                  }];
}
- (void)reviewMessages:(id)sender {
    (void)sender;
    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = @"重新提交已复核的待发送任务？";
    alert.informativeText = @"请先检查服务器恢复后的记录，确认需要重新发送。";
    [alert addButtonWithTitle:@"取消"];
    [alert addButtonWithTitle:@"重新提交"];
    [alert beginSheetModalForWindow:self.window
                  completionHandler:^(NSModalResponse result) {
                    if (result == NSAlertSecondButtonReturn) [self submit:new_task(DROP_TASK_REVIEW)];
                  }];
}
- (BOOL)validateMenuItem:(NSMenuItem *)item {
    if (item.action == @selector(deleteMessage:)) return self.deleteButton.enabled;
    if (item.action == @selector(exportAttachment:)) return self.exportButton.enabled;
    return YES;
}
- (void)exportAttachment:(id)sender {
    (void)sender;
    if (!self.snapshot || !self.snapshot->history || self.selectedRow < 0 || self.selectedRow >= self.snapshot->history->count) return;
    drop_history_row *message = &self.snapshot->history->rows[self.selectedRow];
    if (!message->content || self.selectedFile < 0 || self.selectedFile >= message->content->count || !message->content->files[self.selectedFile].complete) {
        self.status.stringValue = @"请选择已完整保存的附件。";
        return;
    }
    NSString *messageID = [NSString stringWithUTF8String:message->id], *objectID = [NSString stringWithUTF8String:message->content->files[self.selectedFile].id];
    WCHAR safe[260];
    drop_ui_safe_filename(message->content->files[self.selectedFile].name, safe);
    NSSavePanel *panel = [NSSavePanel savePanel];
    panel.title = @"导出所选附件";
    panel.nameFieldStringValue = native_text(safe);
    [panel beginSheetModalForWindow:self.window
                  completionHandler:^(NSModalResponse result) {
                    if (result == NSModalResponseOK) {
                        drop_task *task = new_task(DROP_TASK_EXPORT);
                        if (task) {
                            strcpy_s(task->message, DROP_ID_CAP, messageID.UTF8String);
                            strcpy_s(task->object, DROP_ID_CAP, objectID.UTF8String);
                            task->destination = strdup(panel.URL.path.fileSystemRepresentation);
                        }
                        [self submit:task];
                    }
                  }];
}
- (void)showSettings:(id)sender {
    (void)sender;
    if (!self.settings) {
        self.settings = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 560, 350)
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
        self.settings.title = @"Drop · 配置与账号";
        self.settings.releasedWhenClosed = NO;
        self.settings.contentView = [[DropView alloc] initWithFrame:NSMakeRect(0, 0, 560, 350)];
        NSArray *labels = @[ @"服务器地址（http:// 使用 PAKE；https:// 使用 TLS）", @"账号", @"密码（仅登录时使用）", @"私有 CA 文件（可留空）" ];
        self.serverURL = [self entry:@"https://HOST:PORT"];
        self.login = [self entry:@"账号"];
        self.password = [[NSSecureTextField alloc] initWithFrame:NSZeroRect];
        self.ca = [self entry:@"私有 CA 路径"];
        NSArray *entries = @[ self.serverURL, self.login, self.password, self.ca ];
        for (NSUInteger i = 0; i < 4; ++i) {
            NSTextField *name = [self label:labels[i]];
            name.frame = NSMakeRect(12, 12 + i * 58, 536, 18);
            [self.settings.contentView addSubview:name];
            NSTextField *field = entries[i];
            field.frame = NSMakeRect(12, 33 + i * 58, 536, 24);
            [self.settings.contentView addSubview:field];
        }
        [self button:@"登录并保存" action:@selector(loginAccount:) parent:self.settings.contentView].frame = NSMakeRect(12, 252, 116, 28);
        [self button:@"注销" action:@selector(logoutAccount:) parent:self.settings.contentView].frame = NSMakeRect(136, 252, 76, 28);
        self.notify = [NSButton checkboxWithTitle:@"系统通知" target:nil action:nil];
        self.notify.frame = NSMakeRect(12, 290, 130, 24);
        self.attention = [NSButton checkboxWithTitle:@"菜单栏未读提示" target:nil action:nil];
        self.attention.frame = NSMakeRect(146, 290, 170, 24);
        [self.settings.contentView addSubview:self.notify];
        [self.settings.contentView addSubview:self.attention];
        [self button:@"保存提醒设置" action:@selector(savePreferences:) parent:self.settings.contentView].frame = NSMakeRect(332, 287, 156, 28);
        [self.settings center];
    }
    if (self.snapshot && !self.settings.visible) {
        self.serverURL.stringValue = [NSString stringWithUTF8String:self.snapshot->url];
        self.login.stringValue = [NSString stringWithUTF8String:self.snapshot->login];
        self.ca.stringValue = [NSString stringWithUTF8String:self.snapshot->ca];
        self.notify.state = self.snapshot->notify ? NSControlStateValueOn : NSControlStateValueOff;
        self.attention.state = self.snapshot->attention ? NSControlStateValueOn : NSControlStateValueOff;
    }
    [self.settings makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}
- (void)loginAccount:(id)sender {
    (void)sender;
    drop_task *task = new_task(DROP_TASK_LOGIN);
    if (task) {
        task->url = strdup(self.serverURL.stringValue.UTF8String);
        task->login = strdup(self.login.stringValue.UTF8String);
        task->password = strdup(self.password.stringValue.UTF8String);
        task->ca = strdup(self.ca.stringValue.UTF8String);
    }
    self.password.stringValue = @"";
    [self submit:task];
}
- (void)savePreferences:(id)sender {
    (void)sender;
    drop_task *task = new_task(DROP_TASK_PREFERENCES);
    if (task) {
        task->notify = self.notify.state == NSControlStateValueOn;
        task->attention = self.attention.state == NSControlStateValueOn;
    }
    [self submit:task];
}
- (void)logoutAccount:(id)sender {
    (void)sender;
    NSAlert *alert = [[NSAlert alloc] init];
    alert.messageText = @"注销当前账号？";
    alert.informativeText = @"清除本机登录凭据，历史正文和附件保留。";
    [alert addButtonWithTitle:@"取消"];
    [alert addButtonWithTitle:@"注销"];
    [alert beginSheetModalForWindow:self.window
                  completionHandler:^(NSModalResponse result) {
                    if (result == NSAlertSecondButtonReturn) [self submit:new_task(DROP_TASK_LOGOUT)];
                  }];
}
- (void)poll:(NSTimer *)timer {
    (void)timer;
    if (drop_process_stop_requested()) {
        [self quit:nil];
        return;
    }
    drop_snapshot *snapshot = drop_runtime_take(self.runtime);
    if (!snapshot) return;
    if (self.sendPending && snapshot->send_finished > self.pendingFinished) {
        self.sendPending = NO;
        self.compose.editable = YES;
        if (snapshot->send_serial > self.pendingSerial) {
            self.compose.string = @"";
            [self clearAttachments:nil];
        }
    }
    drop_snapshot_free(self.snapshot);
    self.snapshot = snapshot;
    self.rebuilding = YES;
    [self.users reloadData];
    NSMutableAttributedString *text = [[NSMutableAttributedString alloc] initWithString:@""];
    [self.ranges removeAllObjects];
    [self.fileRanges removeAllObjects];
    self.selectedRow = -1;
    drop_history *history = snapshot->history;
    if (history) {
        self.peer = [NSString stringWithUTF8String:history->peer];
        for (int i = 0; i < history->peer_count; ++i)
            if (!strcmp(history->peers[i].id, history->peer)) {
                [self.users selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)i] byExtendingSelection:NO];
                self.recipient.stringValue =
                    [NSString stringWithFormat:@"发送给：%@%@", native_text(history->peers[i].name), !strcmp(history->peer, history->self) ? @" · 自己的设备" : @""];
                self.overview.stringValue = [NSString stringWithFormat:@"%@ · 第 %ld 页", native_text(history->peers[i].name), (long)self.page + 1];
            }
        NSMutableParagraphStyle *paragraph = [[NSMutableParagraphStyle alloc] init];
        paragraph.paragraphSpacing = 0;
        paragraph.lineSpacing = 0;
        NSDictionary *normal =
            @{NSFontAttributeName : [NSFont systemFontOfSize:13], NSForegroundColorAttributeName : [NSColor textColor], NSParagraphStyleAttributeName : paragraph};
        NSDictionary *header = @{
            NSFontAttributeName : [NSFont boldSystemFontOfSize:13],
            NSForegroundColorAttributeName : [NSColor colorWithRed:.19 green:.35 blue:.53 alpha:1],
            NSParagraphStyleAttributeName : paragraph
        };
        for (int i = 0; i < history->count; ++i) {
            drop_history_row *row = &history->rows[i];
            NSUInteger start = text.length;
            NSString *heading = [NSString stringWithFormat:@"%@  %@ · %@\n", native_text(row->sender), native_text(row->time), row->unread ? @"未读" : @"本机已保存"];
            [text appendAttributedString:[[NSAttributedString alloc] initWithString:heading attributes:header]];
            NSString *body = native_text(row->preview);
            if (body.length) [text appendAttributedString:[[NSAttributedString alloc] initWithString:[body stringByAppendingString:@"\n"] attributes:normal]];
            NSMutableArray *fileRanges = [NSMutableArray array];
            if (row->content)
                for (int j = 0; j < row->content->count; ++j) {
                    drop_file_view *file = &row->content->files[j];
                    NSUInteger fileStart = text.length;
                    NSString *line = [NSString stringWithFormat:@"    附件：%@ · %llu 字节 · %@\n", native_text(file->name), (unsigned long long)file->total,
                        file->complete ? @"已缓存到本机" : @"待下载"];
                    [text appendAttributedString:[[NSAttributedString alloc] initWithString:line attributes:header]];
                    [fileRanges addObject:[NSValue valueWithRange:NSMakeRange(fileStart, text.length - fileStart)]];
                }
            [self.fileRanges addObject:fileRanges];
            [self.ranges addObject:[NSValue valueWithRange:NSMakeRange(start, text.length - start)]];
            if ([self.selected isEqual:[NSString stringWithUTF8String:row->id]]) self.selectedRow = i;
        }
    }
    NSRange selection = self.messages.selectedRange;
    NSPoint scroll = self.messageScroll.contentView.bounds.origin;
    [self.messages.textStorage setAttributedString:text];
    if (NSMaxRange(selection) <= text.length) self.messages.selectedRange = selection;
    [self.messageScroll.contentView scrollToPoint:scroll];
    [self.messageScroll reflectScrolledClipView:self.messageScroll.contentView];
    self.deleteButton.enabled = NO;
    self.exportButton.enabled = NO;
    if (self.selectedRow >= 0)
        [self selectMessage:self.selectedRow file:self.selectedFile];
    else {
        self.detail.string = @"";
        [self.files reloadData];
    }
    self.send.enabled = snapshot->ready && self.peer.length == 32 && !self.sendPending;
    self.status.stringValue = [NSString stringWithFormat:@"%@ · 待发送 %d · 下载 %d · 待删除 %d", [NSString stringWithUTF8String:snapshot->status],
        history ? history->pending_count : 0, history ? history->downloads : 0, history ? history->pending_deletes : 0];
    self.statusItem.button.title = snapshot->attention && history && history->unread ? [NSString stringWithFormat:@"Drop %d", history->unread] : @"Drop";
    if (snapshot->attention && history && history->new_notifications && self.window.visible) [NSApp requestUserAttention:NSInformationalRequest];
    if (snapshot->notify && history && history->new_notifications && !self.window.visible && [NSBundle mainBundle].bundleIdentifier) {
        UNMutableNotificationContent *content = [[UNMutableNotificationContent alloc] init];
        content.title = @"Drop 收到新消息";
        content.body = @"消息和附件正在同步到本机。";
        [[UNUserNotificationCenter currentNotificationCenter] addNotificationRequest:[UNNotificationRequest requestWithIdentifier:@"drop-new-message" content:content trigger:nil]
                                                               withCompletionHandler:nil];
    }
    self.rebuilding = NO;
}
@end

#ifdef DROP_GUI_CHECKS
#include "../checks/macos_probe.inc"
#endif
int drop_native_ui(const WCHAR *root, int hidden) {
    @autoreleasepool {
        /* NSApplication.delegate 不拥有控制器，必须保持强引用跨越整个事件循环。 */
        __attribute__((objc_precise_lifetime)) DropController *controller = [[DropController alloc] init];
        controller.runtime = drop_runtime_start(root);
        controller.hidden = hidden;
        if (!controller.runtime) {
            fprintf(stderr, "Drop data directory unavailable or already running.\n");
            return 1;
        }
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSApp.delegate = controller;
        /* 命令及 LaunchAgent 启动不依赖 LaunchServices 的启动事件，显式完成 AppKit 初始化。 */
        [NSApp finishLaunching];
        [NSApp run];
    }
    return 0;
}
