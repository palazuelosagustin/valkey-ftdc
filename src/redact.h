#ifndef VALKEY_FTDC_REDACT_H
#define VALKEY_FTDC_REDACT_H

int ftdc_is_sensitive_config_name(const char *name);
const char *ftdc_redact_slowlog_arg(const char *arg);

#endif
