/* Windows 服务调度和生命周期管理；共享退出事件防止服务器关闭与 SCM 控制回调竞态。 */
#include "server.h"
#include <winsvc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* SCM 注册返回的状态通道，服务入口和控制回调共用；仅用于上报本实例状态。 */
static SERVICE_STATUS_HANDLE 服务状态句柄;
/* 本实例向 SCM 上报的运行状态，控制回调与服务入口按既有生命周期更新。 */
static SERVICE_STATUS 服务状态;
/* 服务器退出事件的重复句柄，在服务事件锁下访问；控制回调只置位，服务入口最终关闭。 */
static HANDLE 服务退出事件;
/* 保护服务退出事件的安装、置位及关闭，防止停止回调访问已经关闭的句柄。 */
static SRWLOCK 服务事件锁 = SRWLOCK_INIT;
/* 进入 SCM 调度前保存的参数个数，与下方参数列表一起供服务入口读取。 */
static int 服务参数个数;
/* 借用主入口的 argv，SCM 调度返回前必须保持有效；不复制或持久保存密码。 */
static WCHAR **服务参数列表;
/* 通过原子操作记录提前到达的停止请求，使服务器尚未就绪时也能在就绪后立即退出。 */
static LONG 服务停止中;
/* 从服务命令的参数对中查找指定值，未提供时返回空指针。 */
static const WCHAR *option(int argc, WCHAR **argv, const WCHAR *key) {
    int i;
    for (i = 2; i + 1 < argc; i += 2)
        if (!wcscmp(argv[i], key)) return argv[i + 1];
    return NULL;
}
/* 向 SCM 报告当前服务状态及等待信息，停止期间限制可接受的控制命令。 */
static void report(DWORD state, DWORD error) {
    服务状态.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    服务状态.dwCurrentState = state;
    服务状态.dwControlsAccepted = state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN : 0;
    服务状态.dwWin32ExitCode = error;
    服务状态.dwWaitHint = 60000;
    服务状态.dwCheckPoint = state == SERVICE_START_PENDING || state == SERVICE_STOP_PENDING ? 服务状态.dwCheckPoint + 1 : 0;
    SetServiceStatus(服务状态句柄, &服务状态);
}
/* 服务控制回调只更新停止状态并在锁内发出停止事件，避免与服务器关闭并发使用无效句柄。 */
static DWORD WINAPI control(DWORD event, DWORD type, LPVOID data, LPVOID context) {
    (void)type;
    (void)data;
    (void)context;
    if (event == SERVICE_CONTROL_STOP || event == SERVICE_CONTROL_SHUTDOWN) {
        InterlockedExchange(&服务停止中, 1);
        report(SERVICE_STOP_PENDING, NO_ERROR);
        AcquireSRWLockShared(&服务事件锁);
        if (服务退出事件) SetEvent(服务退出事件);
        ReleaseSRWLockShared(&服务事件锁);
    } else if (event == SERVICE_CONTROL_INTERROGATE)
        SetServiceStatus(服务状态句柄, &服务状态);
    return NO_ERROR;
}
/* 服务器完成监听后向服务控制器报告运行状态，并绑定其退出事件。 */
void drop_service_ready(drop_server *server) {
    if (!服务状态句柄) return;
    AcquireSRWLockExclusive(&服务事件锁);
    DuplicateHandle(GetCurrentProcess(), server->stop_event, GetCurrentProcess(), &服务退出事件, EVENT_MODIFY_STATE, FALSE, 0);
    ReleaseSRWLockExclusive(&服务事件锁);
    if (InterlockedCompareExchange(&服务停止中, 0, 0))
        SetEvent(server->stop_event);
    else
        report(SERVICE_RUNNING, NO_ERROR);
}
/* 以 SCM 入口调用已保存的服务器参数，维护启动、运行和停止状态并同步退出事件生命周期。 */
static void WINAPI service_main(DWORD argc, LPWSTR *argv) {
    const WCHAR *name = option(服务参数个数, 服务参数列表, L"--name");
    int result;
    (void)argc;
    (void)argv;
    服务状态句柄 = RegisterServiceCtrlHandlerExW(name, control, NULL);
    if (!服务状态句柄) return;
    report(SERVICE_START_PENDING, NO_ERROR);
    result = drop_server_command(服务参数个数, 服务参数列表);
    AcquireSRWLockExclusive(&服务事件锁);
    if (服务退出事件) CloseHandle(服务退出事件);
    服务退出事件 = NULL;
    ReleaseSRWLockExclusive(&服务事件锁);
    report(SERVICE_STOPPED, result ? ERROR_SERVICE_SPECIFIC_ERROR : NO_ERROR);
}
/* 限制 Windows 服务名的长度和字符，避免非法名称进入 SCM 管理接口。 */
static int name_valid(const WCHAR *name) {
    size_t i, n = name ? wcslen(name) : 0;
    if (n < 3 || n > 80) return 0;
    for (i = 0; i < n; i++)
        if (!((name[i] >= L'A' && name[i] <= L'Z') || (name[i] >= L'a' && name[i] <= L'z') || (name[i] >= L'0' && name[i] <= L'9') || name[i] == L'-' || name[i] == L'_')) return 0;
    return 1;
}
/* 把服务参数路径解析为绝对路径，服务启动后不依赖进程当前目录。 */
static int absolute(const WCHAR *source, WCHAR *target) {
    DWORD n;
    if (!source || wcspbrk(source, L"\"\r\n")) return 0;
    n = GetFullPathNameW(source, 32768, target, NULL);
    return n && n < 32768;
}
/* 轮询 SCM 到目标状态或超时，供安装、停止和卸载的生命周期处理。 */
static int wait_state(SC_HANDLE service, DWORD state) {
    SERVICE_STATUS_PROCESS status;
    DWORD needed;
    ULONGLONG end = GetTickCount64() + 60000;
    do {
        if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (BYTE *)&status, sizeof(status), &needed)) return 0;
        if (status.dwCurrentState == state) return 1;
        if (state == SERVICE_RUNNING && status.dwCurrentState == SERVICE_STOPPED) return 0;
        Sleep(100);
    } while (GetTickCount64() < end);
    return 0;
}
/* 处理服务调度、安装、启停和卸载；固定数据路径，失败回滚本次安装，不影响其他服务。 */
int drop_service_command(int argc, WCHAR **argv) {
    const WCHAR *name = option(argc, argv, L"--name"), *directory = option(argc, argv, L"--data"), *certificate = option(argc, argv, L"--certificate"),
                *listen = option(argc, argv, L"--listen");
    WCHAR *exe = malloc(32768 * sizeof(WCHAR)), *root = malloc(32768 * sizeof(WCHAR)), *cert = malloc(32768 * sizeof(WCHAR)), *command = malloc(100000 * sizeof(WCHAR));
    SC_HANDLE manager = NULL, service = NULL;
    SERVICE_STATUS status;
    QUERY_SERVICE_CONFIGW *config = NULL;
    DWORD needed = 0;
    int ok = 0, i;
    if (!exe || !root || !cert || !command || argc % 2 || !name_valid(name) || !GetModuleFileNameW(NULL, exe, 32768)) goto done;
    if (!wcscmp(argv[1], L"service-run")) {
        SERVICE_TABLE_ENTRYW table[2] = {{(WCHAR *)name, service_main}, {NULL, NULL}};
        if (!directory || !*directory) {
            SetLastError(ERROR_INVALID_PARAMETER);
            goto done;
        }
        服务参数个数 = argc;
        服务参数列表 = argv;
        ok = StartServiceCtrlDispatcherW(table) != 0;
        goto done;
    }
    for (i = 2; i + 1 < argc; i += 2) {
        int j;
        if (wcscmp(argv[i], L"--name") &&
            (wcscmp(argv[1], L"service-install") ||
                (wcscmp(argv[i], L"--data") && wcscmp(argv[i], L"--certificate") && wcscmp(argv[i], L"--listen") && wcscmp(argv[i], L"--backup-directory") &&
                    wcscmp(argv[i], L"--backup-seconds") && wcscmp(argv[i], L"--config"))))
            goto done;
        for (j = 2; j < i; j += 2)
            if (!wcscmp(argv[j], argv[i])) goto done;
    }
    manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT | (!wcscmp(argv[1], L"service-install") ? SC_MANAGER_CREATE_SERVICE : 0));
    if (!manager) goto done;
    if (!wcscmp(argv[1], L"service-install")) {
        SERVICE_FAILURE_ACTIONSW failure = {0};
        SC_ACTION actions[3] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 30000}, {SC_ACTION_NONE, 0}};
        if (!drop_server_resolve_directory(directory, root, 32768) || (certificate && !absolute(certificate, cert)) || (listen && wcspbrk(listen, L"\"\r\n ")) ||
            swprintf_s(command, 100000, L"\"%s\" service-run --name %s --data \"%s\" --listen %s", exe, name, root,
                listen ? listen : (certificate ? L"127.0.0.1:8443" : L"127.0.0.1:8080")) < 0)
            goto done;
        if (certificate) {
            size_t used = wcslen(command);
            if (swprintf_s(command + used, 100000 - used, L" --certificate \"%s\"", cert) < 0) goto done;
        }
        {
            WCHAR key[32768];
            if (!drop_path_join(root, L"runtime-machine.dpapi", key, 32768) || GetFileAttributesW(key) == INVALID_FILE_ATTRIBUTES) goto done;
        }
        for (i = 2; i + 1 < argc; i += 2) {
            size_t used = wcslen(command);
            if (!wcscmp(argv[i], L"--backup-directory") || !wcscmp(argv[i], L"--config")) {
                if (!absolute(argv[i + 1], root) || swprintf_s(command + used, 100000 - used, L" %s \"%s\"", argv[i], root) < 0) goto done;
            } else if (!wcscmp(argv[i], L"--backup-seconds")) {
                char number[32];
                uint64_t seconds;
                if (!option(argc, argv, L"--backup-directory") || !drop_to_utf8(argv[i + 1], number, sizeof(number)) || !drop_u64_parse(number, &seconds) || seconds > 604800 ||
                    (seconds && seconds < 60) || swprintf_s(command + used, 100000 - used, L" --backup-seconds %s", argv[i + 1]) < 0)
                    goto done;
            }
        }
        if (wcslen(command) > 32766) goto done;
        service = CreateServiceW(manager, name, name, SERVICE_CHANGE_CONFIG | SERVICE_QUERY_STATUS | SERVICE_START | DELETE, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
            SERVICE_ERROR_NORMAL, command, NULL, NULL, NULL, NULL, NULL);
        if (!service) goto done;
        failure.dwResetPeriod = 86400;
        failure.cActions = 3;
        failure.lpsaActions = actions;
        ok = ChangeServiceConfig2W(service, SERVICE_CONFIG_FAILURE_ACTIONS, &failure) != 0;
        if (!ok) DeleteService(service);
    } else {
        DWORD access = SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS;
        if (!wcscmp(argv[1], L"service-start"))
            access |= SERVICE_START;
        else if (!wcscmp(argv[1], L"service-stop"))
            access |= SERVICE_STOP;
        else if (!wcscmp(argv[1], L"service-uninstall"))
            access |= SERVICE_STOP | DELETE;
        service = OpenServiceW(manager, name, access);
        if (!service) goto done;
        QueryServiceConfigW(service, NULL, 0, &needed);
        config = malloc(needed);
        if (!config || !QueryServiceConfigW(service, config, needed, &needed) || swprintf_s(command, 100000, L"\"%s\" service-run ", exe) < 0 ||
            wcsncmp(config->lpBinaryPathName, command, wcslen(command)))
            goto done;
        if (!wcscmp(argv[1], L"service-start"))
            ok = (StartServiceW(service, 0, NULL) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING) && wait_state(service, SERVICE_RUNNING);
        else if (!wcscmp(argv[1], L"service-stop") || !wcscmp(argv[1], L"service-uninstall")) {
            ok = (ControlService(service, SERVICE_CONTROL_STOP, &status) || GetLastError() == ERROR_SERVICE_NOT_ACTIVE) && wait_state(service, SERVICE_STOPPED);
            if (ok && !wcscmp(argv[1], L"service-uninstall")) ok = DeleteService(service) != 0;
        } else if (!wcscmp(argv[1], L"service-status")) {
            SERVICE_STATUS_PROCESS current;
            ok = QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO, (BYTE *)&current, sizeof(current), &needed) != 0;
            if (ok) printf("Service state: %lu\nService process: %lu\n", current.dwCurrentState, current.dwProcessId);
        }
    }
done:
    if (!ok) fprintf(stderr, "Service operation failed: Windows error %lu\n", GetLastError());
    if (service) CloseServiceHandle(service);
    if (manager) CloseServiceHandle(manager);
    free(config);
    free(exe);
    free(root);
    free(cert);
    free(command);
    return ok ? 0 : 1;
}
