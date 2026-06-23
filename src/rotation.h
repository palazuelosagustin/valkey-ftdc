#ifndef VALKEY_FTDC_ROTATION_H
#define VALKEY_FTDC_ROTATION_H

#include "ftdc.h"

int ftdc_rotation_prepare_dir(FtdcState *state);
int ftdc_rotation_open_next_file(FtdcState *state);
int ftdc_rotation_should_rotate(FtdcState *state, size_t sample_len);
int ftdc_rotation_enforce_retention(FtdcState *state);

#endif
