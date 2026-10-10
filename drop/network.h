#ifndef DROP_NETWORK_H
#define DROP_NETWORK_H
#include "drop_common.h"
drop_error drop_http_json(const WCHAR *url, const WCHAR *method, const WCHAR *path,
    const char *token, const cJSON *body, cJSON **response, DWORD *status);
int drop_network_probe(const WCHAR *url);
#endif
