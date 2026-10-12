/* Keychain 仅保存本机随机主密钥；秘密缓冲在复制后释放，界面授权由系统处理。 */
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <sodium.h>
#include <stdio.h>

/* 查询或创建按数据目录区分的 Keychain 项，不同步到 iCloud，不持久保存账号密码。 */
int drop_system_key(const char *identity, unsigned char key[32], int create) {
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef account = CFStringCreateWithCString(NULL, identity, kCFStringEncodingUTF8);
    CFTypeRef found = NULL;
    CFDataRef value = NULL;
    OSStatus status = errSecParam;
    int ok = 0;
    if (!query || !account) goto done;
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, CFSTR("win.jsnowly.Drop"));
    CFDictionarySetValue(query, kSecAttrAccount, account);
    CFDictionarySetValue(query, kSecAttrSynchronizable, kCFBooleanFalse);
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    status = SecItemCopyMatching(query, &found);
    if (status == errSecSuccess && CFGetTypeID(found) == CFDataGetTypeID() && CFDataGetLength((CFDataRef)found) == 32) {
        memcpy(key, CFDataGetBytePtr((CFDataRef)found), 32);
        ok = 1;
    } else if (status == errSecItemNotFound && create) {
        randombytes_buf(key, 32);
        value = CFDataCreate(NULL, key, 32);
        if (!value) goto done;
        CFDictionaryRemoveValue(query, kSecReturnData);
        CFDictionarySetValue(query, kSecValueData, value);
        status = SecItemAdd(query, NULL);
        ok = status == errSecSuccess;
    }
done:
    if (value) CFRelease(value);
    if (found) CFRelease(found);
    if (account) CFRelease(account);
    if (query) CFRelease(query);
    if (!ok) {
        /* 仅报告系统错误码，便于区分锁定或会话限制，不能输出 Keychain 项内容。 */
        fprintf(stderr, "keychain_unavailable status=%d\n", (int)status);
        sodium_memzero(key, 32);
    }
    return ok;
}
