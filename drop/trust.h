#ifndef DROP_TRUST_H
#define DROP_TRUST_H
#include "drop_common.h"
#include <wincrypt.h>
typedef struct drop_trust {
    HCERTSTORE roots;
    HCERTCHAINENGINE engine;
    WCHAR host[256];
    LONG rejected, closed;
    int checked;
    HANDLE completed,closing;
    DWORD completion_status,operation_error,available;
} drop_trust;
int drop_trust_prepare(drop_trust *trust, HINTERNET request, const WCHAR *host, const WCHAR *ca_file);
void drop_trust_free(drop_trust *trust);
int drop_trust_async(drop_trust *trust,HINTERNET request);
void drop_trust_begin(drop_trust *trust);
int drop_trust_wait(drop_trust *trust,HINTERNET request,DWORD expected,HANDLE cancel);
void drop_trust_close(drop_trust *trust,HINTERNET request);
#endif
