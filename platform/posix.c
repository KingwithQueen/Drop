/* 现有同步核心的 POSIX 文件适配，采用 O_NOFOLLOW、flock 和刷新后改名，不模拟其他 Win32 子系统。 */
#include "drop_common.h"
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <fnmatch.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <locale.h>
#ifdef __APPLE__
#include <xlocale.h>
#endif

struct drop_posix_handle {
    int kind, fd;
    struct stat initial;
    DIR *directory;
    char pattern[1025];
    atomic_int signaled;
};

int strcpy_s(char *out, size_t capacity, const char *text) {
    if (!out || !capacity) return EINVAL;
    if (!text || strlen(text) >= capacity) {
        out[0] = 0;
        return EINVAL;
    }
    memcpy(out, text, strlen(text) + 1);
    return 0;
}
int wcscpy_s(WCHAR *out, size_t capacity, const WCHAR *text) {
    if (!out || !capacity) return EINVAL;
    if (!text || wcslen(text) >= capacity) {
        out[0] = 0;
        return EINVAL;
    }
    wmemcpy(out, text, wcslen(text) + 1);
    return 0;
}
int wcscat_s(WCHAR *out, size_t capacity, const WCHAR *text) {
    size_t used = wcsnlen(out, capacity);
    return used == capacity ? EINVAL : wcscpy_s(out + used, capacity - used, text);
}
int wcsncpy_s(WCHAR *out, size_t capacity, const WCHAR *text, size_t limit) {
    size_t length;
    if (!out || !text || !capacity) return EINVAL;
    length = wcslen(text);
    if (limit != _TRUNCATE && length > limit) length = limit;
    if (length >= capacity) {
        if (limit != _TRUNCATE) {
            out[0] = 0;
            return EINVAL;
        }
        length = capacity - 1;
    }
    wmemcpy(out, text, length);
    out[length] = 0;
    return 0;
}
int sprintf_s(char *out, size_t capacity, const char *format, ...) {
    int count;
    va_list args;
    va_start(args, format);
    count = vsnprintf(out, capacity, format, args);
    va_end(args);
    if (count < 0 || (size_t)count >= capacity) {
        if (capacity) out[0] = 0;
        return -1;
    }
    return count;
}
/* Windows 宽格式中 %s 表示宽字符串，%hs 表示 UTF-8；只转换格式标记，保持业务字符串原样。 */
int swprintf_s(WCHAR *out, size_t capacity, const WCHAR *format, ...) {
    WCHAR converted[2048];
    size_t i = 0;
    int count;
    va_list args;
    locale_t locale, previous;
    while (*format && i + 4 < 2048) {
        converted[i++] = *format;
        if (*format++ != L'%') continue;
        if (*format == L'%') {
            converted[i++] = *format++;
            continue;
        }
        while (*format && wcschr(L"-+ #0.123456789*", *format) && i + 4 < 2048)
            converted[i++] = *format++;
        if (*format == L's')
            converted[i++] = L'l';
        else if (*format == L'h' && format[1] == L's')
            ++format;
        if (*format) converted[i++] = *format++;
    }
    if (*format) return -1;
    converted[i] = 0;
    /* Darwin 的宽格式化也经过 locale 转码；使用线程局部 UTF-8，不能改变其他线程的进程设置。 */
#ifdef __APPLE__
    locale = newlocale(LC_CTYPE_MASK, "en_US.UTF-8", NULL);
#else
    locale = newlocale(LC_CTYPE_MASK, "C.UTF-8", NULL);
#endif
    if (!locale) return -1;
    previous = uselocale(locale);
    va_start(args, format);
    count = vswprintf(out, capacity, converted, args);
    va_end(args);
    uselocale(previous);
    freelocale(locale);
    if (count < 0 || (size_t)count >= capacity) {
        if (capacity) out[0] = 0;
        return -1;
    }
    return count;
}
int drop_posix_path(const WCHAR *path, char *utf8, size_t capacity) {
    if (!path || !*path || !drop_to_utf8(path, utf8, capacity)) {
        errno = EINVAL;
        return 0;
    }
    return 1;
}
/* 按目录描述符逐层检查父路径，拒绝任一中间符号链接，防止受管理文件越过数据边界。 */
int drop_posix_parent_safe(const char *path) {
    char copy[PATH_MAX], *part, *next, *last;
    int directory, ok = 0;
    if (strcpy_s(copy, sizeof(copy), path) || !(last = strrchr(copy, '/'))) {
        errno = EINVAL;
        return 0;
    }
    *last = 0;
    directory = open(path[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return 0;
    part = strtok_r(copy, "/", &next);
    while (part) {
        int child;
        if (!strcmp(part, "..")) {
            errno = EINVAL;
            goto done;
        }
        child = openat(directory, part, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (child < 0) goto done;
        close(directory);
        directory = child;
        part = strtok_r(NULL, "/", &next);
    }
    ok = 1;
done:
    close(directory);
    return ok;
}
HANDLE CreateFileW(const WCHAR *path, DWORD access, DWORD sharing, void *security, DWORD creation, DWORD attributes, HANDLE template_file) {
    char utf8[PATH_MAX];
    struct drop_posix_handle *h = NULL;
    int flags, fd;
    (void)security;
    (void)template_file;
    if (!drop_posix_path(path, utf8, sizeof(utf8)) || !drop_posix_parent_safe(utf8)) return INVALID_HANDLE_VALUE;
    flags = access & GENERIC_WRITE ? (access & GENERIC_READ ? O_RDWR : O_WRONLY) : O_RDONLY;
    flags |= O_CLOEXEC | O_NOFOLLOW;
    if (attributes & FILE_FLAG_BACKUP_SEMANTICS) flags |= O_DIRECTORY;
    if (creation == CREATE_NEW)
        flags |= O_CREAT | O_EXCL;
    else if (creation == OPEN_ALWAYS)
        flags |= O_CREAT;
    else if (creation != OPEN_EXISTING) {
        errno = EINVAL;
        return INVALID_HANDLE_VALUE;
    }
    fd = open(utf8, flags, 0600);
    if (fd < 0) return INVALID_HANDLE_VALUE;
    if ((!sharing && flock(fd, LOCK_EX | LOCK_NB)) || !(h = calloc(1, sizeof(*h))) || fstat(fd, &h->initial) ||
        (!(attributes & FILE_FLAG_BACKUP_SEMANTICS) && !S_ISREG(h->initial.st_mode))) {
        close(fd);
        free(h);
        return INVALID_HANDLE_VALUE;
    }
    h->fd = fd;
    return h;
}
int CloseHandle(HANDLE h) {
    int ok = 1;
    if (!h || h == INVALID_HANDLE_VALUE) return 0;
    if (h->kind == 0)
        ok = close(h->fd) == 0;
    else if (h->kind == 1)
        ok = closedir(h->directory) == 0;
    free(h);
    return ok;
}
int ReadFile(HANDLE h, void *data, DWORD capacity, DWORD *count, void *overlapped) {
    ssize_t n;
    (void)overlapped;
    *count = 0;
    do {
        n = read(h->fd, data, capacity);
    } while (n < 0 && errno == EINTR);
    if (n < 0) return 0;
    *count = (DWORD)n;
    return 1;
}
int WriteFile(HANDLE h, const void *data, DWORD length, DWORD *count, void *overlapped) {
    (void)overlapped;
    *count = 0;
    while (*count < length) {
        ssize_t n = write(h->fd, (const char *)data + *count, length - *count);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return 0;
        *count += (DWORD)n;
    }
    return 1;
}
int FlushFileBuffers(HANDLE h) {
    return fsync(h->fd) == 0;
}
int GetFileSizeEx(HANDLE h, LARGE_INTEGER *length) {
    struct stat st;
    if (fstat(h->fd, &st)) return 0;
    length->QuadPart = st.st_size;
    return 1;
}
int SetFilePointerEx(HANDLE h, LARGE_INTEGER offset, LARGE_INTEGER *result, DWORD origin) {
    off_t position;
    if (origin != FILE_BEGIN || offset.QuadPart < 0) return 0;
    position = lseek(h->fd, (off_t)offset.QuadPart, SEEK_SET);
    if (position < 0) return 0;
    if (result) result->QuadPart = position;
    return 1;
}
int SetEndOfFile(HANDLE h) {
    off_t position = lseek(h->fd, 0, SEEK_CUR);
    return position >= 0 && ftruncate(h->fd, position) == 0;
}
DWORD GetFileAttributesW(const WCHAR *path) {
    char utf8[PATH_MAX];
    struct stat st;
    if (!drop_posix_path(path, utf8, sizeof(utf8)) || lstat(utf8, &st)) return INVALID_FILE_ATTRIBUTES;
    return S_ISLNK(st.st_mode) ? FILE_ATTRIBUTE_REPARSE_POINT : S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0;
}
DWORD GetLastError(void) {
    return (DWORD)errno;
}
/* 绝对路径按词法消除点段；随后打开或创建目录时仍逐层拒绝符号链接。 */
DWORD GetFullPathNameW(const WCHAR *path, DWORD capacity, WCHAR *output, WCHAR **filepart) {
    char input[PATH_MAX], absolute[PATH_MAX], normalized[PATH_MAX] = "/", cwd[PATH_MAX], *part, *next;
    size_t length = 1;
    if (!drop_posix_path(path, input, sizeof(input))) return 0;
    if (input[0] == '/') {
        if (strcpy_s(absolute, sizeof(absolute), input)) return 0;
    } else if (!getcwd(cwd, sizeof(cwd)) || sprintf_s(absolute, sizeof(absolute), "%s/%s", cwd, input) < 0)
        return 0;
    for (part = strtok_r(absolute, "/", &next); part; part = strtok_r(NULL, "/", &next)) {
        if (!strcmp(part, ".")) continue;
        if (!strcmp(part, "..")) {
            while (length > 1 && normalized[--length] != '/') {
            }
            normalized[length] = 0;
            continue;
        }
        if (length > 1) normalized[length++] = '/';
        if (strlen(part) >= sizeof(normalized) - length) return 0;
        strcpy(normalized + length, part);
        length += strlen(part);
    }
    if (!drop_to_wide(normalized, output, capacity)) return 0;
    if (filepart) *filepart = wcsrchr(output, L'/') + 1;
    return (DWORD)wcslen(output);
}
DWORD GetFinalPathNameByHandleW(HANDLE h, WCHAR *output, DWORD capacity, DWORD flags) {
    char path[PATH_MAX];
    (void)flags;
#ifdef __APPLE__
    if (fcntl(h->fd, F_GETPATH, path)) return 0;
#else
    char link[64];
    ssize_t n;
    if (sprintf_s(link, sizeof(link), "/proc/self/fd/%d", h->fd) < 0 || (n = readlink(link, path, sizeof(path) - 1)) <= 0) return 0;
    path[n] = 0;
#endif
    return drop_to_wide(path, output, capacity) ? (DWORD)wcslen(output) : 0;
}
int DeleteFileW(const WCHAR *path) {
    char utf8[PATH_MAX];
    struct stat st;
    return drop_posix_path(path, utf8, sizeof(utf8)) && drop_posix_parent_safe(utf8) && !lstat(utf8, &st) && S_ISREG(st.st_mode) && unlink(utf8) == 0;
}
/* 改名后刷新父目录，非替换提交用 link 的原子排他性避免先检查后覆盖。 */
int MoveFileExW(const WCHAR *source, const WCHAR *destination, DWORD flags) {
    char from[PATH_MAX], to[PATH_MAX], *slash;
    struct stat st;
    int directory, ok;
    if (!drop_posix_path(source, from, sizeof(from)) || !drop_posix_path(destination, to, sizeof(to)) || !drop_posix_parent_safe(from) || !drop_posix_parent_safe(to) ||
        lstat(from, &st) || !S_ISREG(st.st_mode))
        return 0;
    if (flags & MOVEFILE_REPLACE_EXISTING)
        ok = rename(from, to) == 0;
    else
        ok = link(from, to) == 0 && unlink(from) == 0;
    if (!ok || !(flags & MOVEFILE_WRITE_THROUGH)) return ok;
    slash = strrchr(to, '/');
    if (slash == to)
        slash[1] = 0;
    else
        *slash = 0;
    directory = open(to, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (directory < 0) return 0;
    ok = fsync(directory) == 0;
    close(directory);
    return ok;
}
int drop_posix_file_unchanged(HANDLE h) {
    struct stat st;
    if (fstat(h->fd, &st) || st.st_size != h->initial.st_size) return 0;
#ifdef __APPLE__
    return st.st_mtimespec.tv_sec == h->initial.st_mtimespec.tv_sec && st.st_mtimespec.tv_nsec == h->initial.st_mtimespec.tv_nsec &&
        st.st_ctimespec.tv_sec == h->initial.st_ctimespec.tv_sec && st.st_ctimespec.tv_nsec == h->initial.st_ctimespec.tv_nsec;
#else
    return st.st_mtim.tv_sec == h->initial.st_mtim.tv_sec && st.st_mtim.tv_nsec == h->initial.st_mtim.tv_nsec && st.st_ctim.tv_sec == h->initial.st_ctim.tv_sec &&
        st.st_ctim.tv_nsec == h->initial.st_ctim.tv_nsec;
#endif
}
/* 复制生成独立副本并检查源文件复制期间未变化；失败删除本次新建的目标。 */
int CopyFileW(const WCHAR *source, const WCHAR *destination, int fail_if_exists) {
    HANDLE in = CreateFileW(source, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL), out = INVALID_HANDLE_VALUE;
    unsigned char data[65536];
    DWORD n, written;
    int ok = 0;
    if (!fail_if_exists || in == INVALID_HANDLE_VALUE) goto done;
    out = CreateFileW(destination, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    if (out == INVALID_HANDLE_VALUE) goto done;
    while (ReadFile(in, data, sizeof(data), &n, NULL)) {
        if (!n) {
            ok = drop_posix_file_unchanged(in) && FlushFileBuffers(out);
            break;
        }
        if (!WriteFile(out, data, n, &written, NULL) || written != n) break;
    }
done:
    if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
    if (out != INVALID_HANDLE_VALUE) {
        CloseHandle(out);
        if (!ok) DeleteFileW(destination);
    }
    return ok;
}
int FindNextFileW(HANDLE h, WIN32_FIND_DATAW *item) {
    struct dirent *entry;
    struct stat st;
    while ((entry = readdir(h->directory))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..") || fnmatch(h->pattern, entry->d_name, 0)) continue;
        if (fstatat(dirfd(h->directory), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) || !drop_to_wide(entry->d_name, item->cFileName, 1025)) return 0;
        item->dwFileAttributes = S_ISLNK(st.st_mode) ? FILE_ATTRIBUTE_REPARSE_POINT : S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0;
        return 1;
    }
    errno = ENOENT;
    return 0;
}
HANDLE FindFirstFileW(const WCHAR *pattern, WIN32_FIND_DATAW *item) {
    char utf8[PATH_MAX], *slash;
    HANDLE h;
    int fd;
    if (!drop_posix_path(pattern, utf8, sizeof(utf8)) || !drop_posix_parent_safe(utf8) || !(slash = strrchr(utf8, '/')) || !(h = calloc(1, sizeof(*h))))
        return INVALID_HANDLE_VALUE;
    h->kind = 1;
    if (strcpy_s(h->pattern, sizeof(h->pattern), slash + 1)) {
        free(h);
        return INVALID_HANDLE_VALUE;
    }
    *slash = 0;
    fd = open(utf8, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 || !(h->directory = fdopendir(fd))) {
        if (fd >= 0) close(fd);
        free(h);
        return INVALID_HANDLE_VALUE;
    }
    if (FindNextFileW(h, item)) return h;
    CloseHandle(h);
    return INVALID_HANDLE_VALUE;
}
int FindClose(HANDLE h) {
    return CloseHandle(h);
}
uint64_t GetTickCount64(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}
HANDLE drop_posix_event_create(void) {
    HANDLE h = calloc(1, sizeof(*h));
    if (h) {
        h->kind = 2;
        atomic_init(&h->signaled, 0);
    }
    return h;
}
void drop_posix_event_set(HANDLE h) {
    atomic_store(&h->signaled, 1);
}
void drop_posix_event_reset(HANDLE h) {
    atomic_store(&h->signaled, 0);
}
DWORD WaitForSingleObject(HANDLE h, DWORD milliseconds) {
    uint64_t until = GetTickCount64() + milliseconds;
    if (!h || h == INVALID_HANDLE_VALUE || h->kind != 2) return ERROR_INVALID_DATA;
    for (;;) {
        struct timespec delay = {0, 20000000};
        if (atomic_load(&h->signaled)) return WAIT_OBJECT_0;
        if (GetTickCount64() >= until) return WAIT_TIMEOUT;
        nanosleep(&delay, NULL);
    }
}
