#include "server.h"
#include "cloud.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void usage(void) {
    puts("Drop server " DROP_VERSION " (development)\n"
        "  dropserver init --data DIRECTORY\n"
        "  dropserver account-create --data DIRECTORY --login NAME [--display NAME]\n"
        "  dropserver serve --data DIRECTORY --certificate PEM [--listen 127.0.0.1:8443]\n"
        "    [--config FILE] [--backup-directory DIRECTORY] [--backup-seconds SECONDS]\n"
        "  dropserver account-enable|account-disable|account-reset --data DIRECTORY --login NAME\n"
        "  dropserver device-revoke --data DIRECTORY --login NAME --device ID\n"
        "  dropserver quota-set --data DIRECTORY --login NAME --bytes COUNT\n"
        "  dropserver storage-status|cleanup|runtime-machine --data DIRECTORY\n"
        "  dropserver recovery-export|backup --data DIRECTORY --target PATH\n"
        "  dropserver backup-trigger --data DIRECTORY\n"
        "  dropserver restore --data NEW_DIRECTORY --backup DIRECTORY --package FILE\n"
        "  dropserver service-install --name NAME --data DIRECTORY --certificate PEM [--listen HOST:PORT]\n"
        "  dropserver service-start|service-stop|service-status|service-uninstall --name NAME\n"
        "  dropserver drive-authorize --data DIRECTORY --oauth-client DESKTOP_JSON\n"
        "  dropserver drive-upload --data DIRECTORY --backup COMPLETE_DIRECTORY\n"
        "  dropserver drive-list|drive-status --data DIRECTORY\n"
        "  dropserver drive-download --data DIRECTORY --backup-id ID --target DIRECTORY\n"
        "  dropserver drive-delete --data DIRECTORY --backup-id ID\n"
        "Passwords are entered only at the local masked console. No public admin endpoint.");
}
static const WCHAR *argument(int argc, WCHAR **argv, const WCHAR *key) {
    int i; for (i = 2; i + 1 < argc; i += 2) if (!wcscmp(argv[i], key)) return argv[i+1]; return NULL;
}
int drop_server_command(int argc, WCHAR **argv) {
    drop_server server; const WCHAR *directory, *parameter; char login[256], display[512], password[4096], certificate[131072], listen[256];
    int result = 0, i, j;
    if (!drop_init()) return 1;
    if (argc == 2 && !wcscmp(argv[1], L"--version")) { puts(DROP_VERSION); return 0; }
#if DROP_DIAGNOSTICS
    if(argc==3 && !wcscmp(argv[1],L"--test-backup")) return drop_backup_test(argv[2])?0:1;
    if (argc == 3 && !wcscmp(argv[1], L"--self-test")) return drop_database_test(argv[2]) && drop_migration_test(argv[2]) && drop_maintenance_test(argv[2]) && drop_backup_test(argv[2]) ? 0 : 1;
#endif
    if(!DROP_DIAGNOSTICS && argc>1 && !wcsncmp(argv[1],L"--",2)) return 2;
    if (argc < 4 || argc % 2 != 0 || !(directory = argument(argc, argv, L"--data"))) { usage(); return 2; }
    if(argc==4 && !wcscmp(argv[1],L"backup-trigger")) { result=drop_backup_trigger(directory); puts(result?"Backup requested; completion is reported separately by the running server.":"Backup request failed; check server backup configuration and local permission."); return result?0:1; }
    if(!wcsncmp(argv[1],L"drive-",6)
#if DROP_DIAGNOSTICS
        || !wcscmp(argv[1],L"--test-drive-authorize")
#endif
    ) {
        if(wcscmp(argv[1],L"drive-authorize") && wcscmp(argv[1],L"drive-upload") && wcscmp(argv[1],L"drive-download") && wcscmp(argv[1],L"drive-delete") && wcscmp(argv[1],L"drive-list") && wcscmp(argv[1],L"drive-status")
#if DROP_DIAGNOSTICS
            && wcscmp(argv[1],L"--test-drive-authorize")
#endif
        ) return 2;
        for(i=2;i+1<argc;i+=2) {
            int allowed=!wcscmp(argv[i],L"--data") || (!wcscmp(argv[1],L"drive-authorize") && !wcscmp(argv[i],L"--oauth-client")) ||
                (!wcscmp(argv[1],L"drive-upload") && !wcscmp(argv[i],L"--backup")) ||
                ((!wcscmp(argv[1],L"drive-download") || !wcscmp(argv[1],L"drive-delete")) && !wcscmp(argv[i],L"--backup-id")) ||
                (!wcscmp(argv[1],L"drive-download") && !wcscmp(argv[i],L"--target"));
#if DROP_DIAGNOSTICS
            allowed|=!wcscmp(argv[1],L"--test-drive-authorize") && (!wcscmp(argv[i],L"--origin") || !wcscmp(argv[i],L"--ca"));
#endif
            if(!allowed) return 2; for(j=2;j<i;j+=2) if(!wcscmp(argv[i],argv[j])) return 2;
        }
        if(!drop_server_open(&server,directory,0)) return 1; result=drop_cloud_command(&server,argc,argv); drop_server_close(&server); return result?0:1;
    }
    if (wcscmp(argv[1], L"init") && wcscmp(argv[1], L"account-create") && wcscmp(argv[1], L"serve") && wcscmp(argv[1],L"service-run") && wcscmp(argv[1],L"storage-status") && wcscmp(argv[1],L"quota-set") && wcscmp(argv[1],L"cleanup") && wcscmp(argv[1],L"runtime-machine") && wcscmp(argv[1],L"recovery-export") && wcscmp(argv[1],L"backup") && wcscmp(argv[1],L"restore") && wcscmp(argv[1],L"--test-recovery-export") && wcscmp(argv[1],L"--test-restore") && wcscmp(argv[1],L"account-enable") && wcscmp(argv[1],L"account-disable") && wcscmp(argv[1],L"account-reset") && wcscmp(argv[1],L"device-revoke")) { usage(); return 2; }
    for (i = 2; i + 1 < argc; i += 2) {
        int allowed = !wcscmp(argv[i], L"--data") ||
            (!wcscmp(argv[1], L"account-create") && (!wcscmp(argv[i], L"--login") || !wcscmp(argv[i], L"--display"))) ||
            (!wcscmp(argv[1],L"quota-set") && (!wcscmp(argv[i],L"--login") || !wcscmp(argv[i],L"--bytes"))) ||
            ((!wcscmp(argv[1],L"account-enable") || !wcscmp(argv[1],L"account-disable") || !wcscmp(argv[1],L"account-reset") || !wcscmp(argv[1],L"device-revoke")) && (!wcscmp(argv[i],L"--login") || (!wcscmp(argv[1],L"device-revoke") && !wcscmp(argv[i],L"--device")))) ||
            ((!wcscmp(argv[1],L"recovery-export") || !wcscmp(argv[1],L"--test-recovery-export") || !wcscmp(argv[1],L"backup")) && !wcscmp(argv[i],L"--target")) ||
            ((!wcscmp(argv[1],L"restore") || !wcscmp(argv[1],L"--test-restore")) && (!wcscmp(argv[i],L"--backup") || !wcscmp(argv[i],L"--package"))) ||
            ((!wcscmp(argv[1], L"serve") || !wcscmp(argv[1],L"service-run")) && (!wcscmp(argv[i], L"--listen") || !wcscmp(argv[i], L"--certificate") || !wcscmp(argv[i],L"--config") || !wcscmp(argv[i],L"--backup-directory") || !wcscmp(argv[i],L"--backup-seconds") || (!wcscmp(argv[1],L"service-run") && !wcscmp(argv[i],L"--name"))));
        if (!allowed) { usage(); return 2; }
        for (j = 2; j < i; j += 2) if (!wcscmp(argv[i], argv[j])) { usage(); return 2; }
    }
    if(!wcscmp(argv[1],L"restore") || !wcscmp(argv[1],L"--test-restore")) {
        const WCHAR *backup=argument(argc,argv,L"--backup"),*package=argument(argc,argv,L"--package");
#if DROP_DIAGNOSTICS
        if(!wcscmp(argv[1],L"--test-restore")) strcpy_s(password,sizeof(password),"independent-recovery-fixture-password-123");
        else
#endif
        if(!drop_password_read(L"Recovery password: ",password,sizeof(password))) return 1;
        result=backup && package && drop_backup_restore(backup,package,directory,password);
        sodium_memzero(password,sizeof(password)); puts(result?"Restore complete; server identity retained, sync generation changed.":"Restore failed; no server started."); return result?0:1;
    }
    if (!drop_server_open(&server, directory, !wcscmp(argv[1], L"init"))) {
        fputs("Cannot open encrypted server state: check directory, key identity, and whether another process owns it.\n", stderr); return 1;
    }
    if (!wcscmp(argv[1], L"init")) { puts("Encrypted server initialized. Runtime keys protected for this Windows user."); result = 1; }
    else if(!wcscmp(argv[1],L"runtime-machine")) { result=drop_runtime_machine(&server); if(result) puts("Runtime keys migrated to machine DPAPI; directory restricted to SYSTEM and administrators."); }
    else if(!wcscmp(argv[1],L"recovery-export") || !wcscmp(argv[1],L"--test-recovery-export")) {
#if DROP_DIAGNOSTICS
        if(!wcscmp(argv[1],L"--test-recovery-export")) strcpy_s(password,sizeof(password),"independent-recovery-fixture-password-123");
        else
#endif
        if(!drop_password_read(L"Independent recovery password (16+ UTF-8 bytes): ",password,sizeof(password))) goto cleanup;
        parameter=argument(argc,argv,L"--target"); result=parameter && drop_recovery_export(&server,parameter,password);
        sodium_memzero(password,sizeof(password)); if(result) puts("Protected portable recovery package created. Keep the password separately.");
    }
    else if(!wcscmp(argv[1],L"backup")) {
        char task[DROP_ID_CAP]={0}; parameter=argument(argc,argv,L"--target"); result=parameter && drop_backup_create(&server,parameter,task); if(result) printf("Encrypted backup complete: %s\n",task);
    }
    else if(!wcscmp(argv[1],L"account-enable") || !wcscmp(argv[1],L"account-disable") || !wcscmp(argv[1],L"account-reset") || !wcscmp(argv[1],L"device-revoke")) {
        parameter=argument(argc,argv,L"--login"); if(!parameter || !drop_to_utf8(parameter,login,sizeof(login))) goto cleanup;
        if(!wcscmp(argv[1],L"device-revoke")) {
            char device[DROP_ID_CAP]; parameter=argument(argc,argv,L"--device"); result=parameter && drop_to_utf8(parameter,device,sizeof(device)) && drop_admin_revoke(&server,login,device);
        } else if(!wcscmp(argv[1],L"account-reset")) {
            result=drop_password_read(L"New account password (12+ UTF-8 bytes): ",password,sizeof(password)) && drop_admin_account(&server,login,1,password); sodium_memzero(password,sizeof(password));
        } else result=drop_admin_account(&server,login,!wcscmp(argv[1],L"account-enable"),NULL);
        if(result) puts("Account/device management committed; existing history retained.");
    }
    else if(!wcscmp(argv[1],L"cleanup")) { result=drop_upload_cleanup(&server,(uint64_t)time(NULL))==DROP_OK; if(result) puts("Expired unreferenced uploads processed; history retained."); }
    else if(!wcscmp(argv[1],L"storage-status")) {
        sqlite3_stmt *statement=NULL;
        if(sqlite3_prepare_v2(server.database,"SELECT count(*),coalesce(sum(compressed=1),0) FROM messages",-1,&statement,NULL)==SQLITE_OK && sqlite3_step(statement)==SQLITE_ROW) {
            printf("Text records: %lld\nText zstd: %lld\n",sqlite3_column_int64(statement,0),sqlite3_column_int64(statement,1)); result=1;
        } sqlite3_finalize(statement);
    }
    else if(!wcscmp(argv[1],L"quota-set")) {
        sqlite3_stmt *statement=NULL; char number[32]; uint64_t bytes;
        parameter=argument(argc,argv,L"--bytes");
        if(!parameter || !drop_to_utf8(parameter,number,sizeof(number)) || !drop_u64_parse(number,&bytes) || bytes>INT64_MAX) goto cleanup;
        parameter=argument(argc,argv,L"--login"); if(!parameter || !drop_to_utf8(parameter,login,sizeof(login))) goto cleanup;
        if(sqlite3_prepare_v2(server.database,"UPDATE accounts SET quota_bytes=?1 WHERE login=?2",-1,&statement,NULL)==SQLITE_OK) {
            sqlite3_bind_int64(statement,1,(sqlite3_int64)bytes); sqlite3_bind_text(statement,2,login,-1,SQLITE_TRANSIENT);
            result=sqlite3_step(statement)==SQLITE_DONE && sqlite3_changes(server.database)==1;
        } sqlite3_finalize(statement); if(result) puts("Account quota updated; existing history retained.");
    }
    else if (!wcscmp(argv[1], L"account-create")) {
        parameter = argument(argc, argv, L"--login");
        if (parameter && drop_to_utf8(parameter, login, sizeof(login))) {
            parameter = argument(argc, argv, L"--display");
            if (!parameter) strcpy_s(display, sizeof(display), login);
            else if (!drop_to_utf8(parameter, display, sizeof(display))) goto cleanup;
            if (drop_password_read(L"Account password (12+ UTF-8 bytes): ", password, sizeof(password))) {
                result = drop_account_create(&server, login, display, password); sodium_memzero(password, sizeof(password));
                if (result) puts("Account created.");
            }
        }
    } else {
        parameter=argument(argc,argv,L"--backup-directory");
        if(parameter) { DWORD n=GetFullPathNameW(parameter,32768,server.backup_directory,NULL); if(!n || n>=32768) goto cleanup; }
        parameter=argument(argc,argv,L"--backup-seconds");
        if(parameter) { char number[32]; uint64_t seconds; if(!*server.backup_directory || !drop_to_utf8(parameter,number,sizeof(number)) || !drop_u64_parse(number,&seconds) || seconds>604800 || (seconds && seconds<60)) goto cleanup; server.backup_seconds=(unsigned)seconds; }
        parameter=argument(argc,argv,L"--config");
        if(parameter) {
            drop_config *config=malloc(sizeof(*config));
            if(!config || !drop_config_load(parameter,config)) { free(config); goto cleanup; }
            server.use_zstd=config->use_zstd; server.zstd_level=config->zstd_level; server.connection_timeout_seconds=config->connection_timeout_seconds; free(config);
        }
        parameter = argument(argc, argv, L"--certificate");
        if (!parameter || !drop_to_utf8(parameter, certificate, sizeof(certificate))) goto cleanup;
        parameter = argument(argc, argv, L"--listen");
        if (!parameter) strcpy_s(listen, sizeof(listen), "127.0.0.1:8443");
        else if (!drop_to_utf8(parameter, listen, sizeof(listen))) goto cleanup;
        result = drop_server_run(&server, listen, certificate);
    }
cleanup:
    drop_server_close(&server); if (!result) fputs("Operation failed.\n", stderr); return result ? 0 : 1;
}
int wmain(int argc,WCHAR **argv) {
    if(!drop_init()) return 1;
    if(argc>1 && !wcsncmp(argv[1],L"service-",8)) return drop_service_command(argc,argv);
    return drop_server_command(argc,argv);
}
