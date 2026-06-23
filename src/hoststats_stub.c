#include "hoststats.h"

#include <stdio.h>
#include <string.h>

struct JsonBuf {
    char *data;
    size_t len;
    size_t cap;
};

static int buf_append(struct JsonBuf *buf, const char *s) {
    size_t n = strlen(s);
    if (buf->len + n + 1 > buf->cap) {
        return -1;
    }
    memcpy(buf->data + buf->len, s, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
    return 0;
}

int ftdc_append_hoststats_json(struct JsonBuf *buf) {
    return buf_append(buf, "{\"supported\":false}");
}
