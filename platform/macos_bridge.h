#ifndef DROP_MACOS_BRIDGE_H
#define DROP_MACOS_BRIDGE_H

/* AppKit 的公开运行时边界：只使用 C17 声明，业务及界面文件均由 C 编译器处理。
 * 每次消息调用显式给出返回值和实参类型，不能用可变参数函数指针代替真实 ABI；尤其 CGFloat、BOOL 和结构体必须保持原始类型。
 * 对象遵循 Cocoa 的拥有规则，跨事件循环保存的引用由界面状态显式 retain/release，临时对象归当前自动释放池。 */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <Block.h>
#include <float.h>
#include <stdbool.h>
#include <stddef.h>

typedef long mac_integer;
typedef unsigned long mac_unsigned;
typedef struct mac_range {
    mac_unsigned location, length;
} mac_range;

/* 来自公开 AppKit/Foundation 枚举的稳定数值；保持名称语义，避免在控件调用处散布无说明的数值。 */
enum {
    MAC_WINDOW_TITLED = 1,
    MAC_WINDOW_CLOSABLE = 2,
    MAC_WINDOW_MINIATURIZABLE = 4,
    MAC_WINDOW_RESIZABLE = 8,
    MAC_BACKING_BUFFERED = 2,
    MAC_VIEW_MIN_X = 1,
    MAC_VIEW_WIDTH = 2,
    MAC_VIEW_MIN_Y = 8,
    MAC_VIEW_HEIGHT = 16,
    MAC_LINE_TRUNCATE_TAIL = 4,
    MAC_BEZEL_ROUNDED = 1,
    MAC_BORDER_BEZEL = 2,
    MAC_SPLIT_THIN = 2,
    MAC_CALENDAR_DAY = 16,
    MAC_ATTENTION_INFORMATIONAL = 10,
    MAC_MODAL_OK = 1,
    MAC_ALERT_SECOND_BUTTON = 1001,
    MAC_NOTIFICATION_SOUND = 2,
    MAC_NOTIFICATION_ALERT = 4
};

#define MAC_CLASS(name) ((id)objc_getClass(name))
#define MAC_SEL(name) sel_registerName(name)
#define MAC_SEND0(result, object, name) ((result (*)(id, SEL))objc_msgSend)((id)(object), MAC_SEL(name))
#define MAC_SEND1(result, object, name, type1, arg1) ((result (*)(id, SEL, type1))objc_msgSend)((id)(object), MAC_SEL(name), (arg1))
#define MAC_SEND2(result, object, name, type1, arg1, type2, arg2) ((result (*)(id, SEL, type1, type2))objc_msgSend)((id)(object), MAC_SEL(name), (arg1), (arg2))
#define MAC_SEND3(result, object, name, type1, arg1, type2, arg2, type3, arg3)                                                                                                     \
    ((result (*)(id, SEL, type1, type2, type3))objc_msgSend)((id)(object), MAC_SEL(name), (arg1), (arg2), (arg3))
#define MAC_SEND4(result, object, name, type1, arg1, type2, arg2, type3, arg3, type4, arg4)                                                                                        \
    ((result (*)(id, SEL, type1, type2, type3, type4))objc_msgSend)((id)(object), MAC_SEL(name), (arg1), (arg2), (arg3), (arg4))
#define MAC_SEND5(result, object, name, type1, arg1, type2, arg2, type3, arg3, type4, arg4, type5, arg5)                                                                           \
    ((result (*)(id, SEL, type1, type2, type3, type4, type5))objc_msgSend)((id)(object), MAC_SEL(name), (arg1), (arg2), (arg3), (arg4), (arg5))

static inline id mac_get(id object, const char *name) {
    return MAC_SEND0(id, object, name);
}
static inline void mac_put(id object, const char *name, id value) {
    MAC_SEND1(void, object, name, id, value);
}
static inline void mac_flag(id object, const char *name, BOOL value) {
    MAC_SEND1(void, object, name, BOOL, value);
}
static inline BOOL mac_bool(id object, const char *name) {
    return MAC_SEND0(BOOL, object, name);
}
static inline mac_integer mac_int(id object, const char *name) {
    return MAC_SEND0(mac_integer, object, name);
}
static inline void mac_number(id object, const char *name, mac_integer value) {
    MAC_SEND1(void, object, name, mac_integer, value);
}
static inline void mac_float(id object, const char *name, CGFloat value) {
    MAC_SEND1(void, object, name, CGFloat, value);
}
static inline void mac_void(id object, const char *name) {
    MAC_SEND0(void, object, name);
}
static inline id mac_retain(id object) {
    return mac_get(object, "retain");
}
static inline void mac_release(id object) {
    mac_void(object, "release");
}
static inline id mac_autorelease(id object) {
    return mac_get(object, "autorelease");
}
/* 先持有新值再释放旧值，允许值与原对象相同；仅用于界面线程拥有的状态槽位。 */
static inline void mac_owned(id *slot, id value) {
    id old = *slot;
    *slot = mac_retain(value);
    mac_release(old);
}
static inline id mac_alloc(const char *name) {
    return mac_get(MAC_CLASS(name), "alloc");
}
static inline id mac_new(const char *name) {
    return mac_autorelease(mac_get(mac_alloc(name), "init"));
}
static inline id mac_text(const char *utf8) {
    return MAC_SEND1(id, MAC_CLASS("NSString"), "stringWithUTF8String:", const char *, utf8 ? utf8 : "");
}
static inline const char *mac_utf8(id text) {
    const char *value = MAC_SEND0(const char *, text, "UTF8String");
    return value ? value : "";
}
static inline mac_unsigned mac_count(id array) {
    return MAC_SEND0(mac_unsigned, array, "count");
}
static inline id mac_at(id array, mac_unsigned index) {
    return MAC_SEND1(id, array, "objectAtIndex:", mac_unsigned, index);
}
static inline BOOL mac_equal(id a, id b) {
    return MAC_SEND1(BOOL, a, "isEqual:", id, b);
}
static inline void mac_add(id array, id value) {
    mac_put(array, "addObject:", value);
}
static inline id mac_array(mac_unsigned count, const id *values) {
    return MAC_SEND2(id, MAC_CLASS("NSArray"), "arrayWithObjects:count:", const id *, values, mac_unsigned, count);
}
static inline id mac_dictionary(mac_unsigned count, const id *keys, const id *values) {
    return MAC_SEND3(id, MAC_CLASS("NSDictionary"), "dictionaryWithObjects:forKeys:count:", const id *, values, const id *, keys, mac_unsigned, count);
}
static inline void mac_frame(id view, CGFloat x, CGFloat y, CGFloat width, CGFloat height) {
    MAC_SEND1(void, view, "setFrame:", CGRect, CGRectMake(x, y, width, height));
}
/* Intel 的 32 字节矩形走 stret；Apple Silicon 使用统一的结构体返回约定。禁止将两种函数指针混用。 */
static inline CGRect mac_rect(id object, const char *name) {
    if (!object) return CGRectZero;
#if defined(__x86_64__)
    CGRect value;
    ((void (*)(CGRect *, id, SEL))objc_msgSend_stret)(&value, object, MAC_SEL(name));
    return value;
#else
    return MAC_SEND0(CGRect, object, name);
#endif
}
static inline id mac_view(const char *name, CGRect frame) {
    return mac_autorelease(MAC_SEND1(id, mac_alloc(name), "initWithFrame:", CGRect, frame));
}
static inline mac_range mac_make_range(mac_unsigned location, mac_unsigned length) {
    return (mac_range){location, length};
}
static inline id mac_index(mac_unsigned index) {
    return MAC_SEND1(id, MAC_CLASS("NSIndexSet"), "indexSetWithIndex:", mac_unsigned, index);
}

/* 这些字符串由 AppKit 导出，保留系统原符号，避免自行猜测属性键名。 */
extern id const NSFontAttributeName;
extern id const NSForegroundColorAttributeName;
extern id const NSParagraphStyleAttributeName;
extern id const NSPasteboardURLReadingFileURLsOnlyKey;
extern id const NSPasteboardTypeFileURL;
extern id const NSImageNameActionTemplate;
extern void NSRectFill(CGRect rect);

#endif
