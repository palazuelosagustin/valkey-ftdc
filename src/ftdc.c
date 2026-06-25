#include "ftdc.h"

#include "collector.h"
#include "delta.h"
#include "redact.h"
#include "writer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>

FtdcState g_ftdc = {
    .config = {
        .enabled = 1,
        .path = "./diagnostic.data",
        .interval_ms = 1000,
        .max_file_bytes = 64LL * 1024 * 1024,
        .max_dir_bytes = 512LL * 1024 * 1024,
        .rotation_interval_sec = 3600,
        .redact = 1,
        .collect_host_stats = 1,
        .collect_slowlog = 0,
        .slowlog_redact = 1,
        .compression = 0,
        .delta_metrics = 0,
        .checkpoint_interval_ms = 60000,
    },
    .need_checkpoint = 1,
};

long long ftdc_now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

void ftdc_set_error(FtdcState *state, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(state->last_error, sizeof(state->last_error), fmt, ap);
    va_end(ap);
}

void ftdc_clear_error(FtdcState *state) {
    state->last_error[0] = '\0';
}

const char *ftdc_bool_name(int value) {
    return value ? "yes" : "no";
}

int ftdc_string_to_bool(const char *value, int *out) {
    if (strcasecmp(value, "yes") == 0 || strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0) {
        *out = 1;
        return VALKEYMODULE_OK;
    }
    if (strcasecmp(value, "no") == 0 || strcasecmp(value, "false") == 0 || strcmp(value, "0") == 0) {
        *out = 0;
        return VALKEYMODULE_OK;
    }
    return VALKEYMODULE_ERR;
}

static void ftdc_timer_callback(ValkeyModuleCtx *ctx, void *data) {
    char *json = NULL;
    size_t len = 0;
    (void)data;
    g_ftdc.timer_active = 0;
    if (!g_ftdc.config.enabled) {
        return;
    }
    if (ftdc_collect_sample(ctx, &g_ftdc, &json, &len) == VALKEYMODULE_OK) {
        ftdc_writer_append_sample(ctx, &g_ftdc, json, len);
        ftdc_collect_free(json);
    } else {
        ftdc_set_error(&g_ftdc, "sample collection failed");
    }
    if (g_ftdc.config.enabled) {
        ftdc_schedule_timer(ctx);
    }
}

void ftdc_schedule_timer(ValkeyModuleCtx *ctx) {
    if (!g_ftdc.config.enabled || g_ftdc.timer_active) {
        return;
    }
    g_ftdc.timer_id = ValkeyModule_CreateTimer(ctx, g_ftdc.config.interval_ms, ftdc_timer_callback, NULL);
    g_ftdc.timer_active = 1;
}

void ftdc_stop_timer(ValkeyModuleCtx *ctx) {
    void *data = NULL;
    if (!g_ftdc.timer_active) {
        return;
    }
    ValkeyModule_StopTimer(ctx, g_ftdc.timer_id, &data);
    g_ftdc.timer_active = 0;
}

static void reply_status(ValkeyModuleCtx *ctx) {
    ValkeyModule_ReplyWithMap(ctx, 12);
    ValkeyModule_ReplyWithSimpleString(ctx, "enabled");
    ValkeyModule_ReplyWithSimpleString(ctx, ftdc_bool_name(g_ftdc.config.enabled));
    ValkeyModule_ReplyWithSimpleString(ctx, "path");
    ValkeyModule_ReplyWithStringBuffer(ctx, g_ftdc.config.path, strlen(g_ftdc.config.path));
    ValkeyModule_ReplyWithSimpleString(ctx, "interval_ms");
    ValkeyModule_ReplyWithLongLong(ctx, g_ftdc.config.interval_ms);
    ValkeyModule_ReplyWithSimpleString(ctx, "samples_written");
    ValkeyModule_ReplyWithLongLong(ctx, (long long)g_ftdc.samples_written);
    ValkeyModule_ReplyWithSimpleString(ctx, "bytes_written");
    ValkeyModule_ReplyWithLongLong(ctx, (long long)g_ftdc.bytes_written);
    ValkeyModule_ReplyWithSimpleString(ctx, "current_file");
    ValkeyModule_ReplyWithStringBuffer(ctx, g_ftdc.current_file, strlen(g_ftdc.current_file));
    ValkeyModule_ReplyWithSimpleString(ctx, "last_sample_time_ms");
    ValkeyModule_ReplyWithLongLong(ctx, g_ftdc.last_sample_time_ms);
    ValkeyModule_ReplyWithSimpleString(ctx, "last_error");
    ValkeyModule_ReplyWithStringBuffer(ctx, g_ftdc.last_error, strlen(g_ftdc.last_error));
    ValkeyModule_ReplyWithSimpleString(ctx, "delta_metrics");
    ValkeyModule_ReplyWithSimpleString(ctx, ftdc_bool_name(g_ftdc.config.delta_metrics));
    ValkeyModule_ReplyWithSimpleString(ctx, "checkpoint_interval_ms");
    ValkeyModule_ReplyWithLongLong(ctx, g_ftdc.config.checkpoint_interval_ms);
    ValkeyModule_ReplyWithSimpleString(ctx, "checkpoints_written");
    ValkeyModule_ReplyWithLongLong(ctx, (long long)g_ftdc.checkpoints_written);
    ValkeyModule_ReplyWithSimpleString(ctx, "deltas_written");
    ValkeyModule_ReplyWithLongLong(ctx, (long long)g_ftdc.deltas_written);
}

static int command_status(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1) {
        return ValkeyModule_WrongArity(ctx);
    }
    reply_status(ctx);
    return VALKEYMODULE_OK;
}

static int sample_once(ValkeyModuleCtx *ctx, int persist) {
    char *json = NULL;
    size_t len = 0;
    if (ftdc_collect_sample(ctx, &g_ftdc, &json, &len) != VALKEYMODULE_OK) {
        ValkeyModule_ReplyWithError(ctx, "ERR sample collection failed");
        return VALKEYMODULE_OK;
    }
    if (persist) {
        if (ftdc_writer_append_sample(ctx, &g_ftdc, json, len) != VALKEYMODULE_OK) {
            ftdc_collect_free(json);
            ValkeyModule_ReplyWithError(ctx, "ERR sample write failed");
            return VALKEYMODULE_OK;
        }
    }
    ValkeyModule_ReplyWithStringBuffer(ctx, json, len);
    ftdc_collect_free(json);
    return VALKEYMODULE_OK;
}

static int command_sample(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1) {
        return ValkeyModule_WrongArity(ctx);
    }
    return sample_once(ctx, 0);
}

static int command_flush(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1) {
        return ValkeyModule_WrongArity(ctx);
    }
    if (ftdc_writer_flush(&g_ftdc) != VALKEYMODULE_OK) {
        ValkeyModule_ReplyWithError(ctx, "ERR flush failed");
        return VALKEYMODULE_OK;
    }
    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
    return VALKEYMODULE_OK;
}

static int command_rotate(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    VALKEYMODULE_NOT_USED(argv);
    if (argc != 1) {
        return ValkeyModule_WrongArity(ctx);
    }
    if (ftdc_writer_rotate(ctx, &g_ftdc) != VALKEYMODULE_OK) {
        ValkeyModule_ReplyWithError(ctx, "ERR rotate failed");
        return VALKEYMODULE_OK;
    }
    ValkeyModule_ReplyWithStringBuffer(ctx, g_ftdc.current_file, strlen(g_ftdc.current_file));
    return VALKEYMODULE_OK;
}

static int config_get_one(ValkeyModuleCtx *ctx, const char *name) {
    if (strcmp(name, "enabled") == 0) {
        return ValkeyModule_ReplyWithSimpleString(ctx, ftdc_bool_name(g_ftdc.config.enabled));
    } else if (strcmp(name, "path") == 0) {
        return ValkeyModule_ReplyWithStringBuffer(ctx, g_ftdc.config.path, strlen(g_ftdc.config.path));
    } else if (strcmp(name, "interval-ms") == 0) {
        return ValkeyModule_ReplyWithLongLong(ctx, g_ftdc.config.interval_ms);
    } else if (strcmp(name, "max-file-mb") == 0) {
        return ValkeyModule_ReplyWithLongLong(ctx, g_ftdc.config.max_file_bytes / (1024 * 1024));
    } else if (strcmp(name, "max-dir-mb") == 0) {
        return ValkeyModule_ReplyWithLongLong(ctx, g_ftdc.config.max_dir_bytes / (1024 * 1024));
    } else if (strcmp(name, "rotation-interval-sec") == 0) {
        return ValkeyModule_ReplyWithLongLong(ctx, g_ftdc.config.rotation_interval_sec);
    } else if (strcmp(name, "compression") == 0) {
        return ValkeyModule_ReplyWithSimpleString(ctx, "none");
    } else if (strcmp(name, "redact") == 0) {
        return ValkeyModule_ReplyWithSimpleString(ctx, ftdc_bool_name(g_ftdc.config.redact));
    } else if (strcmp(name, "collect-host-stats") == 0) {
        return ValkeyModule_ReplyWithSimpleString(ctx, ftdc_bool_name(g_ftdc.config.collect_host_stats));
    } else if (strcmp(name, "collect-slowlog") == 0) {
        return ValkeyModule_ReplyWithSimpleString(ctx, ftdc_bool_name(g_ftdc.config.collect_slowlog));
    } else if (strcmp(name, "slowlog-redact") == 0) {
        return ValkeyModule_ReplyWithSimpleString(ctx, ftdc_bool_name(g_ftdc.config.slowlog_redact));
    } else if (strcmp(name, "delta-metrics") == 0) {
        return ValkeyModule_ReplyWithSimpleString(ctx, ftdc_bool_name(g_ftdc.config.delta_metrics));
    } else if (strcmp(name, "checkpoint-interval-ms") == 0) {
        return ValkeyModule_ReplyWithLongLong(ctx, g_ftdc.config.checkpoint_interval_ms);
    }
    return ValkeyModule_ReplyWithError(ctx, "ERR unknown config key");
}

static int command_config(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    size_t op_len = 0;
    const char *op;
    if (argc < 2) {
        return ValkeyModule_WrongArity(ctx);
    }
    op = ValkeyModule_StringPtrLen(argv[1], &op_len);
    if (op_len == 3 && strncasecmp(op, "GET", 3) == 0) {
        if (argc == 3) {
            size_t name_len = 0;
            const char *name = ValkeyModule_StringPtrLen(argv[2], &name_len);
            char key[64];
            if (name_len >= sizeof(key)) {
                ValkeyModule_ReplyWithError(ctx, "ERR config key too long");
                return VALKEYMODULE_OK;
            }
            memcpy(key, name, name_len);
            key[name_len] = '\0';
            return config_get_one(ctx, key), VALKEYMODULE_OK;
        }
        if (argc != 2) {
            return ValkeyModule_WrongArity(ctx);
        }
        ValkeyModule_ReplyWithMap(ctx, 12);
        ValkeyModule_ReplyWithSimpleString(ctx, "enabled");
        config_get_one(ctx, "enabled");
        ValkeyModule_ReplyWithSimpleString(ctx, "path");
        config_get_one(ctx, "path");
        ValkeyModule_ReplyWithSimpleString(ctx, "interval-ms");
        config_get_one(ctx, "interval-ms");
        ValkeyModule_ReplyWithSimpleString(ctx, "max-file-mb");
        config_get_one(ctx, "max-file-mb");
        ValkeyModule_ReplyWithSimpleString(ctx, "max-dir-mb");
        config_get_one(ctx, "max-dir-mb");
        ValkeyModule_ReplyWithSimpleString(ctx, "rotation-interval-sec");
        config_get_one(ctx, "rotation-interval-sec");
        ValkeyModule_ReplyWithSimpleString(ctx, "compression");
        config_get_one(ctx, "compression");
        ValkeyModule_ReplyWithSimpleString(ctx, "redact");
        config_get_one(ctx, "redact");
        ValkeyModule_ReplyWithSimpleString(ctx, "collect-host-stats");
        config_get_one(ctx, "collect-host-stats");
        ValkeyModule_ReplyWithSimpleString(ctx, "collect-slowlog");
        config_get_one(ctx, "collect-slowlog");
        ValkeyModule_ReplyWithSimpleString(ctx, "delta-metrics");
        config_get_one(ctx, "delta-metrics");
        ValkeyModule_ReplyWithSimpleString(ctx, "checkpoint-interval-ms");
        config_get_one(ctx, "checkpoint-interval-ms");
        return VALKEYMODULE_OK;
    }
    if (op_len == 3 && strncasecmp(op, "SET", 3) == 0) {
        size_t key_len = 0, value_len = 0;
        const char *key_ptr;
        const char *value_ptr;
        char key[64];
        char value[PATH_MAX];
        long long ll;
        int boolv;
        if (argc != 4) {
            return ValkeyModule_WrongArity(ctx);
        }
        key_ptr = ValkeyModule_StringPtrLen(argv[2], &key_len);
        value_ptr = ValkeyModule_StringPtrLen(argv[3], &value_len);
        if (key_len >= sizeof(key) || value_len >= sizeof(value)) {
            ValkeyModule_ReplyWithError(ctx, "ERR argument too long");
            return VALKEYMODULE_OK;
        }
        memcpy(key, key_ptr, key_len);
        key[key_len] = '\0';
        memcpy(value, value_ptr, value_len);
        value[value_len] = '\0';

        if (strcmp(key, "enabled") == 0) {
            if (ftdc_string_to_bool(value, &boolv) != VALKEYMODULE_OK) goto bad_value;
            g_ftdc.config.enabled = boolv;
            if (boolv) ftdc_schedule_timer(ctx); else ftdc_stop_timer(ctx);
        } else if (strcmp(key, "path") == 0) {
            snprintf(g_ftdc.config.path, sizeof(g_ftdc.config.path), "%s", value);
            ftdc_delta_reset_segment(&g_ftdc);
            ftdc_writer_rotate(ctx, &g_ftdc);
        } else if (strcmp(key, "interval-ms") == 0) {
            if (ValkeyModule_StringToLongLong(argv[3], &ll) != VALKEYMODULE_OK || ll < 100) goto bad_value;
            g_ftdc.config.interval_ms = ll;
            ftdc_stop_timer(ctx);
            ftdc_schedule_timer(ctx);
        } else if (strcmp(key, "max-file-mb") == 0) {
            if (ValkeyModule_StringToLongLong(argv[3], &ll) != VALKEYMODULE_OK || ll < 1) goto bad_value;
            g_ftdc.config.max_file_bytes = ll * 1024 * 1024;
        } else if (strcmp(key, "max-dir-mb") == 0) {
            if (ValkeyModule_StringToLongLong(argv[3], &ll) != VALKEYMODULE_OK || ll < 1) goto bad_value;
            g_ftdc.config.max_dir_bytes = ll * 1024 * 1024;
        } else if (strcmp(key, "rotation-interval-sec") == 0) {
            if (ValkeyModule_StringToLongLong(argv[3], &ll) != VALKEYMODULE_OK || ll < 1) goto bad_value;
            g_ftdc.config.rotation_interval_sec = ll;
        } else if (strcmp(key, "redact") == 0) {
            if (ftdc_string_to_bool(value, &boolv) != VALKEYMODULE_OK) goto bad_value;
            g_ftdc.config.redact = boolv;
        } else if (strcmp(key, "collect-host-stats") == 0) {
            if (ftdc_string_to_bool(value, &boolv) != VALKEYMODULE_OK) goto bad_value;
            g_ftdc.config.collect_host_stats = boolv;
        } else if (strcmp(key, "collect-slowlog") == 0) {
            if (ftdc_string_to_bool(value, &boolv) != VALKEYMODULE_OK) goto bad_value;
            g_ftdc.config.collect_slowlog = boolv;
        } else if (strcmp(key, "slowlog-redact") == 0) {
            if (ftdc_string_to_bool(value, &boolv) != VALKEYMODULE_OK) goto bad_value;
            g_ftdc.config.slowlog_redact = boolv;
        } else if (strcmp(key, "compression") == 0) {
            if (strcmp(value, "none") != 0) goto bad_value;
        } else if (strcmp(key, "delta-metrics") == 0) {
            if (ftdc_string_to_bool(value, &boolv) != VALKEYMODULE_OK) goto bad_value;
            g_ftdc.config.delta_metrics = boolv;
            ftdc_delta_reset_segment(&g_ftdc);
        } else if (strcmp(key, "checkpoint-interval-ms") == 0) {
            if (ValkeyModule_StringToLongLong(argv[3], &ll) != VALKEYMODULE_OK || ll < 1000) goto bad_value;
            g_ftdc.config.checkpoint_interval_ms = ll;
        } else {
            ValkeyModule_ReplyWithError(ctx, "ERR unknown config key");
            return VALKEYMODULE_OK;
        }
        ValkeyModule_ReplyWithSimpleString(ctx, "OK");
        return VALKEYMODULE_OK;
bad_value:
        ValkeyModule_ReplyWithError(ctx, "ERR invalid config value");
        return VALKEYMODULE_OK;
    }
    ValkeyModule_ReplyWithError(ctx, "ERR expected GET or SET");
    return VALKEYMODULE_OK;
}

static int parse_load_args(ValkeyModuleString **argv, int argc) {
    int i;
    for (i = 0; i + 1 < argc; i += 2) {
        size_t key_len = 0, value_len = 0;
        const char *key_ptr = ValkeyModule_StringPtrLen(argv[i], &key_len);
        const char *value_ptr = ValkeyModule_StringPtrLen(argv[i + 1], &value_len);
        char key[64];
        char value[PATH_MAX];
        int boolv;
        long long ll;
        if (key_len >= sizeof(key) || value_len >= sizeof(value)) {
            return VALKEYMODULE_ERR;
        }
        memcpy(key, key_ptr, key_len);
        key[key_len] = '\0';
        memcpy(value, value_ptr, value_len);
        value[value_len] = '\0';
        if (strcmp(key, "path") == 0) {
            snprintf(g_ftdc.config.path, sizeof(g_ftdc.config.path), "%s", value);
        } else if (strcmp(key, "interval-ms") == 0) {
            if (ValkeyModule_StringToLongLong(argv[i + 1], &ll) != VALKEYMODULE_OK) return VALKEYMODULE_ERR;
            g_ftdc.config.interval_ms = ll;
        } else if (strcmp(key, "max-file-mb") == 0) {
            if (ValkeyModule_StringToLongLong(argv[i + 1], &ll) != VALKEYMODULE_OK) return VALKEYMODULE_ERR;
            g_ftdc.config.max_file_bytes = ll * 1024 * 1024;
        } else if (strcmp(key, "max-dir-mb") == 0) {
            if (ValkeyModule_StringToLongLong(argv[i + 1], &ll) != VALKEYMODULE_OK) return VALKEYMODULE_ERR;
            g_ftdc.config.max_dir_bytes = ll * 1024 * 1024;
        } else if (strcmp(key, "rotation-interval-sec") == 0) {
            if (ValkeyModule_StringToLongLong(argv[i + 1], &ll) != VALKEYMODULE_OK) return VALKEYMODULE_ERR;
            g_ftdc.config.rotation_interval_sec = ll;
        } else if (strcmp(key, "enabled") == 0) {
            if (ftdc_string_to_bool(value, &boolv) != VALKEYMODULE_OK) return VALKEYMODULE_ERR;
            g_ftdc.config.enabled = boolv;
        } else if (strcmp(key, "collect-host-stats") == 0) {
            if (ftdc_string_to_bool(value, &boolv) != VALKEYMODULE_OK) return VALKEYMODULE_ERR;
            g_ftdc.config.collect_host_stats = boolv;
        } else if (strcmp(key, "collect-slowlog") == 0) {
            if (ftdc_string_to_bool(value, &boolv) != VALKEYMODULE_OK) return VALKEYMODULE_ERR;
            g_ftdc.config.collect_slowlog = boolv;
        } else if (strcmp(key, "slowlog-redact") == 0) {
            if (ftdc_string_to_bool(value, &boolv) != VALKEYMODULE_OK) return VALKEYMODULE_ERR;
            g_ftdc.config.slowlog_redact = boolv;
        } else if (strcmp(key, "compression") == 0) {
            if (strcmp(value, "none") != 0) return VALKEYMODULE_ERR;
        } else if (strcmp(key, "redact") == 0) {
            if (ftdc_string_to_bool(value, &boolv) != VALKEYMODULE_OK) return VALKEYMODULE_ERR;
            g_ftdc.config.redact = boolv;
        } else if (strcmp(key, "delta-metrics") == 0) {
            if (ftdc_string_to_bool(value, &boolv) != VALKEYMODULE_OK) return VALKEYMODULE_ERR;
            g_ftdc.config.delta_metrics = boolv;
        } else if (strcmp(key, "checkpoint-interval-ms") == 0) {
            if (ValkeyModule_StringToLongLong(argv[i + 1], &ll) != VALKEYMODULE_OK) return VALKEYMODULE_ERR;
            g_ftdc.config.checkpoint_interval_ms = ll;
        } else {
            return VALKEYMODULE_ERR;
        }
    }
    return (argc % 2 == 0) ? VALKEYMODULE_OK : VALKEYMODULE_ERR;
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (ValkeyModule_Init(ctx, FTDC_MODULE_NAME, 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }
    ValkeyModule_AutoMemory(ctx);
    if (parse_load_args(argv, argc) != VALKEYMODULE_OK) {
        ValkeyModule_Log(ctx, "warning", "invalid ftdc arguments");
        return VALKEYMODULE_ERR;
    }
    if (ValkeyModule_CreateCommand(ctx, "FTDC.STATUS", command_status, "readonly", 0, 0, 0) == VALKEYMODULE_ERR ||
        ValkeyModule_CreateCommand(ctx, "FTDC.SAMPLE", command_sample, "readonly", 0, 0, 0) == VALKEYMODULE_ERR ||
        ValkeyModule_CreateCommand(ctx, "FTDC.FLUSH", command_flush, "", 0, 0, 0) == VALKEYMODULE_ERR ||
        ValkeyModule_CreateCommand(ctx, "FTDC.ROTATE", command_rotate, "", 0, 0, 0) == VALKEYMODULE_ERR ||
        ValkeyModule_CreateCommand(ctx, "FTDC.CONFIG", command_config, "", 0, 0, 0) == VALKEYMODULE_ERR) {
        return VALKEYMODULE_ERR;
    }
    if (g_ftdc.config.enabled) {
        ftdc_schedule_timer(ctx);
    }
    return VALKEYMODULE_OK;
}
