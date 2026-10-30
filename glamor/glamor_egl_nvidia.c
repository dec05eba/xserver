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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "dix-config.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>

#include "glamor_priv.h"
#include "glamor_egl_nvidia.h"

/* From nvidia-drm-ioctl.h in NVIDIA's kernel modules. Importing a dma-buf
 * gives NVIDIA's own buffers back as NVKMS or user memory objects, and
 * buffers of other drivers as DMABUF objects.
 */
#define DRM_NVIDIA_GEM_IDENTIFY_OBJECT 0x0e

enum {
    NV_GEM_OBJECT_NVKMS,
    NV_GEM_OBJECT_DMABUF,
    NV_GEM_OBJECT_USERMEMORY,
};

struct drm_nvidia_gem_identify_object_params {
    uint32_t handle;
    uint32_t object_type;
};

#define DRM_IOCTL_NVIDIA_GEM_IDENTIFY_OBJECT                        \
    DRM_IOWR(DRM_COMMAND_BASE + DRM_NVIDIA_GEM_IDENTIFY_OBJECT,     \
             struct drm_nvidia_gem_identify_object_params)

int
glamor_egl_nvidia_open(int fd)
{
    drmVersionPtr version = drmGetVersion(fd);
    Bool nvidia = version && strcmp(version->name, "nvidia-drm") == 0;
    char *path;
    int nvidia_fd;

    drmFreeVersion(version);
    if (!nvidia)
        return -1;

    path = drmGetRenderDeviceNameFromFd(fd);
    if (!path)
        return -1;

    nvidia_fd = open(path, O_RDWR | O_CLOEXEC);
    free(path);
    return nvidia_fd;
}

Bool
glamor_egl_nvidia_owns_dmabuf(int nvidia_fd, int dmabuf_fd)
{
    struct drm_nvidia_gem_identify_object_params params = { 0 };
    Bool owns;

    if (drmPrimeFDToHandle(nvidia_fd, dmabuf_fd, &params.handle))
        return FALSE;

    owns = drmIoctl(nvidia_fd, DRM_IOCTL_NVIDIA_GEM_IDENTIFY_OBJECT,
                    &params) == 0 &&
        (params.object_type == NV_GEM_OBJECT_NVKMS ||
         params.object_type == NV_GEM_OBJECT_USERMEMORY);
    drmCloseBufferHandle(nvidia_fd, params.handle);
    return owns;
}
