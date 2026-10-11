/* 公开界面设置和当前实例的用户自启动配置，秘密材料仍由独立 DPAPI 存储负责。 */
#include "ui_internal.h"
#include <sodium.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 读取本机公开设置并应用默认值，兼容旧提醒字段；不从普通设置文件读取密码或令牌。 */
int drop_ui_settings_load(drop_ui *ui) {
    WCHAR path[32768];
    unsigned char *bytes = NULL;
    DWORD size;
    cJSON *json = NULL;
    const char *value;
    int ok = 0;
    ui->notifications = 1;
    ui->tray_flash = 1;
    ui->to = INT64_MAX;
    ui->zstd_level = 3;
    if (!drop_path_join(ui->root, L"ui.json", path, 32768)) return 0;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return 1;
    if (!drop_file_read(path, &bytes, &size, 65536) || !(json = drop_json_parse((char *)bytes, size))) goto done;
    value = drop_json_string(json, "url", 8192);
    if (value && !drop_to_wide(value, ui->url, 2049)) goto done;
    value = drop_json_string(json, "ca_file", 32768);
    if (value && !drop_to_wide(value, ui->ca, 32768)) goto done;
    value = drop_json_string(json, "login", 64);
    if (value && !drop_to_wide(value, ui->login, 65)) goto done;
    ui->notifications = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json, "notifications"));
    ui->tray_flash = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(json, "tray_flash"));
    ui->autostart = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(json, "autostart"));
    value = drop_json_string(json, "text_compression", 8);
    if (value) {
        cJSON *level = cJSON_GetObjectItemCaseSensitive(json, "zstd_level");
        if ((strcmp(value, "none") && strcmp(value, "zstd")) || !cJSON_IsNumber(level) || level->valueint < 1 || level->valueint > 19 ||
            level->valuedouble != (double)level->valueint)
            goto done;
        ui->use_zstd = !strcmp(value, "zstd");
        ui->zstd_level = level->valueint;
    }
    ok = 1;
done:
    free(bytes);
    cJSON_Delete(json);
    return ok;
}
/* 将服务器地址、提醒和压缩等公开设置写入本机文件；秘密材料另由 DPAPI 管理。 */
int drop_ui_settings_save(drop_ui *ui) {
    WCHAR path[32768];
    char *wide_buffer = malloc(131072), *serialized = NULL;
    cJSON *json = cJSON_CreateObject();
    int ok = 0;
    if (!wide_buffer || !json || !drop_path_join(ui->root, L"ui.json", path, 32768) || !drop_to_utf8(ui->url, wide_buffer, 131072) ||
        !cJSON_AddStringToObject(json, "url", wide_buffer) || !drop_to_utf8(ui->ca, wide_buffer, 131072) || !cJSON_AddStringToObject(json, "ca_file", wide_buffer) ||
        !drop_to_utf8(ui->login, wide_buffer, 131072) || !cJSON_AddStringToObject(json, "login", wide_buffer) || !cJSON_AddBoolToObject(json, "notifications", ui->notifications) ||
        !cJSON_AddBoolToObject(json, "tray_flash", ui->tray_flash) || !cJSON_AddBoolToObject(json, "autostart", ui->autostart) ||
        !cJSON_AddStringToObject(json, "text_compression", ui->use_zstd ? "zstd" : "none") || !cJSON_AddNumberToObject(json, "zstd_level", ui->zstd_level))
        goto done;
    serialized = cJSON_PrintUnformatted(json);
    if (!serialized) goto done;
    ok = drop_file_write(path, serialized, (DWORD)strlen(serialized), 1);
done:
    free(wide_buffer);
    cJSON_free(serialized);
    cJSON_Delete(json);
    return ok;
}
/* 仅修改本数据目录实例对应的用户启动项，启用命令带后台参数；不触碰其他应用启动配置。 */
int drop_ui_autostart(drop_ui *ui, int enabled) {
    WCHAR executable[32768], *command = NULL, name[96];
    HKEY key = NULL;
    LSTATUS status;
    int ok = 0;
    swprintf_s(name, 96, L"Drop-%s", ui->class_name + 11);
    status = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_SET_VALUE, &key);
    if (status == ERROR_FILE_NOT_FOUND)
        status = RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, NULL, 0, KEY_SET_VALUE, NULL, &key, NULL);
    if (status != ERROR_SUCCESS) {
        SetLastError((DWORD)status);
        return 0;
    }
    if (!enabled) {
        status = RegDeleteValueW(key, name);
        ok = status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
        goto done;
    }
    if (!GetModuleFileNameW(NULL, executable, 32768)) goto done;
    command = malloc(131072 * sizeof(WCHAR));
    if (!command || swprintf_s(command, 131072, L"\"%s\" --ui-root \"%s\" --background", executable, ui->root) < 0) goto done;
    status = RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)command, (DWORD)((wcslen(command) + 1) * sizeof(WCHAR)));
    ok = status == ERROR_SUCCESS;
done:
    free(command);
    RegCloseKey(key);
    if (!ok) SetLastError((DWORD)status);
    return ok;
}
