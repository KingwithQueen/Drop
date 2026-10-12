/* Linux/macOS 命令入口复用持久同步核心；真实密码仅从遮蔽终端输入，GUI 由独立原生适配器接入。 */
#include "runtime.h"
#include <sodium.h>
#include <sys/stat.h>
#include <signal.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>

static volatile sig_atomic_t 停止请求;
int drop_process_stop_requested(void) {
    return 停止请求 != 0;
}
static void stopped(int signal_number) {
    (void)signal_number;
    停止请求 = 1;
}

/* UI/命令请求在同一核心执行；先身份复核再发送，删除在每轮同步时补齐。 */
static drop_error synchronize(drop_client *client, int send) {
    drop_error error = drop_client_reconcile(client);
    if (error == DROP_OK) error = drop_client_deletions(client);
    if (error == DROP_OK && send) error = drop_client_flush(client);
    if (error == DROP_OK) error = drop_client_sync(client);
    return error;
}
/* 历史输出仅包含消息、文件名及状态，不暴露登录凭据；分页查询沿用现有核心。 */
static int history(drop_client *client, int page) {
    drop_history *view = drop_ui_history(client, page, NULL, 0, INT64_MAX, "");
    char text[8193];
    if (!view) return 0;
    for (int i = 0; i < view->count; ++i) {
        drop_history_row *row = &view->rows[i];
        printf("%s %s\n", row->id, row->mine ? "自己" : "对方");
        if (drop_to_utf8(row->time, text, sizeof(text))) puts(text);
        if (drop_to_utf8(row->preview, text, sizeof(text))) puts(text);
        if (row->content)
            for (int j = 0; j < row->content->count; ++j) {
                drop_file_view *file = &row->content->files[j];
                if (drop_to_utf8(file->name, text, sizeof(text)))
                    printf("  %s: %s (%llu bytes, %s)\n", file->id, text, (unsigned long long)file->total, file->complete ? "已保存" : "下载待完成");
            }
    }
    printf("Pending: %d; downloads: %d; deletes: %d; more: %d\n", view->pending_count, view->downloads, view->pending_deletes, view->more);
    drop_ui_history_free(view);
    return 1;
}
/* 参数中的数据目录和用户文件均先变成绝对路径，业务核心不依赖启动时的工作目录。 */
static WCHAR *wide_argument(const char *argument, int path) {
    WCHAR *wide = calloc(32768, sizeof(WCHAR)), *absolute = NULL;
    if (!wide || !drop_to_wide(argument, wide, 32768)) {
        free(wide);
        return NULL;
    }
    if (!path) return wide;
    absolute = calloc(32768, sizeof(WCHAR));
    if (!absolute || !GetFullPathNameW(wide, 32768, absolute, NULL)) {
        free(absolute);
        absolute = NULL;
    }
    free(wide);
    return absolute;
}
static void usage(void) {
    puts("Drop " DROP_VERSION "\n"
         "  drop --version | data-directory\n"
         "  drop --probe URL\n"
         "  drop client-login URL DATA_DIRECTORY LOGIN [CA_PEM]\n"
         "  drop client-queue DATA_DIRECTORY UTF8_BODY_FILE [RECIPIENT_ID]\n"
         "  drop client-attach DATA_DIRECTORY UTF8_BODY_FILE self|RECIPIENT_ID FILE...\n"
         "  drop client-send DATA_DIRECTORY\n"
         "  drop client-sync DATA_DIRECTORY\n"
         "  drop client-watch DATA_DIRECTORY\n"
         "  drop client-run DATA_DIRECTORY\n"
         "  drop client-history DATA_DIRECTORY [PAGE]\n"
         "  drop client-find DATA_DIRECTORY LOGIN\n"
         "  drop client-read DATA_DIRECTORY MESSAGE_ID\n"
         "  drop client-delete DATA_DIRECTORY MESSAGE_ID --confirm\n"
         "  drop client-export DATA_DIRECTORY MESSAGE_ID ATTACHMENT_ID DESTINATION\n"
         "  drop client-retry-reviewed DATA_DIRECTORY --confirm");
}
int main(int argc, char **argv) {
    drop_client *client = NULL;
    WCHAR *root = NULL, *url = NULL, *ca = NULL, *body_path = NULL;
    drop_error error = DROP_E_ARGUMENT;
    int result = 2;
    umask(0077);
    if (!drop_init()) return 1;
    signal(SIGINT, stopped);
    signal(SIGTERM, stopped);
    if (argc == 1 || (argc == 2 && !strcmp(argv[1], "--background"))) return drop_native_ui(NULL, argc == 2);
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "--ui-root") && (argc == 3 || !strcmp(argv[3], "--background"))) {
        root = wide_argument(argv[2], 1);
        if (!root) return 2;
        result = drop_native_ui(root, argc == 4);
        free(root);
        return result;
    }
    if (argc == 2 && !strcmp(argv[1], "--version")) {
        puts("Drop " DROP_VERSION " (" DROP_DEVICE_NAME ")");
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "data-directory")) {
        WCHAR path[32768];
        char text[131072];
        if (!drop_program_data_directory(L"Drop", path, 32768, 0) || !drop_to_utf8(path, text, sizeof(text))) return 1;
        puts(text);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--probe")) {
        url = wide_argument(argv[2], 0);
        result = url && drop_network_probe(url) ? 0 : 1;
        goto done;
    }
    if ((argc == 5 || argc == 6) && !strcmp(argv[1], "client-login")) {
        char password[4096] = {0};
        root = wide_argument(argv[3], 1);
        url = wide_argument(argv[2], 0);
        if (argc == 6) ca = wide_argument(argv[5], 1);
        if (!root || !url || (argc == 6 && !ca) || !drop_account_login_valid(argv[4])) goto done;
        result = drop_password_read(L"Account password: ", password, sizeof(password)) && drop_client_login(root, url, argv[4], password, ca) ? 0 : 1;
        sodium_memzero(password, sizeof(password));
        puts(result ? "Login failed; check account, network and unlocked system keyring." : "Protected login saved.");
        goto done;
    }
    if (argc < 3) {
        usage();
        goto done;
    }
    root = wide_argument(argv[2], 1);
    client = calloc(1, sizeof(*client));
    if (!root || !client) goto done;
    if (!drop_client_open(client, root)) {
        puts("Client state unavailable, keyring locked or directory already in use.");
        result = 1;
        free(client);
        client = NULL;
        goto done;
    }
    if (argc == 3 && (!strcmp(argv[1], "client-sync") || !strcmp(argv[1], "client-send"))) {
        uint64_t deadline = GetTickCount64() + 3600000;
        do {
            error = synchronize(client, !strcmp(argv[1], "client-send"));
        } while (error == DROP_E_PENDING && GetTickCount64() < deadline && !停止请求 && drop_network_wait(1000));
    } else if (argc == 3 && !strcmp(argv[1], "client-watch"))
        error = drop_client_watch(client) ? DROP_OK : DROP_E_NETWORK;
    else if (argc == 3 && !strcmp(argv[1], "client-run")) {
        unsigned retry = 1;
        while (!停止请求) {
            error = synchronize(client, 1);
            printf("Sync: %s\n", drop_error_name(error));
            fflush(stdout);
            if (error == DROP_E_AUTH || error == DROP_E_CONFLICT || error == DROP_E_PROTOCOL) break;
            unsigned delay = error == DROP_OK ? 5 : retry;
            if (error == DROP_OK)
                retry = 1;
            else if (retry < 60)
                retry = retry > 30 ? 60 : retry * 2;
            for (unsigned i = 0; i < delay * 10 && !停止请求; ++i)
                drop_network_wait(100);
        }
    } else if ((argc == 3 || argc == 4) && !strcmp(argv[1], "client-history")) {
        uint64_t page = 0;
        error = (argc == 3 || (drop_u64_parse(argv[3], &page) && page <= 1000000)) && history(client, (int)page) ? DROP_OK : DROP_E_ARGUMENT;
    } else if (argc == 4 && !strcmp(argv[1], "client-find")) {
        char recipient[DROP_ID_CAP];
        error = drop_ui_recipient(client, argv[3], recipient);
        if (error == DROP_OK) puts(recipient);
    } else if ((argc == 4 || argc == 5) && !strcmp(argv[1], "client-queue")) {
        unsigned char *body = NULL;
        DWORD size;
        char request[DROP_ID_CAP];
        body_path = wide_argument(argv[3], 1);
        if (body_path && drop_file_read(body_path, &body, &size, DROP_MAX_BODY) && drop_utf8_valid((char *)body, size))
            error = drop_client_queue(client, (char *)body, argc == 5 ? argv[4] : NULL, request);
        if (error == DROP_OK) puts(request);
        free(body);
    } else if (argc >= 6 && argc <= 5 + DROP_MAX_ATTACHMENTS && !strcmp(argv[1], "client-attach")) {
        unsigned char *body = NULL;
        DWORD size;
        WCHAR *paths[DROP_MAX_ATTACHMENTS] = {0};
        char request[DROP_ID_CAP];
        int valid = 1;
        body_path = wide_argument(argv[3], 1);
        for (int i = 5; i < argc; ++i)
            if (!(paths[i - 5] = wide_argument(argv[i], 1))) valid = 0;
        if (valid && body_path && drop_file_read(body_path, &body, &size, DROP_MAX_BODY) && drop_utf8_valid((char *)body, size))
            error = drop_client_queue_files(client, (char *)body, !strcmp(argv[4], "self") ? NULL : argv[4], (const WCHAR *const *)paths, argc - 5, request);
        if (error == DROP_OK) puts(request);
        for (int i = 5; i < argc; ++i)
            free(paths[i - 5]);
        free(body);
    } else if (argc == 4 && !strcmp(argv[1], "client-read")) {
        error = drop_client_read_queue(client, argv[3]) ? drop_client_read_receipts(client) : DROP_E_ARGUMENT;
    } else if (argc == 5 && !strcmp(argv[1], "client-delete") && !strcmp(argv[4], "--confirm")) {
        error = drop_client_delete_queue(client, argv[3]);
        if (error == DROP_OK) puts("Deletion queued; all devices will receive it after synchronization.");
    } else if (argc == 4 && !strcmp(argv[1], "client-retry-reviewed") && !strcmp(argv[3], "--confirm")) {
        error = drop_client_retry_review(client);
    } else if (argc == 6 && !strcmp(argv[1], "client-export")) {
        drop_detail *detail = drop_ui_detail(client, argv[3], 0);
        WCHAR *destination = wide_argument(argv[5], 1);
        if (detail && destination)
            for (int i = 0; i < detail->count; ++i)
                if (!strcmp(detail->files[i].id, argv[4])) error = drop_ui_export(client, &detail->files[i], destination);
        drop_ui_detail_free(detail);
        free(destination);
    } else
        usage();
    printf("Client: %s\n", drop_error_name(error));
    result = error == DROP_OK ? 0 : 1;
done:
    if (client) {
        drop_client_close(client);
        free(client);
    }
    free(root);
    free(url);
    free(ca);
    free(body_path);
    return result;
}
