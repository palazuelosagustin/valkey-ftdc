#include "redact.h"

#include <string.h>

static const char *k_sensitive_configs[] = {
    "requirepass",
    "masterauth",
    "aclfile",
    "tls-key-file",
    "tls-cert-file",
    "tls-ca-cert-file",
    NULL,
};

int ftdc_is_sensitive_config_name(const char *name) {
    size_t i;
    for (i = 0; k_sensitive_configs[i] != NULL; ++i) {
        if (strcmp(name, k_sensitive_configs[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

const char *ftdc_redact_slowlog_arg(const char *arg) {
    (void)arg;
    return "?";
}
