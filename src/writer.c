#include "writer.h"

#include "rotation.h"

#include <errno.h>
#include <string.h>

void ftdc_writer_close(FtdcState *state) {
    if (state->fp != NULL) {
        fclose(state->fp);
        state->fp = NULL;
    }
    state->current_file[0] = '\0';
    state->current_file_bytes = 0;
    state->current_file_opened_ms = 0;
}

int ftdc_writer_rotate(ValkeyModuleCtx *ctx, FtdcState *state) {
    (void)ctx;
    ftdc_writer_close(state);
    if (ftdc_rotation_prepare_dir(state) != 0) {
        ftdc_set_error(state, "mkdir %s failed: %s", state->config.path, strerror(errno));
        return VALKEYMODULE_ERR;
    }
    if (ftdc_rotation_open_next_file(ctx, state) != 0) {
        ftdc_set_error(state, "open diagnostic file failed: %s", strerror(errno));
        return VALKEYMODULE_ERR;
    }
    if (ftdc_rotation_enforce_retention(state) != 0) {
        ftdc_set_error(state, "retention failed: %s", strerror(errno));
        return VALKEYMODULE_ERR;
    }
    ftdc_clear_error(state);
    return VALKEYMODULE_OK;
}

int ftdc_writer_append_sample(ValkeyModuleCtx *ctx, FtdcState *state, const char *json, size_t len) {
    if (ftdc_rotation_should_rotate(state, len)) {
        if (ftdc_writer_rotate(ctx, state) != VALKEYMODULE_OK) {
            return VALKEYMODULE_ERR;
        }
    }
    if (fwrite(json, 1, len, state->fp) != len || fputc('\n', state->fp) == EOF) {
        ftdc_set_error(state, "write failed: %s", strerror(errno));
        return VALKEYMODULE_ERR;
    }
    fflush(state->fp);
    state->current_file_bytes += (long long)len + 1;
    state->bytes_written += (uint64_t)len + 1;
    state->samples_written += 1;
    state->last_sample_time_ms = ftdc_now_ms();
    if (ftdc_rotation_enforce_retention(state) != 0) {
        ftdc_set_error(state, "retention failed: %s", strerror(errno));
        return VALKEYMODULE_ERR;
    }
    ftdc_clear_error(state);
    return VALKEYMODULE_OK;
}

int ftdc_writer_flush(FtdcState *state) {
    if (state->fp == NULL) {
        return VALKEYMODULE_OK;
    }
    if (fflush(state->fp) != 0) {
        ftdc_set_error(state, "flush failed: %s", strerror(errno));
        return VALKEYMODULE_ERR;
    }
    return VALKEYMODULE_OK;
}
