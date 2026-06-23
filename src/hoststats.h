#ifndef VALKEY_FTDC_HOSTSTATS_H
#define VALKEY_FTDC_HOSTSTATS_H

struct JsonBuf;

int ftdc_append_hoststats_json(struct JsonBuf *buf);

#endif
