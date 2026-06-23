#include "hoststats.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct JsonBuf {
    char *data;
    size_t len;
    size_t cap;
};

static int buf_reserve(struct JsonBuf *buf, size_t extra) {
    size_t need = buf->len + extra + 1;
    char *next;
    if (need <= buf->cap) {
        return 0;
    }
    while (buf->cap < need) {
        buf->cap *= 2;
    }
    next = realloc(buf->data, buf->cap);
    if (next == NULL) {
        return -1;
    }
    buf->data = next;
    return 0;
}

static int buf_append_n(struct JsonBuf *buf, const char *s, size_t n) {
    if (buf_reserve(buf, n) != 0) {
        return -1;
    }
    memcpy(buf->data + buf->len, s, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
    return 0;
}

static int buf_append(struct JsonBuf *buf, const char *s) {
    return buf_append_n(buf, s, strlen(s));
}

static int buf_append_json_escaped_n(struct JsonBuf *buf, const char *s, size_t n) {
    size_t i;
    for (i = 0; i < n; ++i) {
        unsigned char ch = (unsigned char)s[i];
        char esc[7];
        switch (ch) {
            case '\\':
            case '"':
                if (buf_append(buf, "\\") != 0 || buf_append_n(buf, (const char *)&ch, 1) != 0) return -1;
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
                    snprintf(esc, sizeof(esc), "\\u%04x", ch);
                    if (buf_append(buf, esc) != 0) return -1;
                } else if (buf_append_n(buf, (const char *)&ch, 1) != 0) {
                    return -1;
                }
                break;
        }
    }
    return 0;
}

static int buf_append_json_string_n(struct JsonBuf *buf, const char *s, size_t n) {
    if (buf_append(buf, "\"") != 0) {
        return -1;
    }
    if (buf_append_json_escaped_n(buf, s, n) != 0) {
        return -1;
    }
    return buf_append(buf, "\"");
}

static int buf_append_json_string(struct JsonBuf *buf, const char *s) {
    return buf_append_json_string_n(buf, s, strlen(s));
}

static int append_file_string_object(struct JsonBuf *buf, const char *path) {
    FILE *fp = fopen(path, "r");
    char line[512];
    int first = 1;
    if (fp == NULL) {
        return buf_append(buf, "{\"available\":false}");
    }
    if (buf_append(buf, "{") != 0) {
        fclose(fp);
        return -1;
    }
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *colon = strchr(line, ':');
        char *key = line;
        char *value = colon ? colon + 1 : NULL;
        char *end;
        if (value == NULL) {
            continue;
        }
        *colon = '\0';
        while (*value && isspace((unsigned char)*value)) {
            ++value;
        }
        end = value + strlen(value);
        while (end > value && isspace((unsigned char)end[-1])) {
            --end;
        }
        *end = '\0';
        if (!first && buf_append(buf, ",") != 0) {
            fclose(fp);
            return -1;
        }
        first = 0;
        if (buf_append_json_string(buf, key) != 0 || buf_append(buf, ":") != 0 ||
            buf_append_json_string(buf, value) != 0) {
            fclose(fp);
            return -1;
        }
    }
    fclose(fp);
    return buf_append(buf, "}");
}

static int append_loadavg_json(struct JsonBuf *buf) {
    FILE *fp = fopen("/proc/loadavg", "r");
    double one = 0, five = 0, fifteen = 0;
    char tmp[128];
    if (fp == NULL) {
        return buf_append(buf, "{\"available\":false}");
    }
    if (fscanf(fp, "%lf %lf %lf", &one, &five, &fifteen) != 3) {
        fclose(fp);
        return buf_append(buf, "{\"available\":false}");
    }
    fclose(fp);
    snprintf(tmp, sizeof(tmp), "{\"available\":true,\"1m\":%.2f,\"5m\":%.2f,\"15m\":%.2f}", one, five, fifteen);
    return buf_append(buf, tmp);
}

static int append_proc_stat_json(struct JsonBuf *buf) {
    FILE *fp = fopen("/proc/stat", "r");
    char line[512];
    unsigned long long user = 0, nice = 0, sys = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
    char tmp[256];
    if (fp == NULL) {
        return buf_append(buf, "{\"available\":false}");
    }
    if (fgets(line, sizeof(line), fp) == NULL ||
        sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu", &user, &nice, &sys, &idle, &iowait, &irq, &softirq, &steal) < 4) {
        fclose(fp);
        return buf_append(buf, "{\"available\":false}");
    }
    fclose(fp);
    snprintf(tmp, sizeof(tmp),
             "{\"available\":true,\"user\":%llu,\"nice\":%llu,\"system\":%llu,\"idle\":%llu,\"iowait\":%llu,\"irq\":%llu,\"softirq\":%llu,\"steal\":%llu}",
             user, nice, sys, idle, iowait, irq, softirq, steal);
    return buf_append(buf, tmp);
}

static int append_file_blob(struct JsonBuf *buf, const char *path, size_t limit) {
    FILE *fp = fopen(path, "r");
    char chunk[256];
    size_t remaining = limit;
    int rc;
    if (fp == NULL) {
        return buf_append(buf, "\"\"");
    }
    rc = buf_append(buf, "\"");
    while (rc == 0 && remaining > 0) {
        size_t to_read = sizeof(chunk) - 1;
        size_t n;
        if (to_read > remaining) {
            to_read = remaining;
        }
        n = fread(chunk, 1, to_read, fp);
        if (n == 0) {
            break;
        }
        rc = buf_append_json_escaped_n(buf, chunk, n);
        remaining -= n;
    }
    fclose(fp);
    if (rc != 0) {
        return rc;
    }
    return buf_append(buf, "\"");
}

int ftdc_append_hoststats_json(struct JsonBuf *buf) {
    if (buf_append(buf, "{") != 0) return -1;
    if (buf_append(buf, "\"supported\":true,") != 0) return -1;
    if (buf_append(buf, "\"loadavg\":") != 0 || append_loadavg_json(buf) != 0) return -1;
    if (buf_append(buf, ",\"cpu\":") != 0 || append_proc_stat_json(buf) != 0) return -1;
    if (buf_append(buf, ",\"memory\":") != 0 || append_file_string_object(buf, "/proc/meminfo") != 0) return -1;
    if (buf_append(buf, ",\"disk\":{") != 0) return -1;
    if (buf_append(buf, "\"diskstats\":") != 0 || append_file_blob(buf, "/proc/diskstats", 512) != 0) return -1;
    if (buf_append(buf, "}") != 0) return -1;
    if (buf_append(buf, ",\"network\":{") != 0) return -1;
    if (buf_append(buf, "\"net_dev\":") != 0 || append_file_blob(buf, "/proc/net/dev", 512) != 0) return -1;
    if (buf_append(buf, "}") != 0) return -1;
    if (buf_append(buf, ",\"process\":{") != 0) return -1;
    if (buf_append(buf, "\"status\":") != 0 || append_file_string_object(buf, "/proc/self/status") != 0) return -1;
    if (buf_append(buf, ",\"io\":") != 0 || append_file_string_object(buf, "/proc/self/io") != 0) return -1;
    if (buf_append(buf, "}") != 0) return -1;
    return buf_append(buf, "}");
}
