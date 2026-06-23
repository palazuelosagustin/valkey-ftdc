#include "collector.h"

#include "hoststats.h"
#include "redact.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct JsonBuf {
    char *data;
    size_t len;
    size_t cap;
} JsonBuf;

static int buf_reserve(JsonBuf *buf, size_t extra) {
    size_t need = buf->len + extra + 1;
    char *next;
    if (buf->cap >= need) {
        return 0;
    }
    while (buf->cap < need) {
        buf->cap = buf->cap == 0 ? 1024 : buf->cap * 2;
    }
    next = realloc(buf->data, buf->cap);
    if (next == NULL) {
        return -1;
    }
    buf->data = next;
    return 0;
}

static int buf_append_n(JsonBuf *buf, const char *s, size_t n) {
    if (buf_reserve(buf, n) != 0) {
        return -1;
    }
    memcpy(buf->data + buf->len, s, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
    return 0;
}

static int buf_append(JsonBuf *buf, const char *s) {
    return buf_append_n(buf, s, strlen(s));
}

static int buf_printf(JsonBuf *buf, const char *fmt, ...) {
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

static int buf_append_json_string_n(JsonBuf *buf, const char *s, size_t n) {
    size_t i;
    if (buf_append(buf, "\"") != 0) {
        return -1;
    }
    for (i = 0; i < n; ++i) {
        unsigned char ch = (unsigned char)s[i];
        switch (ch) {
            case '\\':
            case '"':
                if (buf_printf(buf, "\\%c", ch) != 0) return -1;
                break;
            case '\n':
                if (buf_append(buf, "\\n") != 0) return -1;
                break;
            case '\r':
                if (buf_append(buf, "\\r") != 0) return -1;
                break;
            case '\t':
                if (buf_append(buf, "\\t") != 0) return -1;
                break;
            default:
                if (ch < 0x20) {
                    if (buf_printf(buf, "\\u%04x", ch) != 0) return -1;
                } else if (buf_append_n(buf, (const char *)&ch, 1) != 0) {
                    return -1;
                }
                break;
        }
    }
    return buf_append(buf, "\"");
}

static int buf_append_json_string(JsonBuf *buf, const char *s) {
    return buf_append_json_string_n(buf, s, strlen(s));
}

static void trim_span(const char **start, size_t *len) {
    while (*len > 0 && isspace((unsigned char)(*start)[0])) {
        ++(*start);
        --(*len);
    }
    while (*len > 0 && isspace((unsigned char)(*start)[*len - 1])) {
        --(*len);
    }
}

static int is_number_text_n(const char *s, size_t len) {
    size_t i = 0;
    int dot = 0;
    const char *start = s;
    if (len == 0) return 0;
    if (*s == '-' || *s == '+') {
        ++s;
        --len;
        start = s;
    }
    if (len == 0) return 0;
    if (s[0] == '0' && len > 1 && s[1] != '.') {
        return 0;
    }
    while (i < len) {
        if (s[i] == '.') {
            if (dot) return 0;
            if (i + 1 >= len) return 0;
            dot = 1;
        } else if (!isdigit((unsigned char)s[i])) {
            return 0;
        }
        ++i;
    }
    if (dot && len > 1 && start[0] == '0' && start[1] != '.') {
        return 0;
    }
    return 1;
}

static int append_value_n(JsonBuf *buf, const char *value, size_t value_len);
static int append_info_compound_value(JsonBuf *buf, const char *value, size_t value_len);

static int append_value_n(JsonBuf *buf, const char *value, size_t value_len) {
    trim_span(&value, &value_len);
    if (value_len == 3 && memcmp(value, "yes", 3) == 0) return buf_append(buf, "true");
    if (value_len == 2 && memcmp(value, "no", 2) == 0) return buf_append(buf, "false");
    if (is_number_text_n(value, value_len)) return buf_append_n(buf, value, value_len);
    if (append_info_compound_value(buf, value, value_len) == 0) return 0;
    return buf_append_json_string_n(buf, value, value_len);
}

static int append_info_compound_value(JsonBuf *buf, const char *value, size_t value_len) {
    const char *scan = value;
    const char *end = value + value_len;
    int first = 1;
    int pair_count = 0;

    while (scan < end) {
        const char *token_end = memchr(scan, ',', (size_t)(end - scan));
        const char *eq;
        const char *key;
        const char *val;
        size_t key_len;
        size_t val_len;

        if (token_end == NULL) {
            token_end = end;
        }
        key = scan;
        key_len = (size_t)(token_end - scan);
        trim_span(&key, &key_len);
        if (key_len == 0) {
            return -1;
        }
        eq = memchr(key, '=', key_len);
        if (eq == NULL || eq == key || eq == key + key_len - 1) {
            return -1;
        }
        val = eq + 1;
        key_len = (size_t)(eq - key);
        val_len = (size_t)(token_end - val);
        trim_span(&key, &key_len);
        trim_span(&val, &val_len);
        if (key_len == 0 || val_len == 0) {
            return -1;
        }
        ++pair_count;
        scan = token_end < end ? token_end + 1 : token_end;
    }

    if (pair_count < 2) {
        return -1;
    }

    scan = value;
    if (buf_append(buf, "{") != 0) {
        return -1;
    }
    while (scan < end) {
        const char *token_end = memchr(scan, ',', (size_t)(end - scan));
        const char *eq;
        const char *key;
        const char *val;
        size_t key_len;
        size_t val_len;

        if (token_end == NULL) {
            token_end = end;
        }
        key = scan;
        key_len = (size_t)(token_end - scan);
        trim_span(&key, &key_len);
        eq = memchr(key, '=', key_len);
        val = eq + 1;
        key_len = (size_t)(eq - key);
        val_len = (size_t)(token_end - val);
        trim_span(&key, &key_len);
        trim_span(&val, &val_len);
        if (!first && buf_append(buf, ",") != 0) {
            return -1;
        }
        first = 0;
        if (buf_append_json_string_n(buf, key, key_len) != 0 ||
            buf_append(buf, ":") != 0 ||
            append_value_n(buf, val, val_len) != 0) {
            return -1;
        }
        scan = token_end < end ? token_end + 1 : token_end;
    }
    return buf_append(buf, "}");
}

static int append_info_section_json(JsonBuf *buf, const char *payload, size_t payload_len) {
    const char *p = payload;
    const char *end = payload + payload_len;
    int first = 1;
    if (buf_append(buf, "{") != 0) return -1;
    while (p < end) {
        const char *line_end = memchr(p, '\n', (size_t)(end - p));
        const char *colon;
        if (line_end == NULL) {
            line_end = end;
        }
        if (*p == '#' || *p == '\r' || p == line_end) {
            p = (line_end < end) ? line_end + 1 : line_end;
            continue;
        }
        colon = memchr(p, ':', (size_t)(line_end - p));
        if (colon != NULL) {
            const char *key = p;
            const char *value = colon + 1;
            size_t key_len = (size_t)(colon - key);
            size_t value_len = (size_t)(line_end - value);
            while (value_len > 0 && (value[value_len - 1] == '\r' || value[value_len - 1] == '\n')) {
                --value_len;
            }
            if (!first && buf_append(buf, ",") != 0) return -1;
            first = 0;
            if (buf_append_json_string_n(buf, key, key_len) != 0 ||
                buf_append(buf, ":") != 0 ||
                append_value_n(buf, value, value_len) != 0) {
                return -1;
            }
        }
        p = (line_end < end) ? line_end + 1 : line_end;
    }
    return buf_append(buf, "}");
}

static int append_info_call(JsonBuf *buf, ValkeyModuleCtx *ctx, const char *section) {
    ValkeyModuleString *section_arg = ValkeyModule_CreateString(ctx, section, strlen(section));
    ValkeyModuleCallReply *reply = ValkeyModule_Call(ctx, "INFO", "s", section_arg);
    size_t len = 0;
    const char *payload;
    if (reply == NULL || ValkeyModule_CallReplyType(reply) != VALKEYMODULE_REPLY_STRING) {
        if (reply != NULL) ValkeyModule_FreeCallReply(reply);
        return buf_append(buf, "{}");
    }
    payload = ValkeyModule_CallReplyStringPtr(reply, &len);
    if (append_info_section_json(buf, payload, len) != 0) {
        ValkeyModule_FreeCallReply(reply);
        return -1;
    }
    ValkeyModule_FreeCallReply(reply);
    return 0;
}

static int append_latency_latest(JsonBuf *buf, ValkeyModuleCtx *ctx) {
    ValkeyModuleCallReply *reply = ValkeyModule_Call(ctx, "LATENCY", "c", "LATEST");
    size_t i;
    if (reply == NULL || ValkeyModule_CallReplyType(reply) != VALKEYMODULE_REPLY_ARRAY) {
        if (reply != NULL) ValkeyModule_FreeCallReply(reply);
        return buf_append(buf, "[]");
    }
    if (buf_append(buf, "[") != 0) {
        ValkeyModule_FreeCallReply(reply);
        return -1;
    }
    for (i = 0; i < ValkeyModule_CallReplyLength(reply); ++i) {
        ValkeyModuleCallReply *row = ValkeyModule_CallReplyArrayElement(reply, i);
        if (i > 0 && buf_append(buf, ",") != 0) {
            ValkeyModule_FreeCallReply(reply);
            return -1;
        }
        if (row == NULL || ValkeyModule_CallReplyType(row) != VALKEYMODULE_REPLY_ARRAY || ValkeyModule_CallReplyLength(row) < 4) {
            if (buf_append(buf, "{}") != 0) {
                ValkeyModule_FreeCallReply(reply);
                return -1;
            }
            continue;
        }
        {
            ValkeyModuleCallReply *name = ValkeyModule_CallReplyArrayElement(row, 0);
            ValkeyModuleCallReply *latest = ValkeyModule_CallReplyArrayElement(row, 1);
            ValkeyModuleCallReply *max = ValkeyModule_CallReplyArrayElement(row, 2);
            ValkeyModuleCallReply *all_time = ValkeyModule_CallReplyArrayElement(row, 3);
            size_t name_len = 0;
            const char *name_ptr = ValkeyModule_CallReplyStringPtr(name, &name_len);
            if (buf_append(buf, "{") != 0 ||
                buf_append(buf, "\"event\":") != 0 || buf_append_json_string_n(buf, name_ptr, name_len) != 0 ||
                buf_printf(buf, ",\"latest_ms\":%lld,\"max_ms\":%lld,\"all_time_ms\":%lld}",
                           ValkeyModule_CallReplyInteger(latest),
                           ValkeyModule_CallReplyInteger(max),
                           ValkeyModule_CallReplyInteger(all_time)) != 0) {
                ValkeyModule_FreeCallReply(reply);
                return -1;
            }
        }
    }
    ValkeyModule_FreeCallReply(reply);
    return buf_append(buf, "]");
}

static int append_slowlog_json(JsonBuf *buf, ValkeyModuleCtx *ctx, FtdcState *state) {
    ValkeyModuleCallReply *reply = ValkeyModule_Call(ctx, "SLOWLOG", "c", "LEN");
    long long length = 0;
    if (reply != NULL && ValkeyModule_CallReplyType(reply) == VALKEYMODULE_REPLY_INTEGER) {
        length = ValkeyModule_CallReplyInteger(reply);
    }
    if (reply != NULL) {
        ValkeyModule_FreeCallReply(reply);
    }
    if (buf_printf(buf, "{\"len\":%lld", length) != 0) return -1;
    if (!state->config.collect_slowlog) {
        return buf_append(buf, "}");
    }
    reply = ValkeyModule_Call(ctx, "SLOWLOG", "cc", "GET", "8");
    if (reply == NULL || ValkeyModule_CallReplyType(reply) != VALKEYMODULE_REPLY_ARRAY) {
        if (reply != NULL) ValkeyModule_FreeCallReply(reply);
        return buf_append(buf, ",\"entries\":[]}");
    }
    if (buf_append(buf, ",\"entries\":[") != 0) {
        ValkeyModule_FreeCallReply(reply);
        return -1;
    }
    for (size_t i = 0; i < ValkeyModule_CallReplyLength(reply); ++i) {
        ValkeyModuleCallReply *row = ValkeyModule_CallReplyArrayElement(reply, i);
        if (i > 0 && buf_append(buf, ",") != 0) {
            ValkeyModule_FreeCallReply(reply);
            return -1;
        }
        if (row == NULL || ValkeyModule_CallReplyType(row) != VALKEYMODULE_REPLY_ARRAY || ValkeyModule_CallReplyLength(row) < 4) {
            if (buf_append(buf, "{}") != 0) {
                ValkeyModule_FreeCallReply(reply);
                return -1;
            }
            continue;
        }
        if (buf_printf(buf, "{\"id\":%lld,\"ts\":%lld,\"duration_usec\":%lld,\"args\":[",
                       ValkeyModule_CallReplyInteger(ValkeyModule_CallReplyArrayElement(row, 0)),
                       ValkeyModule_CallReplyInteger(ValkeyModule_CallReplyArrayElement(row, 1)),
                       ValkeyModule_CallReplyInteger(ValkeyModule_CallReplyArrayElement(row, 2))) != 0) {
            ValkeyModule_FreeCallReply(reply);
            return -1;
        }
        {
            ValkeyModuleCallReply *args = ValkeyModule_CallReplyArrayElement(row, 3);
            for (size_t j = 0; args != NULL && j < ValkeyModule_CallReplyLength(args); ++j) {
                ValkeyModuleCallReply *arg = ValkeyModule_CallReplyArrayElement(args, j);
                size_t arg_len = 0;
                const char *arg_ptr = ValkeyModule_CallReplyStringPtr(arg, &arg_len);
                if (j > 0 && buf_append(buf, ",") != 0) {
                    ValkeyModule_FreeCallReply(reply);
                    return -1;
                }
                if (state->config.slowlog_redact) {
                    if (buf_append_json_string(buf, ftdc_redact_slowlog_arg(arg_ptr)) != 0) {
                        ValkeyModule_FreeCallReply(reply);
                        return -1;
                    }
                } else if (buf_append_json_string_n(buf, arg_ptr, arg_len) != 0) {
                    ValkeyModule_FreeCallReply(reply);
                    return -1;
                }
            }
        }
        if (buf_append(buf, "]}") != 0) {
            ValkeyModule_FreeCallReply(reply);
            return -1;
        }
    }
    ValkeyModule_FreeCallReply(reply);
    return buf_append(buf, "]}");
}

int ftdc_collect_sample(ValkeyModuleCtx *ctx, FtdcState *state, char **json_out, size_t *len_out) {
    static const char *sections[] = {
        "server", "clients", "memory", "persistence", "stats", "replication", "cpu", "commandstats", "cluster",
    };
    JsonBuf buf = {0};
    size_t i;
    if (buf_append(&buf, "{") != 0) goto oom;
    if (buf_printf(&buf, "\"ts_ms\":%lld,", ftdc_now_ms()) != 0) goto oom;
    if (buf_append(&buf, "\"valkey\":{\"info\":{") != 0) goto oom;
    for (i = 0; i < sizeof(sections) / sizeof(sections[0]); ++i) {
        if (i > 0 && buf_append(&buf, ",") != 0) goto oom;
        if (buf_append_json_string(&buf, sections[i]) != 0 || buf_append(&buf, ":") != 0) goto oom;
        if (append_info_call(&buf, ctx, sections[i]) != 0) goto oom;
    }
    if (buf_append(&buf, "},\"latency_latest\":") != 0) goto oom;
    if (append_latency_latest(&buf, ctx) != 0) goto oom;
    if (buf_append(&buf, ",\"slowlog\":") != 0) goto oom;
    if (append_slowlog_json(&buf, ctx, state) != 0) goto oom;
    if (buf_append(&buf, "}") != 0) goto oom;
    if (buf_append(&buf, ",\"host\":") != 0) goto oom;
    if (state->config.collect_host_stats) {
        if (ftdc_append_hoststats_json((struct JsonBuf *)&buf) != 0) goto oom;
    } else if (buf_append(&buf, "{\"enabled\":false}") != 0) {
        goto oom;
    }
    if (buf_append(&buf, "}") != 0) goto oom;
    *json_out = buf.data;
    *len_out = buf.len;
    return VALKEYMODULE_OK;
oom:
    free(buf.data);
    return VALKEYMODULE_ERR;
}

void ftdc_collect_free(char *ptr) {
    free(ptr);
}
