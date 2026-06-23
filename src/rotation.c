#include "rotation.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef struct RetentionFile {
    char path[PATH_MAX];
    time_t mtime;
    off_t size;
} RetentionFile;

static int join_path(char *dst, size_t dst_len, const char *dir, const char *name) {
    int n = snprintf(dst, dst_len, "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= dst_len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static int mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    size_t len = strlen(path);
    size_t i;
    if (len == 0 || len >= sizeof(tmp)) {
        return -1;
    }
    memcpy(tmp, path, len + 1);
    for (i = 1; i < len; ++i) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

int ftdc_rotation_prepare_dir(FtdcState *state) {
    return mkdir_p(state->config.path);
}

static void format_utc_timestamp(long long now_ms, char *dst, size_t dst_len) {
    time_t secs = (time_t)(now_ms / 1000);
    struct tm tmv;
    gmtime_r(&secs, &tmv);
    strftime(dst, dst_len, "%Y-%m-%dT%H-%M-%SZ", &tmv);
}

int ftdc_rotation_open_next_file(FtdcState *state) {
    char stamp[64];
    char meta_path[PATH_MAX];
    FILE *meta;
    format_utc_timestamp(ftdc_now_ms(), stamp, sizeof(stamp));
    {
        char metrics_name[96];
        char metadata_name[96];
        int n1 = snprintf(metrics_name, sizeof(metrics_name), "metrics.%s.vkftdc", stamp);
        int n2 = snprintf(metadata_name, sizeof(metadata_name), "metadata.%s.json", stamp);
        if (n1 < 0 || (size_t)n1 >= sizeof(metrics_name) || n2 < 0 || (size_t)n2 >= sizeof(metadata_name)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        if (join_path(state->current_file, sizeof(state->current_file), state->config.path, metrics_name) != 0 ||
            join_path(meta_path, sizeof(meta_path), state->config.path, metadata_name) != 0) {
            return -1;
        }
    }
    state->fp = fopen(state->current_file, "ab");
    if (state->fp == NULL) {
        return -1;
    }
    if (fwrite(FTDC_FILE_MAGIC, 1, strlen(FTDC_FILE_MAGIC), state->fp) != strlen(FTDC_FILE_MAGIC)) {
        fclose(state->fp);
        state->fp = NULL;
        return -1;
    }
    fprintf(state->fp,
            "{\"format_version\":%d,\"module\":\"valkey-ftdc\",\"compression\":\"none\",\"created_at_ms\":%lld}\n",
            FTDC_METADATA_VERSION, ftdc_now_ms());
    fflush(state->fp);
    state->current_file_bytes = (long long)ftell(state->fp);
    state->current_file_opened_ms = ftdc_now_ms();

    meta = fopen(meta_path, "wb");
    if (meta != NULL) {
        fprintf(meta,
                "{\"format_version\":%d,\"module\":\"valkey-ftdc\",\"current_file\":\"%s\",\"created_at_ms\":%lld}\n",
                FTDC_METADATA_VERSION, state->current_file, state->current_file_opened_ms);
        fclose(meta);
    }
    return 0;
}

int ftdc_rotation_should_rotate(FtdcState *state, size_t sample_len) {
    long long now_ms = ftdc_now_ms();
    if (state->fp == NULL) {
        return 1;
    }
    if (state->config.max_file_bytes > 0 &&
        state->current_file_bytes + (long long)sample_len + 1 >= state->config.max_file_bytes) {
        return 1;
    }
    if (state->config.rotation_interval_sec > 0 &&
        now_ms - state->current_file_opened_ms >= state->config.rotation_interval_sec * 1000) {
        return 1;
    }
    return 0;
}

static int retention_cmp(const void *lhs, const void *rhs) {
    const RetentionFile *a = lhs;
    const RetentionFile *b = rhs;
    if (a->mtime < b->mtime) return -1;
    if (a->mtime > b->mtime) return 1;
    return strcmp(a->path, b->path);
}

int ftdc_rotation_enforce_retention(FtdcState *state) {
    DIR *dir;
    struct dirent *ent;
    RetentionFile *files = NULL;
    size_t count = 0;
    size_t cap = 0;
    long long total = 0;
    size_t i;
    if (state->config.max_dir_bytes <= 0) {
        return 0;
    }
    dir = opendir(state->config.path);
    if (dir == NULL) {
        return -1;
    }
    while ((ent = readdir(dir)) != NULL) {
        struct stat st;
        char full[PATH_MAX];
        if (!starts_with(ent->d_name, "metrics.") && !starts_with(ent->d_name, "metadata.")) {
            continue;
        }
        if (join_path(full, sizeof(full), state->config.path, ent->d_name) != 0) {
            continue;
        }
        if (stat(full, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (count == cap) {
            RetentionFile *next;
            cap = cap == 0 ? 8 : cap * 2;
            next = realloc(files, cap * sizeof(*files));
            if (next == NULL) {
                free(files);
                closedir(dir);
                return -1;
            }
            files = next;
        }
        snprintf(files[count].path, sizeof(files[count].path), "%s", full);
        files[count].mtime = st.st_mtime;
        files[count].size = st.st_size;
        total += st.st_size;
        ++count;
    }
    closedir(dir);

    qsort(files, count, sizeof(*files), retention_cmp);
    for (i = 0; i < count && total > state->config.max_dir_bytes; ++i) {
        if (strcmp(files[i].path, state->current_file) == 0) {
            continue;
        }
        if (unlink(files[i].path) == 0) {
            total -= files[i].size;
        }
    }
    free(files);
    return 0;
}
