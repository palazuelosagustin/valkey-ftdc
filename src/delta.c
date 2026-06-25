#include "delta.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

typedef struct MetricState {
    char path[256];
    long long value;
} MetricState;

typedef struct DeltaStateInternal {
    MetricState *metrics;
    size_t len;
    size_t cap;
} DeltaStateInternal;

typedef struct Buffer {
    char *data;
    size_t len;
    size_t cap;
} Buffer;

typedef struct FlatSample {
    FtdcIdentity identity;
    MetricState *metrics;
    size_t len;
    size_t cap;
} FlatSample;

static int buf_reserve(Buffer *buf, size_t extra) {
    size_t need = buf->len + extra + 1;
    char *next;
    if (buf->cap >= need) {
        return 0;
    }
    while (buf->cap < need) {
        buf->cap = buf->cap == 0 ? 256 : buf->cap * 2;
    }
    next = realloc(buf->data, buf->cap);
    if (next == NULL) {
        return -1;
    }
    buf->data = next;
    return 0;
}

static int buf_append_n(Buffer *buf, const char *s, size_t n) {
    if (buf_reserve(buf, n) != 0) {
        return -1;
    }
    memcpy(buf->data + buf->len, s, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
    return 0;
}

static int buf_append(Buffer *buf, const char *s) {
    return buf_append_n(buf, s, strlen(s));
}

static int buf_printf(Buffer *buf, const char *fmt, ...) {
    va_list ap;
    va_list ap2;
    int needed;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0 || buf_reserve(buf, (size_t)needed) != 0) {
        va_end(ap2);
        return -1;
    }
    vsnprintf(buf->data + buf->len, buf->cap - buf->len, fmt, ap2);
    va_end(ap2);
    buf->len += (size_t)needed;
    return 0;
}

static void buf_free(Buffer *buf) {
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static void skip_ws(const char **p) {
    while (**p != '\0' && isspace((unsigned char)**p)) {
        ++(*p);
    }
}

static void copy_truncated(char *dst, size_t dst_len, const char *src) {
    size_t n;
    if (dst_len == 0) {
        return;
    }
    n = strlen(src);
    if (n >= dst_len) {
        n = dst_len - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int parse_json_string(const char **p, char *dst, size_t dst_len) {
    size_t len = 0;
    if (**p != '"') {
        return -1;
    }
    ++(*p);
    while (**p != '\0' && **p != '"') {
        char ch = **p;
        if (ch == '\\') {
            ++(*p);
            if (**p == '\0') {
                return -1;
            }
            ch = **p;
            switch (ch) {
                case '"':
                case '\\':
                case '/':
                    break;
                case 'b':
                    ch = '\b';
                    break;
                case 'f':
                    ch = '\f';
                    break;
                case 'n':
                    ch = '\n';
                    break;
                case 'r':
                    ch = '\r';
                    break;
                case 't':
                    ch = '\t';
                    break;
                default:
                    return -1;
            }
        }
        if (len + 1 >= dst_len) {
            return -1;
        }
        dst[len++] = ch;
        ++(*p);
    }
    if (**p != '"') {
        return -1;
    }
    ++(*p);
    dst[len] = '\0';
    return 0;
}

static int skip_json_value(const char **p);

static int skip_json_array(const char **p) {
    if (**p != '[') {
        return -1;
    }
    ++(*p);
    skip_ws(p);
    if (**p == ']') {
        ++(*p);
        return 0;
    }
    for (;;) {
        if (skip_json_value(p) != 0) {
            return -1;
        }
        skip_ws(p);
        if (**p == ']') {
            ++(*p);
            return 0;
        }
        if (**p != ',') {
            return -1;
        }
        ++(*p);
        skip_ws(p);
    }
}

static int skip_json_object(const char **p) {
    char tmp[256];
    if (**p != '{') {
        return -1;
    }
    ++(*p);
    skip_ws(p);
    if (**p == '}') {
        ++(*p);
        return 0;
    }
    for (;;) {
        if (parse_json_string(p, tmp, sizeof(tmp)) != 0) {
            return -1;
        }
        skip_ws(p);
        if (**p != ':') {
            return -1;
        }
        ++(*p);
        skip_ws(p);
        if (skip_json_value(p) != 0) {
            return -1;
        }
        skip_ws(p);
        if (**p == '}') {
            ++(*p);
            return 0;
        }
        if (**p != ',') {
            return -1;
        }
        ++(*p);
        skip_ws(p);
    }
}

static int skip_json_value(const char **p) {
    char tmp[256];
    skip_ws(p);
    if (**p == '"') {
        return parse_json_string(p, tmp, sizeof(tmp));
    }
    if (**p == '{') {
        return skip_json_object(p);
    }
    if (**p == '[') {
        return skip_json_array(p);
    }
    if (**p == '-' || isdigit((unsigned char)**p)) {
        char *end = NULL;
        (void)strtod(*p, &end);
        if (end == *p) {
            return -1;
        }
        *p = end;
        return 0;
    }
    if (strncmp(*p, "true", 4) == 0) {
        *p += 4;
        return 0;
    }
    if (strncmp(*p, "false", 5) == 0) {
        *p += 5;
        return 0;
    }
    if (strncmp(*p, "null", 4) == 0) {
        *p += 4;
        return 0;
    }
    return -1;
}

static int metric_is_monotonic(const char *path) {
    static const char *stats_paths[] = {
        "valkey.info.stats.total_commands_processed",
        "valkey.info.stats.total_connections_received",
        "valkey.info.stats.keyspace_hits",
        "valkey.info.stats.keyspace_misses",
        "valkey.info.stats.expired_keys",
        "valkey.info.stats.evicted_keys",
        "valkey.info.stats.rejected_connections",
    };
    size_t i;
    for (i = 0; i < sizeof(stats_paths) / sizeof(stats_paths[0]); ++i) {
        if (strcmp(path, stats_paths[i]) == 0) {
            return 1;
        }
    }
    if (strcmp(path, "valkey.info.replication.master_repl_offset") == 0) {
        return 1;
    }
    if (strncmp(path, "valkey.info.replication.slave", 29) == 0) {
        size_t len = strlen(path);
        return len > 7 && strcmp(path + len - 7, ".offset") == 0;
    }
    return 0;
}

static int metric_state_set(MetricState **items, size_t *len, size_t *cap, const char *path, long long value) {
    size_t i;
    for (i = 0; i < *len; ++i) {
        if (strcmp((*items)[i].path, path) == 0) {
            (*items)[i].value = value;
            return 0;
        }
    }
    if (*len == *cap) {
        size_t next_cap = *cap == 0 ? 16 : *cap * 2;
        MetricState *next = realloc(*items, next_cap * sizeof(**items));
        if (next == NULL) {
            return -1;
        }
        *items = next;
        *cap = next_cap;
    }
    copy_truncated((*items)[*len].path, sizeof((*items)[*len].path), path);
    (*items)[*len].value = value;
    *len += 1;
    return 0;
}

static int metric_state_get(MetricState *items, size_t len, const char *path, long long *value_out) {
    size_t i;
    for (i = 0; i < len; ++i) {
        if (strcmp(items[i].path, path) == 0) {
            *value_out = items[i].value;
            return 0;
        }
    }
    return -1;
}

static int flat_sample_add_metric(FlatSample *flat, const char *path, long long value) {
    if (!metric_is_monotonic(path)) {
        return 0;
    }
    return metric_state_set(&flat->metrics, &flat->len, &flat->cap, path, value);
}

static void flat_sample_note_string(FlatSample *flat, const char *path, const char *value) {
    if (strcmp(path, "valkey.info.server.run_id") == 0) {
        copy_truncated(flat->identity.run_id, sizeof(flat->identity.run_id), value);
        flat->identity.valid = 1;
    } else if (strcmp(path, "valkey.info.replication.role") == 0) {
        copy_truncated(flat->identity.role, sizeof(flat->identity.role), value);
    }
}

static void flat_sample_note_number(FlatSample *flat, const char *path, long long value) {
    if (strcmp(path, "ts_ms") == 0) {
        flat->identity.ts_ms = value;
        flat->identity.valid = 1;
    } else if (strcmp(path, "valkey.info.server.process_id") == 0) {
        flat->identity.process_id = value;
        flat->identity.valid = 1;
    } else if (strcmp(path, "valkey.info.server.uptime_in_seconds") == 0) {
        flat->identity.uptime_in_seconds = value;
        flat->identity.valid = 1;
    }
    (void)flat_sample_add_metric(flat, path, value);
}

static int parse_flat_object(const char **p, FlatSample *flat, char *path, size_t path_len, size_t path_cap);

static int parse_flat_value(const char **p, FlatSample *flat, char *path, size_t path_len, size_t path_cap) {
    char tmp[256];
    skip_ws(p);
    if (**p == '{') {
        return parse_flat_object(p, flat, path, path_len, path_cap);
    }
    if (**p == '[') {
        return skip_json_array(p);
    }
    if (**p == '"') {
        if (parse_json_string(p, tmp, sizeof(tmp)) != 0) {
            return -1;
        }
        flat_sample_note_string(flat, path, tmp);
        return 0;
    }
    if (**p == '-' || isdigit((unsigned char)**p)) {
        char *end = NULL;
        long long value = strtoll(*p, &end, 10);
        if (end == *p) {
            return -1;
        }
        while (*end != '\0' && (isdigit((unsigned char)*end) || *end == '.' || *end == 'e' || *end == 'E' || *end == '+' || *end == '-')) {
            if (*end == '.' || *end == 'e' || *end == 'E') {
                *p = end;
                return skip_json_value(p);
            }
            ++end;
        }
        *p = end;
        flat_sample_note_number(flat, path, value);
        return 0;
    }
    if (strncmp(*p, "true", 4) == 0) {
        *p += 4;
        return 0;
    }
    if (strncmp(*p, "false", 5) == 0) {
        *p += 5;
        return 0;
    }
    if (strncmp(*p, "null", 4) == 0) {
        *p += 4;
        return 0;
    }
    return -1;
}

static int parse_flat_object(const char **p, FlatSample *flat, char *path, size_t path_len, size_t path_cap) {
    char key[128];
    if (**p != '{') {
        return -1;
    }
    ++(*p);
    skip_ws(p);
    if (**p == '}') {
        ++(*p);
        return 0;
    }
    for (;;) {
        size_t base_len = path_len;
        if (parse_json_string(p, key, sizeof(key)) != 0) {
            return -1;
        }
        if (base_len != 0) {
            if (base_len + 1 >= path_cap) {
                return -1;
            }
            path[base_len++] = '.';
        }
        if (base_len + strlen(key) >= path_cap) {
            return -1;
        }
        memcpy(path + base_len, key, strlen(key) + 1);
        skip_ws(p);
        if (**p != ':') {
            return -1;
        }
        ++(*p);
        skip_ws(p);
        if (parse_flat_value(p, flat, path, strlen(path), path_cap) != 0) {
            return -1;
        }
        path[path_len] = '\0';
        skip_ws(p);
        if (**p == '}') {
            ++(*p);
            return 0;
        }
        if (**p != ',') {
            return -1;
        }
        ++(*p);
        skip_ws(p);
    }
}

static int parse_flat_sample(const char *json, FlatSample *flat) {
    char path[256] = {0};
    const char *p = json;
    memset(flat, 0, sizeof(*flat));
    if (parse_flat_object(&p, flat, path, 0, sizeof(path)) != 0) {
        return -1;
    }
    return 0;
}

static void flat_sample_free(FlatSample *flat) {
    free(flat->metrics);
    flat->metrics = NULL;
    flat->len = 0;
    flat->cap = 0;
}

static DeltaStateInternal *get_delta_state(FtdcState *state) {
    DeltaStateInternal *delta = state->delta_state;
    if (delta == NULL) {
        delta = calloc(1, sizeof(*delta));
        state->delta_state = delta;
    }
    return delta;
}

static void delta_state_replace(DeltaStateInternal *delta, FlatSample *flat) {
    size_t i;
    delta->len = 0;
    for (i = 0; i < flat->len; ++i) {
        if (metric_state_set(&delta->metrics, &delta->len, &delta->cap, flat->metrics[i].path, flat->metrics[i].value) != 0) {
            return;
        }
    }
}

static int build_checkpoint_json(const char *sample_json, size_t sample_len, Buffer *buf) {
    if (sample_len == 0 || sample_json[0] != '{') {
        return -1;
    }
    if (buf_append(buf, "{\"sample_kind\":\"checkpoint\",") != 0) {
        return -1;
    }
    return buf_append_n(buf, sample_json + 1, sample_len - 1);
}

static int build_restart_json(FtdcIdentity *identity, const char *reason, Buffer *buf) {
    return buf_printf(buf,
                      "{\"sample_kind\":\"restart\",\"ts_ms\":%lld,\"process_id\":%lld,"
                      "\"run_id\":\"%s\",\"role\":\"%s\",\"reason\":\"%s\"}",
                      identity->ts_ms,
                      identity->process_id,
                      identity->run_id,
                      identity->role,
                      reason);
}

static int build_delta_json(FlatSample *flat, DeltaStateInternal *delta, Buffer *buf) {
    size_t i;
    int first = 1;
    if (buf_printf(buf, "{\"sample_kind\":\"delta\",\"ts_ms\":%lld,\"deltas\":{", flat->identity.ts_ms) != 0) {
        return -1;
    }
    for (i = 0; i < flat->len; ++i) {
        long long prev = 0;
        long long diff;
        if (metric_state_get(delta->metrics, delta->len, flat->metrics[i].path, &prev) != 0) {
            return -1;
        }
        diff = flat->metrics[i].value - prev;
        if (diff == 0) {
            continue;
        }
        if (!first && buf_append(buf, ",") != 0) {
            return -1;
        }
        first = 0;
        if (buf_printf(buf, "\"%s\":%lld", flat->metrics[i].path, diff) != 0) {
            return -1;
        }
    }
    return buf_append(buf, "}}");
}

static const char *discontinuity_reason(FtdcIdentity *previous, FlatSample *flat) {
    if (!previous->valid) {
        return NULL;
    }
    if (strcmp(previous->run_id, flat->identity.run_id) != 0) {
        return "run_id_changed";
    }
    if (previous->process_id != flat->identity.process_id) {
        return "process_id_changed";
    }
    if (flat->identity.uptime_in_seconds < previous->uptime_in_seconds) {
        return "uptime_decreased";
    }
    return NULL;
}

void ftdc_delta_mark_checkpoint_required(FtdcState *state) {
    state->need_checkpoint = 1;
}

void ftdc_delta_reset_segment(FtdcState *state) {
    DeltaStateInternal *delta = state->delta_state;
    if (delta != NULL) {
        free(delta->metrics);
        delta->metrics = NULL;
        delta->len = 0;
        delta->cap = 0;
    }
    memset(&state->last_identity, 0, sizeof(state->last_identity));
    state->need_checkpoint = 1;
}

void ftdc_delta_free_encoded(FtdcEncodedSample *encoded) {
    free(encoded->record_json);
    free(encoded->event_json);
    memset(encoded, 0, sizeof(*encoded));
}

int ftdc_delta_encode_sample(ValkeyModuleCtx *ctx,
                             FtdcState *state,
                             const char *sample_json,
                             size_t sample_len,
                             FtdcEncodedSample *out) {
    Buffer record = {0};
    Buffer event = {0};
    FlatSample flat = {0};
    DeltaStateInternal *delta;
    const char *reason = NULL;
    int force_checkpoint;
    size_t i;

    memset(out, 0, sizeof(*out));
    if (!state->config.delta_metrics) {
        out->record_json = malloc(sample_len + 1);
        if (out->record_json == NULL) {
            return VALKEYMODULE_ERR;
        }
        memcpy(out->record_json, sample_json, sample_len);
        out->record_json[sample_len] = '\0';
        out->record_len = sample_len;
        return VALKEYMODULE_OK;
    }

    if (parse_flat_sample(sample_json, &flat) != 0) {
        flat_sample_free(&flat);
        return VALKEYMODULE_ERR;
    }
    delta = get_delta_state(state);
    if (delta == NULL) {
        flat_sample_free(&flat);
        return VALKEYMODULE_ERR;
    }

    force_checkpoint = state->need_checkpoint || !state->last_identity.valid;
    if (!force_checkpoint && state->config.checkpoint_interval_ms > 0 &&
        flat.identity.ts_ms - state->last_checkpoint_time_ms >= state->config.checkpoint_interval_ms) {
        force_checkpoint = 1;
    }
    if (!force_checkpoint) {
        reason = discontinuity_reason(&state->last_identity, &flat);
        if (reason != NULL) {
            if (build_restart_json(&flat.identity, reason, &event) != 0) {
                goto fail;
            }
            ValkeyModule_Log(ctx, "debug", "ftdc segment boundary: %s", reason);
            out->has_event = 1;
            force_checkpoint = 1;
        }
    }
    if (!force_checkpoint) {
        for (i = 0; i < flat.len; ++i) {
            long long prev = 0;
            if (metric_state_get(delta->metrics, delta->len, flat.metrics[i].path, &prev) != 0 || flat.metrics[i].value < prev) {
                force_checkpoint = 1;
                break;
            }
        }
    }

    if (force_checkpoint) {
        if (build_checkpoint_json(sample_json, sample_len, &record) != 0) {
            goto fail;
        }
        delta_state_replace(delta, &flat);
        state->last_checkpoint_time_ms = flat.identity.ts_ms;
        state->need_checkpoint = 0;
        out->is_checkpoint = 1;
    } else {
        if (build_delta_json(&flat, delta, &record) != 0) {
            goto fail;
        }
        delta_state_replace(delta, &flat);
    }

    state->last_identity = flat.identity;
    out->record_json = record.data;
    out->record_len = record.len;
    out->event_json = event.data;
    out->event_len = event.len;
    flat_sample_free(&flat);
    return VALKEYMODULE_OK;

fail:
    buf_free(&record);
    buf_free(&event);
    flat_sample_free(&flat);
    return VALKEYMODULE_ERR;
}
