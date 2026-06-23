#ifndef VALKEY_FTDC_WRITER_H
#define VALKEY_FTDC_WRITER_H

#include <stddef.h>

#include "ftdc.h"

int ftdc_writer_append_sample(ValkeyModuleCtx *ctx, FtdcState *state, const char *json, size_t len);
int ftdc_writer_flush(FtdcState *state);
int ftdc_writer_rotate(ValkeyModuleCtx *ctx, FtdcState *state);
void ftdc_writer_close(FtdcState *state);

#endif
