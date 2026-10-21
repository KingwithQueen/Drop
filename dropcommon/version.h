#ifndef DROP_VERSION_H
#define DROP_VERSION_H

/* 程序显示、Windows 字符串资源和打包脚本共用三段产品版本，禁止单独增加显示用迭代段。 */
#define DROP_VERSION "1.0.4"
/* Windows 固定数值资源必须使用四个 WORD；末尾零仅满足系统格式，不作为产品迭代编号。 */
#define DROP_VERSION_RESOURCE 1, 0, 4, 0
#define DROP_VERSION_RESOURCE_STRING DROP_VERSION
#define DROP_VERSION_WIDEN_INNER(value) L##value
#define DROP_VERSION_WIDEN(value) DROP_VERSION_WIDEN_INNER(value)
#define DROP_VERSION_WIDE DROP_VERSION_WIDEN(DROP_VERSION)

#endif
