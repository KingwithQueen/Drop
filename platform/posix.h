#ifndef DROP_POSIX_H
#define DROP_POSIX_H
/* 仅为现有 C 同步核心提供文件、锁和有界运行库适配；图形及网络代码使用各平台原生接口。 */
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <string.h>

typedef wchar_t WCHAR;
typedef uint32_t DWORD;
typedef uint16_t INTERNET_PORT;
typedef uint64_t ULONGLONG;
typedef int64_t LONGLONG;
typedef void *HINSTANCE;
typedef struct drop_posix_handle *HANDLE;
typedef pthread_mutex_t SRWLOCK;
typedef struct {
    int64_t QuadPart;
} LARGE_INTEGER;
typedef struct {
    DWORD dwFileAttributes;
    WCHAR cFileName[1025];
} WIN32_FIND_DATAW;
typedef enum { WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE } WINHTTP_WEB_SOCKET_BUFFER_TYPE;

#define SRWLOCK_INIT PTHREAD_MUTEX_INITIALIZER
#define AcquireSRWLockExclusive(lock) ((void)pthread_mutex_lock(lock))
#define ReleaseSRWLockExclusive(lock) ((void)pthread_mutex_unlock(lock))
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES UINT32_MAX
#define GENERIC_READ 1u
#define GENERIC_WRITE 2u
#define FILE_READ_ATTRIBUTES 4u
#define FILE_SHARE_READ 1u
#define FILE_SHARE_WRITE 2u
#define FILE_SHARE_DELETE 4u
#define CREATE_NEW 1u
#define OPEN_EXISTING 2u
#define OPEN_ALWAYS 3u
#define FILE_ATTRIBUTE_NORMAL 0u
#define FILE_ATTRIBUTE_DIRECTORY 16u
#define FILE_ATTRIBUTE_REPARSE_POINT 1024u
#define FILE_FLAG_OPEN_REPARSE_POINT 0u
#define FILE_FLAG_BACKUP_SEMANTICS 1u
#define FILE_BEGIN 0u
#define FILE_NAME_NORMALIZED 0u
#define MOVEFILE_WRITE_THROUGH 1u
#define MOVEFILE_REPLACE_EXISTING 2u
#define ERROR_FILE_NOT_FOUND ENOENT
#define ERROR_PATH_NOT_FOUND ENOENT
#define NO_ERROR 0u
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define ERROR_OPERATION_ABORTED 995u
#define ERROR_INVALID_DATA 13u
#define ERROR_TIMEOUT 1460u
#define TRUE 1
#define FALSE 0
#define _TRUNCATE SIZE_MAX
#define _wcsicmp wcscasecmp
#define _wcsnicmp wcsncasecmp
#define ZeroMemory(p, n) memset(p, 0, n)

/* 有界复制失败清空输出，格式化保留 Windows 宽字符串格式的既有含义。 */
int strcpy_s(char *, size_t, const char *);
int wcscpy_s(WCHAR *, size_t, const WCHAR *);
int wcscat_s(WCHAR *, size_t, const WCHAR *);
int wcsncpy_s(WCHAR *, size_t, const WCHAR *, size_t);
int sprintf_s(char *, size_t, const char *, ...);
int swprintf_s(WCHAR *, size_t, const WCHAR *, ...);
/* 文件句柄拥有描述符；共享模式为零时取得非阻塞 flock，失败不允许第二个目录写入者。 */
HANDLE CreateFileW(const WCHAR *, DWORD, DWORD, void *, DWORD, DWORD, HANDLE);
int CloseHandle(HANDLE);
int ReadFile(HANDLE, void *, DWORD, DWORD *, void *);
int WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
int FlushFileBuffers(HANDLE);
int GetFileSizeEx(HANDLE, LARGE_INTEGER *);
int SetFilePointerEx(HANDLE, LARGE_INTEGER, LARGE_INTEGER *, DWORD);
int SetEndOfFile(HANDLE);
DWORD GetFileAttributesW(const WCHAR *);
DWORD GetFullPathNameW(const WCHAR *, DWORD, WCHAR *, WCHAR **);
DWORD GetFinalPathNameByHandleW(HANDLE, WCHAR *, DWORD, DWORD);
DWORD GetLastError(void);
int DeleteFileW(const WCHAR *);
int MoveFileExW(const WCHAR *, const WCHAR *, DWORD);
int CopyFileW(const WCHAR *, const WCHAR *, int);
/* 枚举句柄仅扫描指定父目录，文件类型来自 lstat，不沿符号链接清理缓存。 */
HANDLE FindFirstFileW(const WCHAR *, WIN32_FIND_DATAW *);
int FindNextFileW(HANDLE, WIN32_FIND_DATAW *);
int FindClose(HANDLE);
uint64_t GetTickCount64(void);
/* 手动复位取消事件受原子状态保护，等待可用于网络取消和分块让出。 */
HANDLE drop_posix_event_create(void);
void drop_posix_event_set(HANDLE);
void drop_posix_event_reset(HANDLE);
DWORD WaitForSingleObject(HANDLE, DWORD);
int drop_posix_file_unchanged(HANDLE);
/* 校验路径所有中间目录，拒绝链接和非目录，供私有目录及秘密文件使用。 */
int drop_posix_path(const WCHAR *, char *, size_t);
int drop_posix_parent_safe(const char *);
#ifdef __APPLE__
/* 应用包携带的公开根证书集合；不存在时沿用系统 libcurl 信任，显式 CA 优先。 */
int drop_bundle_ca(char *, size_t);
#endif
#endif
