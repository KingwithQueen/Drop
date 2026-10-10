#include "server.h"
#include <winsvc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SERVICE_STATUS_HANDLE service_handle;
static SERVICE_STATUS service_status;
static HANDLE service_stop_event;
static SRWLOCK event_lock=SRWLOCK_INIT;
static int saved_argc;
static WCHAR **saved_argv;
static LONG stopping;
static const WCHAR *option(int argc,WCHAR **argv,const WCHAR *key) {
    int i; for(i=2;i+1<argc;i+=2) if(!wcscmp(argv[i],key)) return argv[i+1]; return NULL;
}
static void report(DWORD state,DWORD error) {
    service_status.dwServiceType=SERVICE_WIN32_OWN_PROCESS; service_status.dwCurrentState=state;
    service_status.dwControlsAccepted=state==SERVICE_RUNNING?SERVICE_ACCEPT_STOP|SERVICE_ACCEPT_SHUTDOWN:0;
    service_status.dwWin32ExitCode=error; service_status.dwWaitHint=60000;
    service_status.dwCheckPoint=state==SERVICE_START_PENDING || state==SERVICE_STOP_PENDING?service_status.dwCheckPoint+1:0;
    SetServiceStatus(service_handle,&service_status);
}
static DWORD WINAPI control(DWORD event,DWORD type,LPVOID data,LPVOID context) {
    (void)type; (void)data; (void)context;
    if(event==SERVICE_CONTROL_STOP || event==SERVICE_CONTROL_SHUTDOWN) {
        InterlockedExchange(&stopping,1); report(SERVICE_STOP_PENDING,NO_ERROR);
        AcquireSRWLockShared(&event_lock); if(service_stop_event) SetEvent(service_stop_event); ReleaseSRWLockShared(&event_lock);
    } else if(event==SERVICE_CONTROL_INTERROGATE) SetServiceStatus(service_handle,&service_status);
    return NO_ERROR;
}
void drop_service_ready(drop_server *server) {
    if(!service_handle) return;
    AcquireSRWLockExclusive(&event_lock);
    DuplicateHandle(GetCurrentProcess(),server->stop_event,GetCurrentProcess(),&service_stop_event,EVENT_MODIFY_STATE,FALSE,0);
    ReleaseSRWLockExclusive(&event_lock);
    if(InterlockedCompareExchange(&stopping,0,0)) SetEvent(server->stop_event); else report(SERVICE_RUNNING,NO_ERROR);
}
static void WINAPI service_main(DWORD argc,LPWSTR *argv) {
    const WCHAR *name=option(saved_argc,saved_argv,L"--name"); int result;
    (void)argc; (void)argv;
    service_handle=RegisterServiceCtrlHandlerExW(name,control,NULL); if(!service_handle) return;
    report(SERVICE_START_PENDING,NO_ERROR);
    result=drop_server_command(saved_argc,saved_argv);
    AcquireSRWLockExclusive(&event_lock); if(service_stop_event) CloseHandle(service_stop_event); service_stop_event=NULL; ReleaseSRWLockExclusive(&event_lock);
    report(SERVICE_STOPPED,result?ERROR_SERVICE_SPECIFIC_ERROR:NO_ERROR);
}
static int name_valid(const WCHAR *name) {
    size_t i,n=name?wcslen(name):0; if(n<3 || n>80) return 0;
    for(i=0;i<n;i++) if(!((name[i]>=L'A' && name[i]<=L'Z') || (name[i]>=L'a' && name[i]<=L'z') || (name[i]>=L'0' && name[i]<=L'9') || name[i]==L'-' || name[i]==L'_')) return 0;
    return 1;
}
static int absolute(const WCHAR *source,WCHAR *target) {
    DWORD n; if(!source || wcspbrk(source,L"\"\r\n")) return 0;
    n=GetFullPathNameW(source,32768,target,NULL); return n && n<32768;
}
static int wait_state(SC_HANDLE service,DWORD state) {
    SERVICE_STATUS_PROCESS status; DWORD needed; ULONGLONG end=GetTickCount64()+60000;
    do {
        if(!QueryServiceStatusEx(service,SC_STATUS_PROCESS_INFO,(BYTE *)&status,sizeof(status),&needed)) return 0;
        if(status.dwCurrentState==state) return 1;
        if(state==SERVICE_RUNNING && status.dwCurrentState==SERVICE_STOPPED) return 0;
        Sleep(100);
    } while(GetTickCount64()<end); return 0;
}
int drop_service_command(int argc,WCHAR **argv) {
    const WCHAR *name=option(argc,argv,L"--name"),*directory=option(argc,argv,L"--data"),*certificate=option(argc,argv,L"--certificate"),*listen=option(argc,argv,L"--listen");
    WCHAR *exe=malloc(32768*sizeof(WCHAR)),*root=malloc(32768*sizeof(WCHAR)),*cert=malloc(32768*sizeof(WCHAR)),*command=malloc(100000*sizeof(WCHAR));
    SC_HANDLE manager=NULL,service=NULL; SERVICE_STATUS status; QUERY_SERVICE_CONFIGW *config=NULL; DWORD needed=0; int ok=0,i;
    if(!exe || !root || !cert || !command || argc%2 || !name_valid(name) || !GetModuleFileNameW(NULL,exe,32768)) goto done;
    if(!wcscmp(argv[1],L"service-run")) {
        SERVICE_TABLE_ENTRYW table[2]={{(WCHAR *)name,service_main},{NULL,NULL}};
        saved_argc=argc; saved_argv=argv; ok=StartServiceCtrlDispatcherW(table)!=0; goto done;
    }
    for(i=2;i+1<argc;i+=2) {
        int j;
        if(wcscmp(argv[i],L"--name") && (wcscmp(argv[1],L"service-install") || (wcscmp(argv[i],L"--data") && wcscmp(argv[i],L"--certificate") && wcscmp(argv[i],L"--listen") && wcscmp(argv[i],L"--backup-directory") && wcscmp(argv[i],L"--backup-seconds") && wcscmp(argv[i],L"--config")))) goto done;
        for(j=2;j<i;j+=2) if(!wcscmp(argv[j],argv[i])) goto done;
    }
    manager=OpenSCManagerW(NULL,NULL,SC_MANAGER_CONNECT|(!wcscmp(argv[1],L"service-install")?SC_MANAGER_CREATE_SERVICE:0)); if(!manager) goto done;
    if(!wcscmp(argv[1],L"service-install")) {
        SERVICE_FAILURE_ACTIONSW failure={0}; SC_ACTION actions[3]={{SC_ACTION_RESTART,5000},{SC_ACTION_RESTART,30000},{SC_ACTION_NONE,0}};
        if(!absolute(directory,root) || !absolute(certificate,cert) || (listen && wcspbrk(listen,L"\"\r\n ")) ||
            swprintf_s(command,100000,L"\"%s\" service-run --name %s --data \"%s\" --certificate \"%s\" --listen %s",exe,name,root,cert,listen?listen:L"127.0.0.1:8443")<0) goto done;
        { WCHAR key[32768]; if(!drop_path_join(root,L"runtime-machine.dpapi",key,32768) || GetFileAttributesW(key)==INVALID_FILE_ATTRIBUTES) goto done; }
        for(i=2;i+1<argc;i+=2) {
            size_t used=wcslen(command);
            if(!wcscmp(argv[i],L"--backup-directory") || !wcscmp(argv[i],L"--config")) {
                if(!absolute(argv[i+1],root) || swprintf_s(command+used,100000-used,L" %s \"%s\"",argv[i],root)<0) goto done;
            } else if(!wcscmp(argv[i],L"--backup-seconds")) {
                char number[32]; uint64_t seconds;
                if(!option(argc,argv,L"--backup-directory") || !drop_to_utf8(argv[i+1],number,sizeof(number)) || !drop_u64_parse(number,&seconds) || seconds>604800 || (seconds && seconds<60) || swprintf_s(command+used,100000-used,L" --backup-seconds %s",argv[i+1])<0) goto done;
            }
        }
        if(wcslen(command)>32766) goto done;
        service=CreateServiceW(manager,name,name,SERVICE_CHANGE_CONFIG|SERVICE_QUERY_STATUS|SERVICE_START|DELETE,SERVICE_WIN32_OWN_PROCESS,SERVICE_AUTO_START,SERVICE_ERROR_NORMAL,command,NULL,NULL,NULL,NULL,NULL);
        if(!service) goto done;
        failure.dwResetPeriod=86400; failure.cActions=3; failure.lpsaActions=actions;
        ok=ChangeServiceConfig2W(service,SERVICE_CONFIG_FAILURE_ACTIONS,&failure)!=0;
        if(!ok) DeleteService(service);
    } else {
        DWORD access=SERVICE_QUERY_CONFIG|SERVICE_QUERY_STATUS;
        if(!wcscmp(argv[1],L"service-start")) access|=SERVICE_START;
        else if(!wcscmp(argv[1],L"service-stop")) access|=SERVICE_STOP;
        else if(!wcscmp(argv[1],L"service-uninstall")) access|=SERVICE_STOP|DELETE;
        service=OpenServiceW(manager,name,access); if(!service) goto done;
        QueryServiceConfigW(service,NULL,0,&needed); config=malloc(needed);
        if(!config || !QueryServiceConfigW(service,config,needed,&needed) || swprintf_s(command,100000,L"\"%s\" service-run ",exe)<0 || wcsncmp(config->lpBinaryPathName,command,wcslen(command))) goto done;
        if(!wcscmp(argv[1],L"service-start")) ok=(StartServiceW(service,0,NULL) || GetLastError()==ERROR_SERVICE_ALREADY_RUNNING) && wait_state(service,SERVICE_RUNNING);
        else if(!wcscmp(argv[1],L"service-stop") || !wcscmp(argv[1],L"service-uninstall")) {
            ok=(ControlService(service,SERVICE_CONTROL_STOP,&status) || GetLastError()==ERROR_SERVICE_NOT_ACTIVE) && wait_state(service,SERVICE_STOPPED);
            if(ok && !wcscmp(argv[1],L"service-uninstall")) ok=DeleteService(service)!=0;
        } else if(!wcscmp(argv[1],L"service-status")) {
            SERVICE_STATUS_PROCESS current;
            ok=QueryServiceStatusEx(service,SC_STATUS_PROCESS_INFO,(BYTE *)&current,sizeof(current),&needed)!=0;
            if(ok) printf("Service state: %lu\nService process: %lu\n",current.dwCurrentState,current.dwProcessId);
        }
    }
done:
    if(!ok) fprintf(stderr,"Service operation failed: Windows error %lu\n",GetLastError());
    if(service) CloseServiceHandle(service); if(manager) CloseServiceHandle(manager);
    free(config); free(exe); free(root); free(cert); free(command); return ok?0:1;
}
