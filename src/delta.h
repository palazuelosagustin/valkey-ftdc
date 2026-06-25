#ifndef VALKEY_FTDC_DELTA_H
#define VALKEY_FTDC_DELTA_H

#include <stddef.h>

#include "ftdc.h"

typedef struct FtdcEncodedSample {
    char *record_json;
    size_t record_len;
    char *event_json;
    size_t event_len;
    int is_checkpoint;
    int has_event;
} FtdcEncodedSample;

int ftdc_delta_encode_sample(ValkeyModuleCtx *ctx,
                             FtdcState *state,
                             const char *sample_json,
                             size_t sample_len,
                             FtdcEncodedSample *out);
void ftdc_delta_free_encoded(FtdcEncodedSample *encoded);
void ftdc_delta_reset_segment(FtdcState *state);
void ftdc_delta_mark_checkpoint_required(FtdcState *state);

#endif
