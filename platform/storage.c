/* 私有 POSIX 存储和遮蔽输入；凭据主密钥只由系统秘密存储返回，失败不回退到普通文件。 */
#include "drop_common.h"
#include <sodium.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

#ifdef __APPLE__
int drop_bundle_ca(char *path, size_t capacity) {
    CFURLRef url = CFBundleCopyResourceURL(CFBundleGetMainBundle(), CFSTR("ca-bundle"), CFSTR("pem"), NULL);
    int ok = url && CFURLGetFileSystemRepresentation(url, 1, (UInt8 *)path, (CFIndex)capacity);
    if (url) CFRelease(url);
    return ok;
}
#endif

/* 系统密钥环实现拥有其返回缓冲；此接口只复制固定 32 字节主密钥，调用者负责清零。 */
int drop_system_key(const char *identity, unsigned char key[32], int create);

int drop_path_join(const WCHAR *root, const WCHAR *name, WCHAR *path, size_t capacity) {
    size_t length;
    if (!root || !name || !*name || !path || !capacity || wcschr(name, L'/') || !wcscmp(name, L".") || !wcscmp(name, L"..")) return 0;
    length = wcslen(root);
    return swprintf_s(path, capacity, L"%s%s%s", root, length && root[length - 1] == L'/' ? L"" : L"/", name) >= 0;
}
int drop_private_directory(const WCHAR *path) {
    char utf8[PATH_MAX];
    struct stat st;
    int fd, ok;
    if (!drop_posix_path(path, utf8, sizeof(utf8)) || !drop_posix_parent_safe(utf8)) return 0;
    if (mkdir(utf8, 0700) && errno != EEXIST) return 0;
    fd = open(utf8, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return 0;
    ok = !fstat(fd, &st) && st.st_uid == getuid() && !fchmod(fd, 0700);
    close(fd);
    return ok;
}
/* 根据实际可执行文件定位 Data，macOS 应用包的数据放在包旁，保持签名后的程序包只读。 */
int drop_program_data_directory(const WCHAR *application, WCHAR *path, size_t capacity, int create_parent) {
    char executable[PATH_MAX], resolved[PATH_MAX], *slash;
    WCHAR directory[32768], data[32768];
#ifdef __APPLE__
    uint32_t size = sizeof(executable);
    if (_NSGetExecutablePath(executable, &size)) return 0;
#else
    ssize_t size = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    if (size <= 0 || size >= (ssize_t)sizeof(executable) - 1) return 0;
    executable[size] = 0;
#endif
    if (!realpath(executable, resolved) || !(slash = strrchr(resolved, '/'))) return 0;
    *slash = 0;
#ifdef __APPLE__
    slash = strstr(resolved, ".app/Contents/MacOS");
    if (slash && !strcmp(slash, ".app/Contents/MacOS")) {
        *slash = 0;
        slash = strrchr(resolved, '/');
        if (!slash) return 0;
        *slash = 0;
    }
#endif
    return drop_to_wide(resolved, directory, 32768) && drop_path_join(directory, L"Data", data, 32768) && (!create_parent || drop_private_directory(data)) &&
        drop_path_join(data, application, path, capacity);
}
int drop_file_write(const WCHAR *path, const void *data, DWORD size, int replace) {
    WCHAR temporary[32768], wide[DROP_ID_CAP];
    char id[DROP_ID_CAP];
    HANDLE file;
    DWORD written;
    int ok;
    drop_id_new(id);
    if (!drop_to_wide(id, wide, DROP_ID_CAP) || swprintf_s(temporary, 32768, L"%s.%s.tmp", path, wide) < 0) return 0;
    file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    ok = WriteFile(file, data, size, &written, NULL) && written == size && FlushFileBuffers(file);
    CloseHandle(file);
    if (ok) ok = MoveFileExW(temporary, path, MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0));
    if (!ok) DeleteFileW(temporary);
    return ok;
}
int drop_file_read(const WCHAR *path, unsigned char **data, DWORD *size, DWORD limit) {
    HANDLE file;
    LARGE_INTEGER length;
    unsigned char *bytes = NULL;
    DWORD done = 0, n;
    int ok = 0;
    if (!data || !size) return 0;
    *data = NULL;
    *size = 0;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (!GetFileSizeEx(file, &length) || length.QuadPart < 0 || (uint64_t)length.QuadPart > limit || !(bytes = malloc((size_t)length.QuadPart + 1))) goto done;
    while (done < (DWORD)length.QuadPart) {
        if (!ReadFile(file, bytes + done, (DWORD)length.QuadPart - done, &n, NULL) || !n) goto done;
        done += n;
    }
    if (!drop_posix_file_unchanged(file)) goto done;
    bytes[done] = 0;
    *data = bytes;
    *size = done;
    bytes = NULL;
    ok = 1;
done:
    free(bytes);
    CloseHandle(file);
    return ok;
}
/* 规范化秘密文件的父目录身份；目录改名或复制到另一主机后要求重新登录，不自动搬运主密钥。 */
static int secret_key(const WCHAR *path, unsigned char key[32], int create, char utf8[PATH_MAX]) {
    char parent[PATH_MAX], canonical[PATH_MAX], identity[65], *slash;
    unsigned char hash[32];
    struct stat st;
    if (!drop_posix_path(path, utf8, PATH_MAX) || !drop_posix_parent_safe(utf8) || strcpy_s(parent, sizeof(parent), utf8) || !(slash = strrchr(parent, '/'))) return 0;
    if (slash == parent)
        slash[1] = 0;
    else
        *slash = 0;
    if (!realpath(parent, canonical) || lstat(canonical, &st) || st.st_uid != getuid() || (st.st_mode & 077) || !S_ISDIR(st.st_mode)) return 0;
    crypto_hash_sha256(hash, (unsigned char *)canonical, strlen(canonical));
    sodium_bin2hex(identity, sizeof(identity), hash, sizeof(hash));
    return drop_system_key(identity, key, create);
}
/* 以系统保存的随机主密钥加密凭据；帧和路径绑定，任何失败都清零主密钥及临时明文。 */
static int secret_write(const WCHAR *path, const void *data, DWORD size, int replace) {
    unsigned char key[32], *bytes = NULL;
    char utf8[PATH_MAX];
    unsigned long long length;
    int ok = 0;
    if (size > DROP_MAX_JSON || !secret_key(path, key, 1, utf8) || !(bytes = malloc((size_t)size + 48))) goto done;
    memcpy(bytes, "DROPSEC1", 8);
    randombytes_buf(bytes + 8, 24);
    if (crypto_aead_xchacha20poly1305_ietf_encrypt(bytes + 32, &length, data, size, (unsigned char *)utf8, strlen(utf8), NULL, bytes + 8, key)) goto done;
    ok = drop_file_write(path, bytes, (DWORD)length + 32, replace);
done:
    sodium_memzero(key, sizeof(key));
    if (bytes) {
        sodium_memzero(bytes, (size_t)size + 48);
        free(bytes);
    }
    return ok;
}
int drop_secret_save(const WCHAR *path, const void *data, DWORD size) {
    return secret_write(path, data, size, 0);
}
int drop_secret_replace(const WCHAR *path, const void *data, DWORD size) {
    return secret_write(path, data, size, 1);
}
int drop_secret_load(const WCHAR *path, unsigned char **data, DWORD *size) {
    unsigned char key[32], *bytes = NULL, *plain = NULL;
    char utf8[PATH_MAX];
    struct stat st;
    DWORD length = 0;
    unsigned long long opened;
    int ok = 0;
    *data = NULL;
    *size = 0;
    if (!secret_key(path, key, 0, utf8) || lstat(utf8, &st) || !S_ISREG(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077) ||
        !drop_file_read(path, &bytes, &length, DROP_MAX_JSON + 48) || length < 48 || memcmp(bytes, "DROPSEC1", 8) || !(plain = malloc(length)))
        goto done;
    if (crypto_aead_xchacha20poly1305_ietf_decrypt(plain, &opened, NULL, bytes + 32, length - 32, (unsigned char *)utf8, strlen(utf8), bytes + 8, key)) goto done;
    plain[opened] = 0;
    *data = plain;
    *size = (DWORD)opened;
    plain = NULL;
    ok = 1;
done:
    sodium_memzero(key, sizeof(key));
    if (plain) {
        sodium_memzero(plain, length);
        free(plain);
    }
    if (bytes) {
        sodium_memzero(bytes, length);
        free(bytes);
    }
    return ok;
}
/* 从真实终端读取密码，禁止重定向；恢复 echo 后才返回，过长输入整行丢弃。 */
int drop_password_read(const WCHAR *prompt, char *password, size_t capacity) {
    struct termios saved, masked;
    char text[1024], ch;
    size_t length = 0;
    int fd, ok = 0, overflow = 0;
    if (!password || capacity < 2 || !drop_to_utf8(prompt, text, sizeof(text))) return 0;
    password[0] = 0;
    fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (fd < 0 || tcgetattr(fd, &saved)) {
        if (fd >= 0) close(fd);
        return 0;
    }
    masked = saved;
    masked.c_lflag &= ~(ECHO | ECHONL);
    if (tcsetattr(fd, TCSAFLUSH, &masked)) goto done;
    if (write(fd, text, strlen(text)) < 0) goto restore;
    while (read(fd, &ch, 1) == 1) {
        if (ch == '\n' || ch == '\r') {
            ok = !overflow;
            break;
        }
        if (length + 1 < capacity)
            password[length++] = ch;
        else
            overflow = 1;
    }
    password[length] = 0;
restore:
    if (tcsetattr(fd, TCSAFLUSH, &saved)) ok = 0;
    (void)write(fd, "\n", 1);
done:
    close(fd);
    if (!ok) sodium_memzero(password, capacity);
    return ok;
}
