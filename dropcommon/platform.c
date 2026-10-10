#include "drop_common.h"
#include <sddl.h>
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int drop_path_join(const WCHAR *root, const WCHAR *name, WCHAR *path, size_t capacity) {
    size_t length;
    if (!root || !name || !path || !capacity || wcspbrk(name, L"/\\:")) return 0;
    length = wcslen(root);
    return swprintf_s(path, capacity, L"%s%s%s", root,
        length && root[length-1] == L'\\' ? L"" : L"\\", name) >= 0;
}
int drop_private_directory(const WCHAR *path) {
    HANDLE token = NULL; DWORD required = 0; TOKEN_USER *user = NULL;
    WCHAR *sid = NULL, descriptor[512]; PSECURITY_DESCRIPTOR security = NULL;
    SECURITY_ATTRIBUTES attributes; int result = 0; DWORD error;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) goto cleanup;
    GetTokenInformation(token, TokenUser, NULL, 0, &required);
    user = malloc(required); if (!user) goto cleanup;
    if (!GetTokenInformation(token, TokenUser, user, required, &required) ||
        !ConvertSidToStringSidW(user->User.Sid, &sid)) goto cleanup;
    /* Protected DACL: this user, administrators, and SYSTEM; inherited by children. */
    if (swprintf_s(descriptor, 512, L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;%s)", sid) < 0 ||
        !ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor, SDDL_REVISION_1, &security, NULL)) goto cleanup;
    attributes.nLength = sizeof(attributes); attributes.lpSecurityDescriptor = security;
    attributes.bInheritHandle = FALSE;
    if (!CreateDirectoryW(path, &attributes)) {
        error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS || !(GetFileAttributesW(path) & FILE_ATTRIBUTE_DIRECTORY) ||
            (GetFileAttributesW(path) & FILE_ATTRIBUTE_REPARSE_POINT)) goto cleanup;
        if (!SetFileSecurityW(path, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, security)) goto cleanup;
    }
    result = 1;
cleanup:
    if (security) LocalFree(security);
    if (sid) LocalFree(sid);
    free(user); if (token) CloseHandle(token); return result;
}
int drop_file_write(const WCHAR *path, const void *data, DWORD size, int replace) {
    WCHAR temporary[32768]; char id[DROP_ID_CAP]; WCHAR wideId[DROP_ID_CAP];
    HANDLE file; DWORD written = 0; int success;
    drop_id_new(id);
    if (!drop_to_wide(id, wideId, DROP_ID_CAP) ||
        swprintf_s(temporary, 32768, L"%s.%s.tmp", path, wideId) < 0) return 0;
    file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    success = WriteFile(file, data, size, &written, NULL) && written == size && FlushFileBuffers(file);
    CloseHandle(file);
    if (success) success = MoveFileExW(temporary, path, MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0)) != 0;
    if (!success) DeleteFileW(temporary);
    return success;
}
int drop_file_read(const WCHAR *path, unsigned char **data, DWORD *size, DWORD limit) {
    HANDLE file; LARGE_INTEGER length; unsigned char *buffer; DWORD read = 0; int success;
    if (!path || !data || !size) return 0;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (!GetFileSizeEx(file, &length) || length.QuadPart < 0 || length.QuadPart > limit) { CloseHandle(file); return 0; }
    buffer = malloc((size_t)length.QuadPart + 1); if (!buffer) { CloseHandle(file); return 0; }
    success = ReadFile(file, buffer, (DWORD)length.QuadPart, &read, NULL) && read == (DWORD)length.QuadPart;
    CloseHandle(file); if (!success) { free(buffer); return 0; }
    buffer[read] = 0; *data = buffer; *size = read; return 1;
}
int drop_secret_save(const WCHAR *path, const void *data, DWORD size) {
    DATA_BLOB input, output = {0}; int result;
    input.pbData = (BYTE *)data; input.cbData = size;
    if (!CryptProtectData(&input, L"Drop user secret v1", NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &output)) return 0;
    result = drop_file_write(path, output.pbData, output.cbData, 0);
    sodium_memzero(output.pbData, output.cbData); LocalFree(output.pbData); return result;
}
int drop_secret_load(const WCHAR *path, unsigned char **data, DWORD *size) {
    unsigned char *buffer = NULL; DWORD length; DATA_BLOB input, output = {0}; int result = 0;
    if (!drop_file_read(path, &buffer, &length, 65536)) return 0;
    input.pbData = buffer; input.cbData = length;
    if (CryptUnprotectData(&input, NULL, NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        *data = malloc((size_t)output.cbData + 1);
        if (*data) { memcpy(*data, output.pbData, output.cbData); (*data)[output.cbData] = 0; *size = output.cbData; result = 1; }
        sodium_memzero(output.pbData, output.cbData); LocalFree(output.pbData);
    }
    sodium_memzero(buffer, length); free(buffer); return result;
}
int drop_password_read(const WCHAR *prompt, char *password, size_t capacity) {
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE), output = GetStdHandle(STD_OUTPUT_HANDLE);
    WCHAR buffer[1025]; DWORD mode, read = 0, written; int result;
    /* Refuse redirected stdin: secrets must not travel through command-line pipelines. */
    if (!GetConsoleMode(input, &mode)) return 0;
    if (!SetConsoleMode(input, mode & ~(DWORD)ENABLE_ECHO_INPUT)) return 0;
    WriteConsoleW(output, prompt, (DWORD)wcslen(prompt), &written, NULL);
    result = ReadConsoleW(input, buffer, 1024, &read, NULL) != 0;
    SetConsoleMode(input, mode); WriteConsoleW(output, L"\r\n", 2, &written, NULL);
    if (result) {
        while (read && (buffer[read-1] == L'\r' || buffer[read-1] == L'\n')) --read;
        buffer[read] = 0; result = drop_to_utf8(buffer, password, capacity);
    }
    sodium_memzero(buffer, sizeof(buffer)); return result;
}
