#ifndef VALKEY_FTDC_H
#define VALKEY_FTDC_H

#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#include "valkeymodule.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define FTDC_MODULE_NAME "ftdc"
#define FTDC_FILE_MAGIC "VKFTDC1\n"
#define FTDC_METADATA_VERSION 1

typedef struct FtdcConfig {
    int enabled;
    char path[PATH_MAX];
    long long interval_ms;
    long long max_file_bytes;
    long long max_dir_bytes;
    long long rotation_interval_sec;
    int redact;
    int collect_host_stats;
    int collect_slowlog;
    int slowlog_redact;
    int compression;
} FtdcConfig;

typedef struct FtdcState {
    FtdcConfig config;
    FILE *fp;
    char current_file[PATH_MAX];
    long long current_file_bytes;
    long long current_file_opened_ms;
    uint64_t samples_written;
    uint64_t bytes_written;
    long long last_sample_time_ms;
    char last_error[256];
    ValkeyModuleTimerID timer_id;
    int timer_active;
} FtdcState;

extern FtdcState g_ftdc;

void ftdc_set_error(FtdcState *state, const char *fmt, ...);
void ftdc_clear_error(FtdcState *state);
long long ftdc_now_ms(void);
int ftdc_string_to_bool(const char *value, int *out);
const char *ftdc_bool_name(int value);
void ftdc_schedule_timer(ValkeyModuleCtx *ctx);
void ftdc_stop_timer(ValkeyModuleCtx *ctx);

#endif
