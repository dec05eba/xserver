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

#define GLAMOR_FOR_XORG
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include "glamor_priv.h"
#include "glamor_egl_copy_thread.h"

struct glamor_egl_copy_image {
    uint64_t id;
    EGLImageKHR image;
    Bool external;
    int width;
    int height;
};

struct glamor_egl_copy_source {
    PixmapPtr pixmap;
    struct glamor_egl_copy_image image;
    int origin_x;
    int origin_y;
    int nbox;
    BoxPtr boxes;
};

struct glamor_egl_copy_result {
    Bool copied;
    int fence_fd;
};

struct glamor_egl_copy_callback {
    glamor_egl_copy_done_proc proc;
    void *data;
};

struct glamor_egl_copy_job {
    struct xorg_list link;
    PixmapPtr dst_pixmap;
    struct glamor_egl_copy_image dst;
    int wait_fence_fd;
    Bool fence;
    struct glamor_egl_copy_result result;
    struct glamor_egl_copy_callback callback;
    BoxPtr boxes;
    int nsrc;
    struct glamor_egl_copy_source srcs[];
};

struct glamor_egl_copy_ids {
    uint64_t *ids;
    int count;
    int size;
};

struct glamor_egl_copy_shared {
    pthread_mutex_t lock;
    pthread_cond_t wake;
    pthread_cond_t idle;
    Bool quit;
    struct xorg_list queued;
    struct glamor_egl_copy_job *running;
    struct xorg_list done;
    struct glamor_egl_copy_ids forgotten;
};

struct glamor_egl_copy_target {
    uint64_t id;
    uint64_t used;
    GLenum tex_target;
    GLuint tex;
    GLuint fb;
};

struct glamor_egl_copy_cache {
    struct glamor_egl_copy_target targets[16];
    uint64_t use_count;
};

struct glamor_egl_copy_program {
    GLuint program;
    GLint dst_size;
    GLint src_origin;
    GLint src_size;
};

struct glamor_egl_copy_renderer {
    struct glamor_egl_copy_program texture_2d;
    struct glamor_egl_copy_program external;
    GLuint vbo;
    GLfloat *vertices;
    int vertices_size;
};

struct glamor_egl_copy_worker {
    Bool ready;
    struct glamor_egl_copy_renderer renderer;
    struct glamor_egl_copy_cache cache;
};

struct glamor_egl_copy_thread {
    EGLDisplay display;
    EGLContext context;
    Bool has_native_fence;
    Bool failed;
    pthread_t thread;
    int event_fd;
    struct glamor_egl_copy_shared shared;
    struct glamor_egl_copy_worker worker;
};

static const char glamor_egl_copy_vs[] =
    "#version 300 es\n"
    "in vec2 position;\n"
    "uniform vec2 dst_size;\n"
    "void main() {\n"
    "    gl_Position = vec4(position / dst_size * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

#define GLAMOR_EGL_COPY_FS(extension, sampler)                                  \
    "#version 300 es\n"                                                         \
    extension                                                                   \
    "precision highp float;\n"                                                  \
    "uniform highp " sampler " src;\n"                                          \
    "uniform vec2 src_origin;\n"                                                \
    "uniform vec2 src_size;\n"                                                  \
    "out vec4 color;\n"                                                         \
    "void main() {\n"                                                           \
    "    color = texture(src, (gl_FragCoord.xy - src_origin) / src_size);\n"    \
    "}\n"

static const char glamor_egl_copy_fs_2d[] =
    GLAMOR_EGL_COPY_FS("", "sampler2D");

static const char glamor_egl_copy_fs_external[] =
    GLAMOR_EGL_COPY_FS("#extension GL_OES_EGL_image_external_essl3 : require\n",
                       "samplerExternalOES");

static GLuint
glamor_egl_copy_shader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    GLint compiled;

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint
glamor_egl_copy_link(const char *fs_source)
{
    GLuint vs = glamor_egl_copy_shader(GL_VERTEX_SHADER, glamor_egl_copy_vs);
    GLuint fs = glamor_egl_copy_shader(GL_FRAGMENT_SHADER, fs_source);
    GLuint program = 0;
    GLint linked = GL_FALSE;

    if (vs && fs) {
        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glBindAttribLocation(program, 0, "position");
        glLinkProgram(program);
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);

    if (!linked && program) {
        glDeleteProgram(program);
        program = 0;
    }
    return program;
}

static Bool
glamor_egl_copy_program_init(struct glamor_egl_copy_program *program,
                             const char *fs_source)
{
    program->program = glamor_egl_copy_link(fs_source);
    if (!program->program)
        return FALSE;

    glUseProgram(program->program);
    glUniform1i(glGetUniformLocation(program->program, "src"), 0);
    program->dst_size = glGetUniformLocation(program->program, "dst_size");
    program->src_origin = glGetUniformLocation(program->program, "src_origin");
    program->src_size = glGetUniformLocation(program->program, "src_size");
    return TRUE;
}

static Bool
glamor_egl_copy_renderer_init(struct glamor_egl_copy_renderer *renderer)
{
    if (!glamor_egl_copy_program_init(&renderer->texture_2d,
                                      glamor_egl_copy_fs_2d))
        return FALSE;

    if (epoxy_has_gl_extension("GL_OES_EGL_image_external_essl3"))
        glamor_egl_copy_program_init(&renderer->external,
                                     glamor_egl_copy_fs_external);

    glGenBuffers(1, &renderer->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, renderer->vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    glEnableVertexAttribArray(0);
    glDisable(GL_DITHER);
    return glGetError() == GL_NO_ERROR;
}

static void
glamor_egl_copy_renderer_fini(struct glamor_egl_copy_renderer *renderer)
{
    glDeleteBuffers(1, &renderer->vbo);
    glDeleteProgram(renderer->texture_2d.program);
    glDeleteProgram(renderer->external.program);
    free(renderer->vertices);
    *renderer = (struct glamor_egl_copy_renderer) { 0 };
}

static GLfloat *
glamor_egl_copy_renderer_vertices(struct glamor_egl_copy_renderer *renderer,
                                  int count)
{
    GLfloat *grown;

    if (count <= renderer->vertices_size)
        return renderer->vertices;

    grown = reallocarray(renderer->vertices, count, sizeof(*grown));
    if (!grown)
        return NULL;
    renderer->vertices = grown;
    renderer->vertices_size = count;
    return grown;
}

static Bool
glamor_egl_copy_clip(const struct glamor_egl_copy_image *dst,
                     const struct glamor_egl_copy_source *src,
                     const BoxRec *box, BoxPtr clip)
{
    *clip = (BoxRec) {
        .x1 = max(max(box->x1, 0), src->origin_x),
        .y1 = max(max(box->y1, 0), src->origin_y),
        .x2 = min(min(box->x2, dst->width), src->origin_x + src->image.width),
        .y2 = min(min(box->y2, dst->height), src->origin_y + src->image.height),
    };
    return clip->x1 < clip->x2 && clip->y1 < clip->y2;
}

static void
glamor_egl_copy_box_vertices(GLfloat *vertices, const BoxRec *box)
{
    const GLfloat x1 = box->x1, y1 = box->y1, x2 = box->x2, y2 = box->y2;
    const GLfloat quad[] = { x1, y1, x2, y1, x1, y2, x2, y1, x2, y2, x1, y2 };

    memcpy(vertices, quad, sizeof(quad));
}

static Bool
glamor_egl_copy_renderer_draw(struct glamor_egl_copy_renderer *renderer,
                              const struct glamor_egl_copy_image *dst,
                              const struct glamor_egl_copy_source *src,
                              const struct glamor_egl_copy_target *read)
{
    const struct glamor_egl_copy_program *program =
        src->image.external ? &renderer->external : &renderer->texture_2d;
    GLfloat *vertices = glamor_egl_copy_renderer_vertices(renderer, src->nbox * 12);
    int i, quads = 0;

    if (!program->program || !vertices)
        return FALSE;

    for (i = 0; i < src->nbox; i++) {
        BoxRec clip;

        if (glamor_egl_copy_clip(dst, src, &src->boxes[i], &clip))
            glamor_egl_copy_box_vertices(vertices + quads++ * 12, &clip);
    }
    if (!quads)
        return TRUE;

    glUseProgram(program->program);
    glUniform2f(program->dst_size, dst->width, dst->height);
    glUniform2f(program->src_origin, src->origin_x, src->origin_y);
    glUniform2f(program->src_size, src->image.width, src->image.height);
    glBindTexture(read->tex_target, read->tex);
    glBufferData(GL_ARRAY_BUFFER, quads * 12 * sizeof(GLfloat), vertices,
                 GL_STREAM_DRAW);
    glDrawArrays(GL_TRIANGLES, 0, quads * 6);
    return TRUE;
}

static void
glamor_egl_copy_target_release(struct glamor_egl_copy_target *target)
{
    if (target->fb)
        glDeleteFramebuffers(1, &target->fb);
    if (target->tex)
        glDeleteTextures(1, &target->tex);
    *target = (struct glamor_egl_copy_target) { 0 };
}

static Bool
glamor_egl_copy_target_init(struct glamor_egl_copy_target *target,
                            const struct glamor_egl_copy_image *image)
{
    target->tex_target = image->external ? GL_TEXTURE_EXTERNAL_OES :
        GL_TEXTURE_2D;
    glGenTextures(1, &target->tex);
    glBindTexture(target->tex_target, target->tex);
    glTexParameteri(target->tex_target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(target->tex_target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEGLImageTargetTexture2DOES(target->tex_target, image->image);
    return glGetError() == GL_NO_ERROR;
}

static Bool
glamor_egl_copy_target_bind_framebuffer(struct glamor_egl_copy_target *target)
{
    if (target->fb) {
        glBindFramebuffer(GL_FRAMEBUFFER, target->fb);
        return TRUE;
    }

    glGenFramebuffers(1, &target->fb);
    glBindFramebuffer(GL_FRAMEBUFFER, target->fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           target->tex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        glDeleteFramebuffers(1, &target->fb);
        target->fb = 0;
        return FALSE;
    }
    return TRUE;
}

static struct glamor_egl_copy_target *
glamor_egl_copy_cache_target(struct glamor_egl_copy_cache *cache,
                             const struct glamor_egl_copy_image *image)
{
    struct glamor_egl_copy_target *oldest = &cache->targets[0];
    int i;

    for (i = 0; i < ARRAY_SIZE(cache->targets); i++) {
        struct glamor_egl_copy_target *target = &cache->targets[i];

        if (target->id == image->id) {
            target->used = ++cache->use_count;
            return target;
        }
        if (target->used < oldest->used)
            oldest = target;
    }

    glamor_egl_copy_target_release(oldest);
    if (!glamor_egl_copy_target_init(oldest, image)) {
        glamor_egl_copy_target_release(oldest);
        return NULL;
    }

    oldest->id = image->id;
    oldest->used = ++cache->use_count;
    return oldest;
}

static void
glamor_egl_copy_cache_forget(struct glamor_egl_copy_cache *cache,
                             struct glamor_egl_copy_ids *ids)
{
    int i, j;

    for (i = 0; i < ids->count; i++) {
        for (j = 0; j < ARRAY_SIZE(cache->targets); j++) {
            if (cache->targets[j].id == ids->ids[i])
                glamor_egl_copy_target_release(&cache->targets[j]);
        }
    }
    ids->count = 0;
}

static void
glamor_egl_copy_cache_clear(struct glamor_egl_copy_cache *cache)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(cache->targets); i++)
        glamor_egl_copy_target_release(&cache->targets[i]);
}

static void
glamor_egl_copy_ids_append(struct glamor_egl_copy_ids *ids, uint64_t id)
{
    if (ids->count == ids->size) {
        int size = ids->size ? ids->size * 2 : 16;
        uint64_t *grown = reallocarray(ids->ids, size, sizeof(*grown));

        if (!grown)
            return;
        ids->ids = grown;
        ids->size = size;
    }
    ids->ids[ids->count++] = id;
}

static void
glamor_egl_copy_ids_swap(struct glamor_egl_copy_ids *a,
                         struct glamor_egl_copy_ids *b)
{
    struct glamor_egl_copy_ids tmp = *a;

    *a = *b;
    *b = tmp;
}

static void
glamor_egl_copy_wait_fence(int fence_fd)
{
    struct pollfd pfd = { .fd = fence_fd, .events = POLLIN };

    while (poll(&pfd, 1, -1) < 0 && errno == EINTR)
        ;
    close(fence_fd);
}

static Bool
glamor_egl_copy_worker_draw(struct glamor_egl_copy_worker *worker,
                            const struct glamor_egl_copy_job *job)
{
    struct glamor_egl_copy_target *dst;
    int i;

    if (!worker->ready)
        return FALSE;

    dst = glamor_egl_copy_cache_target(&worker->cache, &job->dst);
    if (!dst || !glamor_egl_copy_target_bind_framebuffer(dst))
        return FALSE;

    glViewport(0, 0, job->dst.width, job->dst.height);
    for (i = 0; i < job->nsrc; i++) {
        const struct glamor_egl_copy_source *src = &job->srcs[i];
        struct glamor_egl_copy_target *read;

        if (!src->nbox)
            continue;

        read = glamor_egl_copy_cache_target(&worker->cache, &src->image);
        if (!read ||
            !glamor_egl_copy_renderer_draw(&worker->renderer, &job->dst, src,
                                           read))
            return FALSE;
    }
    return glGetError() == GL_NO_ERROR;
}

static int
glamor_egl_copy_thread_fence(struct glamor_egl_copy_thread *thread,
                             Bool want_fd)
{
    EGLSyncKHR sync = EGL_NO_SYNC_KHR;
    int fd;

    if (want_fd && thread->has_native_fence)
        sync = eglCreateSyncKHR(thread->display,
                                EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
    if (sync == EGL_NO_SYNC_KHR) {
        glFinish();
        return -1;
    }

    glFlush();
    fd = eglDupNativeFenceFDANDROID(thread->display, sync);
    eglDestroySyncKHR(thread->display, sync);
    if (fd < 0)
        glFinish();
    return fd;
}

static void
glamor_egl_copy_thread_run(struct glamor_egl_copy_thread *thread,
                           struct glamor_egl_copy_job *job)
{
    if (job->wait_fence_fd >= 0) {
        glamor_egl_copy_wait_fence(job->wait_fence_fd);
        job->wait_fence_fd = -1;
    }

    job->result.copied = glamor_egl_copy_worker_draw(&thread->worker, job);
    if (job->result.copied)
        job->result.fence_fd = glamor_egl_copy_thread_fence(thread, job->fence);
}

static void
glamor_egl_copy_thread_set_name(void)
{
#if defined(HAVE_PTHREAD_SETNAME_NP_WITH_TID)
    pthread_setname_np(pthread_self(), "GlamorCopy");
#elif defined(HAVE_PTHREAD_SETNAME_NP_WITHOUT_TID)
    pthread_setname_np("GlamorCopy");
#endif
}

static void *
glamor_egl_copy_thread_main(void *data)
{
    struct glamor_egl_copy_thread *thread = data;
    struct glamor_egl_copy_shared *shared = &thread->shared;
    struct glamor_egl_copy_worker *worker = &thread->worker;
    struct glamor_egl_copy_ids forgotten = { 0 };
    Bool current;

    glamor_egl_copy_thread_set_name();
    eglBindAPI(EGL_OPENGL_ES_API);
    current = eglMakeCurrent(thread->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                             thread->context);
    worker->ready = current &&
        glamor_egl_copy_renderer_init(&worker->renderer);

    pthread_mutex_lock(&shared->lock);
    while (!shared->quit) {
        struct glamor_egl_copy_job *job = NULL;

        if (!shared->forgotten.count && xorg_list_is_empty(&shared->queued)) {
            pthread_cond_wait(&shared->wake, &shared->lock);
            continue;
        }

        glamor_egl_copy_ids_swap(&forgotten, &shared->forgotten);
        if (!xorg_list_is_empty(&shared->queued)) {
            job = xorg_list_first_entry(&shared->queued,
                                        struct glamor_egl_copy_job, link);
            xorg_list_del(&job->link);
        }
        shared->running = job;
        pthread_mutex_unlock(&shared->lock);

        glamor_egl_copy_cache_forget(&worker->cache, &forgotten);
        if (job)
            glamor_egl_copy_thread_run(thread, job);

        pthread_mutex_lock(&shared->lock);
        if (job) {
            shared->running = NULL;
            xorg_list_append(&job->link, &shared->done);
            pthread_cond_broadcast(&shared->idle);
            eventfd_write(thread->event_fd, 1);
        }
    }
    pthread_mutex_unlock(&shared->lock);

    if (current) {
        glamor_egl_copy_cache_clear(&worker->cache);
        glamor_egl_copy_renderer_fini(&worker->renderer);
        eglMakeCurrent(thread->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                       EGL_NO_CONTEXT);
    }
    free(forgotten.ids);
    return NULL;
}

static void
glamor_egl_copy_job_free(struct glamor_egl_copy_job *job)
{
    int i;

    if (job->wait_fence_fd >= 0)
        close(job->wait_fence_fd);
    dixDestroyPixmap(job->dst_pixmap, 0);
    for (i = 0; i < job->nsrc; i++)
        dixDestroyPixmap(job->srcs[i].pixmap, 0);
    free(job->boxes);
    free(job);
}

static void
glamor_egl_copy_job_drop(struct glamor_egl_copy_job *job)
{
    xorg_list_del(&job->link);
    if (job->result.fence_fd >= 0)
        close(job->result.fence_fd);
    glamor_egl_copy_job_free(job);
}

static void
glamor_egl_copy_thread_complete(struct glamor_egl_copy_thread *thread)
{
    struct glamor_egl_copy_shared *shared = &thread->shared;
    struct glamor_egl_copy_job *job, *tmp;
    struct xorg_list done;

    xorg_list_init(&done);
    pthread_mutex_lock(&shared->lock);
    xorg_list_for_each_entry_safe(job, tmp, &shared->done, link) {
        xorg_list_del(&job->link);
        xorg_list_append(&job->link, &done);
    }
    pthread_mutex_unlock(&shared->lock);

    xorg_list_for_each_entry_safe(job, tmp, &done, link) {
        xorg_list_del(&job->link);
        if (!job->result.copied && !thread->failed) {
            LogMessage(X_WARNING,
                       "glamor: Copy thread failed, copying on the main thread instead\n");
            thread->failed = TRUE;
        }
        job->callback.proc(job->result.fence_fd, job->result.copied,
                           job->callback.data);
        glamor_egl_copy_job_free(job);
    }
}

static void
glamor_egl_copy_thread_notified(int fd, int ready, void *data)
{
    eventfd_t count;

    eventfd_read(fd, &count);
    glamor_egl_copy_thread_complete(data);
}

static Bool
glamor_egl_copy_image_get(PixmapPtr pixmap, struct glamor_egl_copy_image *image)
{
    struct glamor_pixmap_private *priv = glamor_get_pixmap_private(pixmap);

    *image = (struct glamor_egl_copy_image) {
        .id = priv->image_info.id,
        .image = priv->image,
        .external = glamor_pixmap_is_external(pixmap),
        .width = pixmap->drawable.width,
        .height = pixmap->drawable.height,
    };
    return image->id && image->image != EGL_NO_IMAGE_KHR;
}

static Bool
glamor_egl_copy_job_init(struct glamor_egl_copy_job *job, PixmapPtr dst,
                         const glamor_egl_copy_op *ops, int nops)
{
    BoxPtr boxes = job->boxes;
    int i;

    if (!glamor_egl_copy_image_get(dst, &job->dst) || job->dst.external)
        return FALSE;

    for (i = 0; i < nops; i++) {
        struct glamor_egl_copy_source *src = &job->srcs[i];

        if (ops[i].src->drawable.pScreen != dst->drawable.pScreen ||
            !glamor_egl_copy_image_get(ops[i].src, &src->image))
            return FALSE;

        src->origin_x = ops[i].src_origin_x;
        src->origin_y = ops[i].src_origin_y;
        src->nbox = RegionNumRects(ops[i].region);
        src->boxes = boxes;
        memcpy(boxes, RegionRects(ops[i].region), src->nbox * sizeof(BoxRec));
        boxes += src->nbox;
    }
    return TRUE;
}

static void
glamor_egl_copy_job_ref_pixmaps(struct glamor_egl_copy_job *job, PixmapPtr dst,
                                const glamor_egl_copy_op *ops, int nops)
{
    int i;

    job->dst_pixmap = dst;
    dst->refcnt++;
    for (i = 0; i < nops; i++) {
        job->srcs[i].pixmap = ops[i].src;
        ops[i].src->refcnt++;
    }
    job->nsrc = nops;
}

struct glamor_egl_copy_job *
glamor_egl_copy_job_create(struct glamor_egl_copy_thread *thread,
                           PixmapPtr dst, const glamor_egl_copy_op *ops,
                           int nops, Bool fence,
                           glamor_egl_copy_done_proc done, void *data)
{
    struct glamor_egl_copy_job *job;
    int i, nbox = 0;

    if (thread->failed)
        return NULL;

    for (i = 0; i < nops; i++)
        nbox += RegionNumRects(ops[i].region);

    job = calloc(1, sizeof(*job) + nops * sizeof(job->srcs[0]));
    if (!job)
        return NULL;

    job->boxes = xallocarray(max(nbox, 1), sizeof(BoxRec));
    if (!job->boxes || !glamor_egl_copy_job_init(job, dst, ops, nops)) {
        free(job->boxes);
        free(job);
        return NULL;
    }

    glamor_egl_copy_job_ref_pixmaps(job, dst, ops, nops);
    job->wait_fence_fd = -1;
    job->fence = fence;
    job->result.fence_fd = -1;
    job->callback = (struct glamor_egl_copy_callback) { done, data };
    return job;
}

void
glamor_egl_copy_thread_queue(struct glamor_egl_copy_thread *thread,
                             struct glamor_egl_copy_job *job,
                             int wait_fence_fd)
{
    struct glamor_egl_copy_shared *shared = &thread->shared;

    job->wait_fence_fd = wait_fence_fd;
    pthread_mutex_lock(&shared->lock);
    xorg_list_append(&job->link, &shared->queued);
    pthread_cond_signal(&shared->wake);
    pthread_mutex_unlock(&shared->lock);
}

static void
glamor_egl_copy_jobs_take(struct xorg_list *jobs, struct xorg_list *taken,
                          void *data)
{
    struct glamor_egl_copy_job *job, *tmp;

    xorg_list_for_each_entry_safe(job, tmp, jobs, link) {
        if (job->callback.data != data)
            continue;
        xorg_list_del(&job->link);
        xorg_list_append(&job->link, taken);
    }
}

void
glamor_egl_copy_thread_cancel(struct glamor_egl_copy_thread *thread,
                              void *data)
{
    struct glamor_egl_copy_shared *shared = &thread->shared;
    struct glamor_egl_copy_job *job, *tmp;
    struct xorg_list cancelled;

    xorg_list_init(&cancelled);
    pthread_mutex_lock(&shared->lock);
    glamor_egl_copy_jobs_take(&shared->queued, &cancelled, data);
    while (shared->running && shared->running->callback.data == data)
        pthread_cond_wait(&shared->idle, &shared->lock);
    glamor_egl_copy_jobs_take(&shared->done, &cancelled, data);
    pthread_mutex_unlock(&shared->lock);

    xorg_list_for_each_entry_safe(job, tmp, &cancelled, link)
        glamor_egl_copy_job_drop(job);
}

static Bool
glamor_egl_copy_job_uses(const struct glamor_egl_copy_job *job, uint64_t id)
{
    int i;

    if (job->dst.id == id)
        return TRUE;
    for (i = 0; i < job->nsrc; i++) {
        if (job->srcs[i].image.id == id)
            return TRUE;
    }
    return FALSE;
}

static Bool
glamor_egl_copy_thread_uses(struct glamor_egl_copy_thread *thread, uint64_t id)
{
    struct glamor_egl_copy_shared *shared = &thread->shared;
    struct glamor_egl_copy_job *job;

    if (shared->running && glamor_egl_copy_job_uses(shared->running, id))
        return TRUE;
    xorg_list_for_each_entry(job, &shared->queued, link) {
        if (glamor_egl_copy_job_uses(job, id))
            return TRUE;
    }
    return FALSE;
}

void
glamor_egl_copy_thread_forget_image(struct glamor_egl_copy_thread *thread,
                                    uint64_t image_id)
{
    struct glamor_egl_copy_shared *shared = &thread->shared;

    if (!image_id)
        return;

    pthread_mutex_lock(&shared->lock);
    while (glamor_egl_copy_thread_uses(thread, image_id))
        pthread_cond_wait(&shared->idle, &shared->lock);
    glamor_egl_copy_ids_append(&shared->forgotten, image_id);
    pthread_cond_signal(&shared->wake);
    pthread_mutex_unlock(&shared->lock);
}

static Bool
glamor_egl_copy_thread_start(struct glamor_egl_copy_thread *thread)
{
    sigset_t blocked, saved;
    int ret;

    sigfillset(&blocked);
    pthread_sigmask(SIG_BLOCK, &blocked, &saved);
    ret = pthread_create(&thread->thread, NULL, glamor_egl_copy_thread_main,
                         thread);
    pthread_sigmask(SIG_SETMASK, &saved, NULL);
    return ret == 0;
}

static void
glamor_egl_copy_shared_init(struct glamor_egl_copy_shared *shared)
{
    pthread_mutex_init(&shared->lock, NULL);
    pthread_cond_init(&shared->wake, NULL);
    pthread_cond_init(&shared->idle, NULL);
    xorg_list_init(&shared->queued);
    xorg_list_init(&shared->done);
}

static void
glamor_egl_copy_shared_fini(struct glamor_egl_copy_shared *shared)
{
    struct glamor_egl_copy_job *job, *tmp;

    xorg_list_for_each_entry_safe(job, tmp, &shared->queued, link)
        glamor_egl_copy_job_drop(job);
    xorg_list_for_each_entry_safe(job, tmp, &shared->done, link)
        glamor_egl_copy_job_drop(job);
    free(shared->forgotten.ids);
    pthread_cond_destroy(&shared->idle);
    pthread_cond_destroy(&shared->wake);
    pthread_mutex_destroy(&shared->lock);
}

struct glamor_egl_copy_thread *
glamor_egl_copy_thread_create(EGLDisplay egl_display)
{
    static const EGLint attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE
    };
    struct glamor_egl_copy_thread *thread;
    EGLenum api = eglQueryAPI();

    thread = calloc(1, sizeof(*thread));
    if (!thread)
        return NULL;

    thread->display = egl_display;
    thread->has_native_fence =
        epoxy_has_egl_extension(egl_display, "EGL_ANDROID_native_fence_sync");
    eglBindAPI(EGL_OPENGL_ES_API);
    thread->context = glamor_egl_create_context(egl_display, attribs);
    eglBindAPI(api);
    if (thread->context == EGL_NO_CONTEXT)
        goto fail;

    thread->event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (thread->event_fd < 0)
        goto fail_context;

    glamor_egl_copy_shared_init(&thread->shared);
    if (!SetNotifyFd(thread->event_fd, glamor_egl_copy_thread_notified,
                     X_NOTIFY_READ, thread))
        goto fail_event_fd;
    if (!glamor_egl_copy_thread_start(thread))
        goto fail_notify;

    return thread;

fail_notify:
    RemoveNotifyFd(thread->event_fd);
fail_event_fd:
    glamor_egl_copy_shared_fini(&thread->shared);
    close(thread->event_fd);
fail_context:
    eglDestroyContext(egl_display, thread->context);
fail:
    free(thread);
    return NULL;
}

void
glamor_egl_copy_thread_destroy(struct glamor_egl_copy_thread *thread)
{
    struct glamor_egl_copy_shared *shared = &thread->shared;

    pthread_mutex_lock(&shared->lock);
    shared->quit = TRUE;
    pthread_cond_signal(&shared->wake);
    pthread_mutex_unlock(&shared->lock);
    pthread_join(thread->thread, NULL);

    RemoveNotifyFd(thread->event_fd);
    glamor_egl_copy_shared_fini(shared);
    close(thread->event_fd);
    eglDestroyContext(thread->display, thread->context);
    free(thread);
}
