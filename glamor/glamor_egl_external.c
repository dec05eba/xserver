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

#include <unistd.h>
#include <linux/dma-buf.h>
#include <xf86drm.h>

#include "glamor_priv.h"
#include "glamor_egl_external.h"

struct glamor_egl_external_target {
    EGLImageKHR image;
    GLuint tex;
    GLuint fb;
};

struct glamor_egl_external_context {
    ScreenPtr screen;
    EGLDisplay display;
    EGLContext context;
    GLuint program;
    Bool has_wait_sync;
    Bool has_native_fence;
    unsigned long generation;
    struct glamor_egl_external_target targets[4];
    unsigned int next_target;
};

struct glamor_egl_external_pixmap {
    glamor_external base;
    struct glamor_egl_external_context *context;
    GLuint src_tex;
    GLuint dst_tex;
    GLuint fb;
    int width;
    int height;
    int dmabuf_fd;
    unsigned long generation;
    RegionRec updated;
};

static const char glamor_egl_external_vs[] =
    "attribute vec2 position;\n"
    "attribute vec2 source;\n"
    "varying vec2 texcoord;\n"
    "void main() {\n"
    "    texcoord = source;\n"
    "    gl_Position = vec4(position * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

static const char glamor_egl_external_fs[] =
    "#extension GL_OES_EGL_image_external : require\n"
    "precision highp float;\n"
    "uniform samplerExternalOES sampler;\n"
    "varying vec2 texcoord;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(sampler, texcoord);\n"
    "}\n";

static void
glamor_egl_external_enter(struct glamor_egl_external_context *context)
{
    eglMakeCurrent(context->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    eglBindAPI(EGL_OPENGL_ES_API);
    if (!eglMakeCurrent(context->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                        context->context))
        FatalError("Failed to make external EGL context current\n");
    lastGLContext = context->context;
}

static void
glamor_egl_external_leave(struct glamor_egl_external_context *context)
{
    glamor_screen_private *glamor_priv =
        glamor_get_screen_private(context->screen);

    eglMakeCurrent(context->display, EGL_NO_SURFACE, EGL_NO_SURFACE,
                   EGL_NO_CONTEXT);
    eglBindAPI(glamor_priv->is_gles ? EGL_OPENGL_ES_API : EGL_OPENGL_API);
    glamor_make_current(glamor_priv);
}

static EGLSyncKHR
glamor_egl_external_fence(struct glamor_egl_external_context *context)
{
    EGLSyncKHR sync = EGL_NO_SYNC_KHR;

    if (context->has_wait_sync)
        sync = eglCreateSyncKHR(context->display, EGL_SYNC_FENCE_KHR, NULL);

    if (sync == EGL_NO_SYNC_KHR)
        glFinish();
    else
        glFlush();

    return sync;
}

static void
glamor_egl_external_wait(struct glamor_egl_external_context *context,
                         EGLSyncKHR sync)
{
    if (sync == EGL_NO_SYNC_KHR)
        return;

    eglWaitSyncKHR(context->display, sync, 0);
    eglDestroySyncKHR(context->display, sync);
}

static void
glamor_egl_external_wait_writers(struct glamor_egl_external_pixmap *pixmap)
{
#ifdef DMA_BUF_IOCTL_EXPORT_SYNC_FILE
    struct glamor_egl_external_context *context = pixmap->context;
    struct dma_buf_export_sync_file args = {
        .flags = DMA_BUF_SYNC_READ,
        .fd = -1,
    };
    EGLint attribs[] = { EGL_SYNC_NATIVE_FENCE_FD_ANDROID, -1, EGL_NONE };
    EGLSyncKHR sync;

    if (pixmap->dmabuf_fd < 0 || !context->has_native_fence ||
        drmIoctl(pixmap->dmabuf_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &args) != 0)
        return;

    attribs[1] = args.fd;
    sync = eglCreateSyncKHR(context->display, EGL_SYNC_NATIVE_FENCE_ANDROID,
                            attribs);
    if (sync == EGL_NO_SYNC_KHR) {
        close(args.fd);
        return;
    }
    glamor_egl_external_wait(context, sync);
#endif
}

static void
glamor_egl_external_add_reader(struct glamor_egl_external_pixmap *pixmap)
{
#ifdef DMA_BUF_IOCTL_IMPORT_SYNC_FILE
    struct glamor_egl_external_context *context = pixmap->context;
    struct dma_buf_import_sync_file args = { .flags = DMA_BUF_SYNC_READ };
    EGLSyncKHR sync;

    if (pixmap->dmabuf_fd < 0 || !context->has_native_fence)
        return;

    sync = eglCreateSyncKHR(context->display, EGL_SYNC_NATIVE_FENCE_ANDROID,
                            NULL);
    if (sync == EGL_NO_SYNC_KHR)
        return;

    glFlush();
    args.fd = eglDupNativeFenceFDANDROID(context->display, sync);
    eglDestroySyncKHR(context->display, sync);
    if (args.fd < 0)
        return;

    drmIoctl(pixmap->dmabuf_fd, DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &args);
    close(args.fd);
#endif
}

static GLuint
glamor_egl_external_texture(GLenum target, EGLImageKHR image)
{
    GLuint texture;

    glGenTextures(1, &texture);
    glBindTexture(target, texture);
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glEGLImageTargetTexture2DOES(target, image);
    return texture;
}

static GLuint
glamor_egl_external_program(void)
{
    GLuint program = glCreateProgram();
    GLint vs = glamor_compile_glsl_prog(GL_VERTEX_SHADER, glamor_egl_external_vs);
    GLint fs = glamor_compile_glsl_prog(GL_FRAGMENT_SHADER, glamor_egl_external_fs);
    GLint linked;

    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glBindAttribLocation(program, 0, "position");
    glBindAttribLocation(program, 1, "source");
    glLinkProgram(program);

    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

static Bool
glamor_egl_external_stale(struct glamor_egl_external_pixmap *pixmap,
                          const BoxRec *box, BoxPtr stale)
{
    struct glamor_egl_external_context *context = pixmap->context;
    BoxRec whole = { 0, 0, pixmap->width, pixmap->height };
    BoxRec asked = box ? *box : whole;
    RegionRec wanted, clip;
    Bool any;

    if (pixmap->generation != context->generation) {
        pixmap->generation = context->generation;
        RegionEmpty(&pixmap->updated);
    }

    RegionInit(&wanted, &asked, 1);
    RegionInit(&clip, &whole, 1);
    RegionIntersect(&wanted, &wanted, &clip);
    RegionSubtract(&wanted, &wanted, &pixmap->updated);
    any = RegionNotEmpty(&wanted);
    if (any) {
        *stale = *RegionExtents(&wanted);
        RegionUnion(&pixmap->updated, &pixmap->updated, &wanted);
    }
    RegionUninit(&clip);
    RegionUninit(&wanted);
    return any;
}

static void
glamor_egl_external_quad(GLfloat *quad, const BoxRec *box, int width,
                         int height)
{
    quad[0] = quad[4] = (GLfloat) box->x1 / width;
    quad[2] = quad[6] = (GLfloat) box->x2 / width;
    quad[1] = quad[3] = (GLfloat) box->y1 / height;
    quad[5] = quad[7] = (GLfloat) box->y2 / height;
}

static void
glamor_egl_external_draw(struct glamor_egl_external_pixmap *pixmap, GLuint fb,
                         int width, int height, const BoxRec *dst_box,
                         const BoxRec *src_box)
{
    GLfloat positions[8], sources[8];

    glamor_egl_external_quad(positions, dst_box, width, height);
    glamor_egl_external_quad(sources, src_box, pixmap->width, pixmap->height);
    glBindFramebuffer(GL_FRAMEBUFFER, fb);
    glViewport(0, 0, width, height);
    glUseProgram(pixmap->context->program);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, pixmap->src_tex);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, positions);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, sources);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static EGLSyncKHR
glamor_egl_external_begin(struct glamor_egl_external_pixmap *pixmap)
{
    struct glamor_egl_external_context *context = pixmap->context;
    EGLSyncKHR sync;

    glamor_make_current(glamor_get_screen_private(context->screen));
    sync = glamor_egl_external_fence(context);
    glamor_egl_external_enter(context);
    return sync;
}

static void
glamor_egl_external_end(struct glamor_egl_external_pixmap *pixmap, Bool drawn)
{
    struct glamor_egl_external_context *context = pixmap->context;
    EGLSyncKHR sync = EGL_NO_SYNC_KHR;

    if (drawn) {
        glamor_egl_external_add_reader(pixmap);
        sync = glamor_egl_external_fence(context);
    }
    glamor_egl_external_leave(context);
    glamor_egl_external_wait(context, sync);
}

static void
glamor_egl_external_update(glamor_external *external, const BoxRec *box)
{
    struct glamor_egl_external_pixmap *pixmap =
        (struct glamor_egl_external_pixmap *) external;
    BoxRec stale;
    EGLSyncKHR sync;

    if (!glamor_egl_external_stale(pixmap, box, &stale))
        return;

    sync = glamor_egl_external_begin(pixmap);
    glamor_egl_external_wait(pixmap->context, sync);
    glamor_egl_external_wait_writers(pixmap);
    glamor_egl_external_draw(pixmap, pixmap->fb, pixmap->width,
                             pixmap->height, &stale, &stale);
    glamor_egl_external_end(pixmap, TRUE);
}

static void
glamor_egl_external_target_release(struct glamor_egl_external_target *target)
{
    if (!target->image)
        return;

    glDeleteFramebuffers(1, &target->fb);
    glDeleteTextures(1, &target->tex);
    target->image = NULL;
}

static struct glamor_egl_external_target *
glamor_egl_external_target(struct glamor_egl_external_context *context,
                           EGLImageKHR image)
{
    struct glamor_egl_external_target *target;
    int i;

    for (i = 0; i < ARRAY_SIZE(context->targets); i++) {
        if (context->targets[i].image == image)
            return &context->targets[i];
    }

    target = &context->targets[context->next_target];
    context->next_target = (context->next_target + 1) %
        ARRAY_SIZE(context->targets);
    glamor_egl_external_target_release(target);

    while (glGetError() != GL_NO_ERROR)
        ;
    target->tex = glamor_egl_external_texture(GL_TEXTURE_2D, image);
    glGenFramebuffers(1, &target->fb);
    glBindFramebuffer(GL_FRAMEBUFFER, target->fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                           target->tex, 0);
    target->image = image;
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
        glGetError() == GL_NO_ERROR)
        return target;

    glamor_egl_external_target_release(target);
    return NULL;
}

void
glamor_egl_external_forget_image(struct glamor_egl_external_context *context,
                                 EGLImageKHR image)
{
    int i;

    if (!context)
        return;

    for (i = 0; i < ARRAY_SIZE(context->targets); i++) {
        if (context->targets[i].image == image) {
            glamor_egl_external_enter(context);
            glamor_egl_external_target_release(&context->targets[i]);
            glamor_egl_external_leave(context);
        }
    }
}

static Bool
glamor_egl_external_copy(glamor_external *external, PixmapPtr dst, BoxPtr box,
                         int nbox, int dst_dx, int dst_dy, int src_dx,
                         int src_dy)
{
    struct glamor_egl_external_pixmap *pixmap =
        (struct glamor_egl_external_pixmap *) external;
    glamor_pixmap_private *dst_priv = glamor_get_pixmap_private(dst);
    struct glamor_egl_external_target *target;
    EGLSyncKHR sync;

    if (!dst_priv || !dst_priv->image)
        return FALSE;

    sync = glamor_egl_external_begin(pixmap);
    target = glamor_egl_external_target(pixmap->context, dst_priv->image);
    glamor_egl_external_wait(pixmap->context, sync);
    if (!target) {
        glamor_egl_external_end(pixmap, FALSE);
        return FALSE;
    }

    glamor_egl_external_wait_writers(pixmap);
    for (; nbox--; box++) {
        BoxRec dst_box = {
            .x1 = box->x1 + dst_dx,
            .y1 = box->y1 + dst_dy,
            .x2 = box->x2 + dst_dx,
            .y2 = box->y2 + dst_dy,
        };
        BoxRec src_box = {
            .x1 = box->x1 + src_dx,
            .y1 = box->y1 + src_dy,
            .x2 = box->x2 + src_dx,
            .y2 = box->y2 + src_dy,
        };

        glamor_egl_external_draw(pixmap, target->fb, dst->drawable.width,
                                 dst->drawable.height, &dst_box, &src_box);
    }
    glamor_egl_external_end(pixmap, TRUE);
    return TRUE;
}

static void
glamor_egl_external_destroy(glamor_external *external)
{
    struct glamor_egl_external_pixmap *pixmap =
        (struct glamor_egl_external_pixmap *) external;

    glamor_egl_external_enter(pixmap->context);
    glDeleteFramebuffers(1, &pixmap->fb);
    glDeleteTextures(1, &pixmap->src_tex);
    glDeleteTextures(1, &pixmap->dst_tex);
    glamor_egl_external_leave(pixmap->context);
    if (pixmap->dmabuf_fd >= 0)
        close(pixmap->dmabuf_fd);
    RegionUninit(&pixmap->updated);
    free(pixmap);
}

Bool
glamor_egl_external_attach(struct glamor_egl_external_context *context,
                           PixmapPtr pixmap, EGLImageKHR src_image,
                           EGLImageKHR dst_image, int dmabuf_fd)
{
    glamor_pixmap_private *pixmap_priv = glamor_get_pixmap_private(pixmap);
    struct glamor_egl_external_pixmap *external;
    Bool complete;

    external = calloc(1, sizeof(*external));
    if (!external) {
        if (dmabuf_fd >= 0)
            close(dmabuf_fd);
        return FALSE;
    }

    external->base.update = glamor_egl_external_update;
    external->base.copy = glamor_egl_external_copy;
    external->base.destroy = glamor_egl_external_destroy;
    external->context = context;
    external->dmabuf_fd = dmabuf_fd;
    external->width = pixmap->drawable.width;
    external->height = pixmap->drawable.height;
    RegionNull(&external->updated);

    glamor_egl_external_enter(context);
    while (glGetError() != GL_NO_ERROR)
        ;
    external->src_tex = glamor_egl_external_texture(GL_TEXTURE_EXTERNAL_OES,
                                                    src_image);
    external->dst_tex = glamor_egl_external_texture(GL_TEXTURE_2D, dst_image);
    glGenFramebuffers(1, &external->fb);
    glBindFramebuffer(GL_FRAMEBUFFER, external->fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, external->dst_tex, 0);
    complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
               glGetError() == GL_NO_ERROR;
    glamor_egl_external_leave(context);

    if (!complete) {
        glamor_egl_external_destroy(&external->base);
        return FALSE;
    }

    pixmap_priv->fbo->external = &external->base;
    return TRUE;
}

static void
glamor_egl_external_block_handler(void *data, void *timeout)
{
}

static void
glamor_egl_external_wakeup_handler(void *data, int result)
{
    struct glamor_egl_external_context *context = data;

    context->generation++;
}

struct glamor_egl_external_context *
glamor_egl_external_context_create(ScreenPtr screen, EGLDisplay egl_display)
{
    static const EGLint attribs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    struct glamor_egl_external_context *context;
    EGLenum api = eglQueryAPI();

    context = calloc(1, sizeof(*context));
    if (!context)
        return NULL;

    context->screen = screen;
    context->display = egl_display;
    context->generation = 1;
    context->has_wait_sync =
        epoxy_has_egl_extension(egl_display, "EGL_KHR_fence_sync") &&
        epoxy_has_egl_extension(egl_display, "EGL_KHR_wait_sync");
    context->has_native_fence = context->has_wait_sync &&
        epoxy_has_egl_extension(egl_display, "EGL_ANDROID_native_fence_sync");

    eglBindAPI(EGL_OPENGL_ES_API);
    context->context = eglCreateContext(egl_display, EGL_NO_CONFIG_KHR,
                                        EGL_NO_CONTEXT, attribs);
    eglBindAPI(api);
    if (context->context == EGL_NO_CONTEXT) {
        free(context);
        return NULL;
    }

    glamor_egl_external_enter(context);
    if (epoxy_has_gl_extension("GL_OES_EGL_image_external")) {
        context->program = glamor_egl_external_program();
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
    }
    glamor_egl_external_leave(context);

    if (!context->program ||
        !RegisterBlockAndWakeupHandlers(glamor_egl_external_block_handler,
                                        glamor_egl_external_wakeup_handler,
                                        context)) {
        eglDestroyContext(context->display, context->context);
        free(context);
        return NULL;
    }
    return context;
}

void
glamor_egl_external_context_destroy(struct glamor_egl_external_context *context)
{
    if (!context)
        return;

    RemoveBlockAndWakeupHandlers(glamor_egl_external_block_handler,
                                 glamor_egl_external_wakeup_handler, context);
    eglDestroyContext(context->display, context->context);
    free(context);
}
