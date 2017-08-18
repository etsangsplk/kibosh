/**
 * Copyright 2017 Confluent Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#include "fault.h"
#include "file.h"
#include "fs.h"
#include "io.h"
#include "log.h"
#include "meta.h"
#include "pid.h"
#include "util.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/**
 * The length of the control buffer which we use when reading the control file JSON.
 */
#define CONTROL_BUF_LEN 16384

/**
 * The environment variable used to set the path to the pid file.
 */
#define PIDFILE_PATH "PIDFILE_PATH"

static int kibosh_fs_alloc_oom(struct kibosh_fs *fs)
{
    INFO("kibosh_fs_alloc: OOM\n");
    kibosh_fs_free(fs);
    return -ENOMEM;
}

int kibosh_fs_alloc(struct kibosh_fs **out, const char *root)
{
    int ret;
    struct kibosh_fs *fs;
    char *path;

    *out = NULL;
    fs = calloc(1, sizeof(*fs));
    if (!fs)
        return kibosh_fs_alloc_oom(fs);
    if (pthread_mutex_init(&fs->lock, NULL)) {
        ret = -errno;
        free(fs);
        INFO("kibosh_fs_alloc: pthread_mutex_init failed: %s (%d)\n", safe_strerror(-ret), -ret);
        return ret;
    }
    fs->control_fd = -1;
    fs->root = strdup(root);
    if (!fs->root)
        return kibosh_fs_alloc_oom(fs);
    if (access(fs->root, R_OK) < 0) {
        ret = -errno;
        INFO("kibosh_fs_alloc: unable to access root path %s: %s\n", fs->root, safe_strerror(ret));
        kibosh_fs_free(fs);
        return ret;
    }
    path = getenv(PIDFILE_PATH);
    if (path) {
        fs->pidfile_path = strdup(path);
        if (!fs->pidfile_path)
            return kibosh_fs_alloc_oom(fs);
        ret = write_pidfile(fs->pidfile_path);
        if (ret < 0) {
            kibosh_fs_free(fs);
            return ret;
        }
    }
    fs->control_fd = memfd_create(KIBOSH_CONTROL);
    if (fs->control_fd < 0) {
        ret = fs->control_fd;
        INFO("kibosh_fs_alloc: memfd_create failed: %s\n", safe_strerror(-ret));
        kibosh_fs_free(fs);
        return ret;
    }
    ret = safe_write(fs->control_fd, FAULTS_EMPTY_JSON, strlen(FAULTS_EMPTY_JSON) + 1);
    if (ret < 0) {
        INFO("kibosh_fs_alloc: failed to write initial JSON to control file: %s\n", safe_strerror(-ret));
        kibosh_fs_free(fs);
        return ret;
    }
    ret = faults_parse(FAULTS_EMPTY_JSON, &fs->faults);
    if (ret < 0) {
        INFO("kibosh_fs_alloc: failed to parse empty faults json %s\n", FAULTS_EMPTY_JSON);
        kibosh_fs_free(fs);
        return ret;
    }
    fs->control_buf = malloc(CONTROL_BUF_LEN);
    if (!fs->control_buf) {
        ret = -ENOMEM;
        INFO("kibosh_fs_alloc: failed to allocate control buffer.\n");
        kibosh_fs_free(fs);
        return ret;
    }
    *out = fs;
    return 0;
}

void kibosh_fs_free(struct kibosh_fs *fs)
{
    if (!fs)
        return;
    if (fs->root) {
        free(fs->root);
        fs->root = NULL;
    }
    if (fs->pidfile_path) {
        remove_pidfile(fs->pidfile_path);
        free(fs->pidfile_path);
        fs->pidfile_path = NULL;
    }
    if (fs->control_fd >= 0) {
        close(fs->control_fd);
        fs->control_fd = -1;
    }
    if (fs->faults) {
        faults_free(fs->faults);
        fs->faults = NULL;
    }
    if (fs->control_buf) {
        free(fs->control_buf);
        fs->control_buf = NULL;
    }
    pthread_mutex_destroy(&fs->lock);
    free(fs);
}

int kibosh_fs_accessor_fd_alloc(struct kibosh_fs *fs, int populate)
{
    int new_fd = -1, ret;

    new_fd = memfd_create(KIBOSH_CONTROL);
    if (new_fd < 0) {
        ret = new_fd;
        INFO("kibosh_fs_accessor_fd_alloc: memfd_create failed: error %d (%s)\n",
             -ret, safe_strerror(-ret));
        goto error;
    }
    if (populate) {
        pthread_mutex_lock(&fs->lock);
        if (lseek(fs->control_fd, 0, SEEK_SET) < 0) {
            ret = -errno;
            INFO("kibosh_fs_accessor_fd_alloc: lseek(fs->control_fd, 0, SEEK_SET) "
                 "failed: error %d (%s)\n", -ret, safe_strerror(-ret));
            goto error_release_lock;
        }
        ret = duplicate_fd(new_fd, fs->control_fd);
        if (ret < 0) {
            INFO("kibosh_fs_accessor_fd_alloc: duplicate_fd failed: error %d (%s)\n",
                 -ret, safe_strerror(-ret));
            goto error_release_lock;
        }
        if (lseek(new_fd, 0, SEEK_SET) < 0) {
            ret = -errno;
            INFO("kibosh_fs_accessor_fd_alloc: lseek(new_fd, 0, SEEK_SET) "
                 "failed: error %d (%s)\n", -ret, safe_strerror(-ret));
            goto error_release_lock;
        }
        pthread_mutex_unlock(&fs->lock);
    }
    return new_fd;

error_release_lock:
    pthread_mutex_unlock(&fs->lock);
    close(new_fd);
error:
    return ret;
}

int kibosh_fs_control_stat(struct kibosh_fs *fs, struct stat *stbuf)
{
    int ret = 0;

    pthread_mutex_lock(&fs->lock);
    if (fstat(fs->control_fd, stbuf) < 0) {
        ret = -errno;
    }
    pthread_mutex_unlock(&fs->lock);
    return ret;
}

static void swap_ints(int *a, int *b)
{
    int temp;
    temp = *a;
    *a = *b;
    *b = temp;
}

int kibosh_fs_accessor_fd_release(struct kibosh_fs *fs, int fd)
{
    int flags, ret = 0;
    struct kibosh_faults *faults = NULL;

    flags = fcntl(fd, F_GETFL, 0);
    if ((flags & O_ACCMODE) == O_RDONLY) {
        DEBUG("kibosh_fs_accessor_fd_release: closing read-only accessor.\n");
        goto done_close_fd;
    }
    pthread_mutex_lock(&fs->lock);
    if (lseek(fd, 0, SEEK_SET) < 0) {
        ret = -errno;
        INFO("kibosh_fs_accessor_fd_release: lseek(control_fd, 0, SEEK_SET) failed: "
             "error %d (%s)\n", -ret, safe_strerror(-ret));
        goto done_release_lock;
    }
    ret = read_string_from_fd(fd, fs->control_buf, CONTROL_BUF_LEN);
    if (ret < 0) {
        INFO("kibosh_fs_accessor_fd_release: read_string_from_fd(control_fd) failed: "
             "error %d (%s)\n", -ret, safe_strerror(-ret));
        goto done_release_lock;
    }
    ret = faults_parse(fs->control_buf, &faults);
    if (ret < 0) {
        INFO("kibosh_fs_accessor_fd_release: faults_parse failed: error %d (%s)\n",
                -ret, safe_strerror(-ret));
        goto done_release_lock;
    }
    fs->faults = faults;
    swap_ints(&fd, &fs->control_fd);
    INFO("kibosh_fs_accessor_fd_release: refreshed faults: %s\n", fs->control_buf);
    ret = 0;
done_release_lock:
    pthread_mutex_unlock(&fs->lock);
done_close_fd:
    close(fd);
    return ret;
}

int kibosh_fs_check_read_fault(struct kibosh_fs *fs, const char *path)
{
    int ret;
    pthread_mutex_lock(&fs->lock);
    ret = faults_check(fs->faults, path, "read");
    pthread_mutex_unlock(&fs->lock);
    return ret;
}

// vim: ts=4:sw=4:tw=99:et