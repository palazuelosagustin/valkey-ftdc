#ifndef VALKEY_FTDC_COLLECTOR_H
#define VALKEY_FTDC_COLLECTOR_H

#include <stddef.h>

#include "ftdc.h"

int ftdc_collect_sample(ValkeyModuleCtx *ctx, FtdcState *state, char **json_out, size_t *len_out);
void ftdc_collect_free(char *ptr);

#endif
