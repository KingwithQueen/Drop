#include "drop_common.h"
#include <wincrypt.h>
#include <dpapi.h>
#include <sddl.h>
#include <aclapi.h>
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
    HANDLE file; DWORD written = 0,error=ERROR_SUCCESS; int success;
    drop_id_new(id);
    if (!drop_to_wide(id, wideId, DROP_ID_CAP) ||
        swprintf_s(temporary, 32768, L"%s.%s.tmp", path, wideId) < 0) return 0;
    file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    success = WriteFile(file, data, size, &written, NULL) && written == size && FlushFileBuffers(file);
    if(!success) error=GetLastError();
    CloseHandle(file);
    if (success) success = MoveFileExW(temporary, path, MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0)) != 0;
    if (!success) { if(!error) error=GetLastError(); DeleteFileW(temporary); SetLastError(error); }
    return success;
}
int drop_file_read(const WCHAR *path, unsigned char **data, DWORD *size, DWORD limit) {
    HANDLE file; LARGE_INTEGER length; unsigned char *buffer; DWORD read = 0; int success;
    if (!path || !data || !size) return 0;
    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    if (!GetFileSizeEx(file, &length) || length.QuadPart < 0 || length.QuadPart > limit) { CloseHandle(file); return 0; }
    buffer = malloc((size_t)length.QuadPart + 1); if (!buffer) { CloseHandle(file); return 0; }
    success = ReadFile(file, buffer, (DWORD)length.QuadPart, &read, NULL) && read == (DWORD)length.QuadPart;
    CloseHandle(file); if (!success) { free(buffer); return 0; }
    buffer[read] = 0; *data = buffer; *size = read; return 1;
}
int drop_machine_directory(const WCHAR *path) {
    PSECURITY_DESCRIPTOR security=NULL; SECURITY_ATTRIBUTES attributes; DWORD error; int ok=0;
    SID_IDENTIFIER_AUTHORITY authority=SECURITY_NT_AUTHORITY; PSID administrators=NULL; BOOL member=FALSE;
    if(!AllocateAndInitializeSid(&authority,2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_ADMINS,0,0,0,0,0,0,&administrators)) return 0;
    CheckTokenMembership(NULL,administrators,&member); FreeSid(administrators);
    if(!member) { SetLastError(ERROR_ACCESS_DENIED); return 0; }
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",SDDL_REVISION_1,&security,NULL)) return 0;
    attributes.nLength=sizeof(attributes); attributes.lpSecurityDescriptor=security; attributes.bInheritHandle=FALSE;
    if(CreateDirectoryW(path,&attributes)) ok=1;
    else {
        error=GetLastError();
        if(error==ERROR_ALREADY_EXISTS && (GetFileAttributesW(path)&FILE_ATTRIBUTE_DIRECTORY) && !(GetFileAttributesW(path)&FILE_ATTRIBUTE_REPARSE_POINT))
            ok=SetFileSecurityW(path,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,security)!=0;
    } LocalFree(security); return ok;
}
static int machine_children(const WCHAR *path,PSECURITY_DESCRIPTOR security,unsigned depth) {
    WCHAR *search=malloc(32768*sizeof(WCHAR)),*child=malloc(32768*sizeof(WCHAR)); WIN32_FIND_DATAW item; HANDLE find=INVALID_HANDLE_VALUE; int ok=0;
    PACL dacl=NULL; PSID owner=NULL; BOOL present,defaulted;
    if(depth>16 || !search || !child || !GetSecurityDescriptorDacl(security,&present,&dacl,&defaulted) || !present || !GetSecurityDescriptorOwner(security,&owner,&defaulted) || !drop_path_join(path,L"*",search,32768)) goto done;
    find=FindFirstFileW(search,&item); if(find==INVALID_HANDLE_VALUE) goto done;
    do {
        HANDLE file; BY_HANDLE_FILE_INFORMATION info;
        if(!wcscmp(item.cFileName,L".") || !wcscmp(item.cFileName,L"..")) continue;
        if((item.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) || !drop_path_join(path,item.cFileName,child,32768)) goto done;
        file=CreateFileW(child,READ_CONTROL|WRITE_DAC|WRITE_OWNER,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,NULL);
        if(file==INVALID_HANDLE_VALUE) goto done;
        if(!GetFileInformationByHandle(file,&info) || (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) || SetSecurityInfo(file,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,owner,NULL,dacl,NULL)!=ERROR_SUCCESS) { CloseHandle(file); goto done; }
        CloseHandle(file);
        if((item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) && !machine_children(child,security,depth+1)) goto done;
    } while(FindNextFileW(find,&item)); ok=GetLastError()==ERROR_NO_MORE_FILES;
done:
    if(find!=INVALID_HANDLE_VALUE) FindClose(find); free(search); free(child); return ok;
}
int drop_machine_tree(const WCHAR *path) {
    PSECURITY_DESCRIPTOR security=NULL; int ok=0;
    if(!drop_machine_directory(path) || !ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",SDDL_REVISION_1,&security,NULL)) return 0;
    ok=machine_children(path,security,0); LocalFree(security); return ok;
}
static int secret_write(const WCHAR *path, const void *data, DWORD size, int replace,DWORD flags) {
    DATA_BLOB input, output = {0}; int result;
    input.pbData = (BYTE *)data; input.cbData = size;
    if (!CryptProtectData(&input, L"Drop protected secret v1", NULL, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN|flags, &output)) return 0;
    result = drop_file_write(path, output.pbData, output.cbData, replace);
    sodium_memzero(output.pbData, output.cbData); LocalFree(output.pbData); return result;
}
int drop_secret_save(const WCHAR *path, const void *data, DWORD size) { return secret_write(path, data, size, 0,0); }
int drop_secret_replace(const WCHAR *path, const void *data, DWORD size) { return secret_write(path, data, size, 1,0); }
int drop_machine_secret_save(const WCHAR *path,const void *data,DWORD size) { return secret_write(path,data,size,0,CRYPTPROTECT_LOCAL_MACHINE); }
int drop_machine_secret_replace(const WCHAR *path,const void *data,DWORD size) { return secret_write(path,data,size,1,CRYPTPROTECT_LOCAL_MACHINE); }
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
    WCHAR buffer[1027]; DWORD mode, read = 0, written; int result;
    /* Refuse redirected stdin: secrets must not travel through command-line pipelines. */
    if (!GetConsoleMode(input, &mode)) return 0;
    if (!SetConsoleMode(input, mode & ~(DWORD)ENABLE_ECHO_INPUT)) return 0;
    WriteConsoleW(output, prompt, (DWORD)wcslen(prompt), &written, NULL);
    result = ReadConsoleW(input, buffer, 1026, &read, NULL) != 0;
    SetConsoleMode(input, mode); WriteConsoleW(output, L"\r\n", 2, &written, NULL);
    if (result) {
        if (!read || (buffer[read-1] != L'\r' && buffer[read-1] != L'\n')) {
            sodium_memzero(buffer, sizeof(buffer)); return 0;
        }
        while (read && (buffer[read-1] == L'\r' || buffer[read-1] == L'\n')) --read;
        buffer[read] = 0; result = drop_to_utf8(buffer, password, capacity);
    }
    sodium_memzero(buffer, sizeof(buffer)); return result;
}
