/*
 * Copyright © 2026 dec05eba
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <dix-config.h>

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "present_priv.h"

#define PRESENT_GPU_TIME_RESCAN_USEC 10000000

static const char *present_gpu_time_engines[] = { "gfx", "compute", "render" };

static void
present_gpu_time_scan(present_gpu_time_ptr gpu_time, pid_t pid)
{
    struct dirent *entry;
    char path[32];
    DIR *dir;

    gpu_time->num_fds = 0;
    snprintf(path, sizeof(path), "/proc/%d/fd", (int) pid);
    dir = opendir(path);
    if (!dir)
        return;

    while ((entry = readdir(dir)) && gpu_time->num_fds < PRESENT_GPU_TIME_MAX_FDS) {
        char target[32];
        ssize_t len = readlinkat(dirfd(dir), entry->d_name, target, sizeof(target) - 1);

        if (len <= 0)
            continue;

        target[len] = '\0';
        if (!strncmp(target, "/dev/dri/", strlen("/dev/dri/")))
            gpu_time->fds[gpu_time->num_fds++] = atoi(entry->d_name);
    }
    closedir(dir);
}

static Bool
present_gpu_time_engine_counted(const char *engine)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(present_gpu_time_engines); i++) {
        if (!strcmp(engine, present_gpu_time_engines[i]))
            return TRUE;
    }
    return FALSE;
}

static Bool
present_gpu_time_read_fd(pid_t pid, int fd, uint64_t *client_id, uint64_t *ns, Bool *reported)
{
    char path[64], buf[4096], *line, *save;
    Bool drm = FALSE;
    ssize_t len;
    int info;

    snprintf(path, sizeof(path), "/proc/%d/fdinfo/%d", (int) pid, fd);
    info = open(path, O_RDONLY | O_CLOEXEC);
    if (info < 0)
        return FALSE;

    len = read(info, buf, sizeof(buf) - 1);
    close(info);
    if (len <= 0)
        return FALSE;
    buf[len] = '\0';

    *ns = 0;
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        unsigned long long value;
        char key[64];

        if (sscanf(line, "%63[^:]:%llu", key, &value) != 2)
            continue;

        if (!strcmp(key, "drm-client-id")) {
            *client_id = value;
            drm = TRUE;
        } else if (!strncmp(key, "drm-engine-", strlen("drm-engine-")) &&
                   present_gpu_time_engine_counted(key + strlen("drm-engine-"))) {
            *ns += value;
            *reported = TRUE;
        }
    }
    return drm;
}

Bool
present_gpu_time_read(present_gpu_time_ptr gpu_time, pid_t pid, uint64_t now, uint64_t *ns)
{
    uint64_t client_ids[PRESENT_GPU_TIME_MAX_FDS];
    int num_client_ids = 0, i, j;
    Bool reported = FALSE;

    if (pid <= 0)
        return FALSE;

    if (!gpu_time->scanned_usec || now - gpu_time->scanned_usec >= PRESENT_GPU_TIME_RESCAN_USEC) {
        present_gpu_time_scan(gpu_time, pid);
        gpu_time->scanned_usec = now;
    }

    *ns = 0;
    for (i = 0; i < gpu_time->num_fds; i++) {
        uint64_t client_id, fd_ns;

        if (!present_gpu_time_read_fd(pid, gpu_time->fds[i], &client_id, &fd_ns, &reported)) {
            gpu_time->scanned_usec = 0;
            continue;
        }

        for (j = 0; j < num_client_ids && client_ids[j] != client_id; j++)
            ;
        if (j < num_client_ids)
            continue;

        client_ids[num_client_ids++] = client_id;
        *ns += fd_ns;
    }
    return reported;
}
