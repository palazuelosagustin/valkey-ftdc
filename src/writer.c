#include "writer.h"

#include "delta.h"
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
    ftdc_delta_mark_checkpoint_required(state);
    ftdc_clear_error(state);
    return VALKEYMODULE_OK;
}

static int append_line(FtdcState *state, const char *json, size_t len) {
    if (fwrite(json, 1, len, state->fp) != len || fputc('\n', state->fp) == EOF) {
        ftdc_set_error(state, "write failed: %s", strerror(errno));
        return VALKEYMODULE_ERR;
    }
    state->current_file_bytes += (long long)len + 1;
    state->bytes_written += (uint64_t)len + 1;
    return VALKEYMODULE_OK;
}

int ftdc_writer_append_sample(ValkeyModuleCtx *ctx, FtdcState *state, const char *json, size_t len) {
    FtdcEncodedSample encoded = {0};
    size_t write_len = len;
    if (state->config.delta_metrics && ftdc_rotation_should_rotate(state, len)) {
        if (ftdc_writer_rotate(ctx, state) != VALKEYMODULE_OK) {
            return VALKEYMODULE_ERR;
        }
    }
    if (!state->config.delta_metrics && ftdc_rotation_should_rotate(state, len)) {
        if (ftdc_writer_rotate(ctx, state) != VALKEYMODULE_OK) {
            return VALKEYMODULE_ERR;
        }
    }
    if (ftdc_delta_encode_sample(ctx, state, json, len, &encoded) != VALKEYMODULE_OK) {
        ftdc_set_error(state, "delta encoding failed");
        return VALKEYMODULE_ERR;
    }
    if (state->config.delta_metrics) {
        write_len = encoded.record_len + (encoded.has_event ? encoded.event_len + 1 : 0);
    }
    if (!state->config.delta_metrics && ftdc_rotation_should_rotate(state, write_len)) {
        ftdc_delta_free_encoded(&encoded);
        if (ftdc_writer_rotate(ctx, state) != VALKEYMODULE_OK) {
            return VALKEYMODULE_ERR;
        }
    }
    if (encoded.has_event && append_line(state, encoded.event_json, encoded.event_len) != VALKEYMODULE_OK) {
        ftdc_delta_free_encoded(&encoded);
        return VALKEYMODULE_ERR;
    }
    if (append_line(state, encoded.record_json, encoded.record_len) != VALKEYMODULE_OK) {
        ftdc_delta_free_encoded(&encoded);
        return VALKEYMODULE_ERR;
    }
    if (fflush(state->fp) != 0) {
        ftdc_delta_free_encoded(&encoded);
        ftdc_set_error(state, "flush failed: %s", strerror(errno));
        return VALKEYMODULE_ERR;
    }
    state->samples_written += 1;
    if (encoded.has_event) {
        state->restart_records_written += 1;
    }
    if (encoded.is_checkpoint || !state->config.delta_metrics) {
        state->checkpoints_written += 1;
    } else {
        state->deltas_written += 1;
    }
    state->last_sample_time_ms = ftdc_now_ms();
    ftdc_delta_free_encoded(&encoded);
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
