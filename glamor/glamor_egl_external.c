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

#include "glamor_priv.h"
#include "glamor_egl_external.h"

struct glamor_egl_external_context {
    ScreenPtr screen;
    EGLDisplay display;
    EGLContext context;
    GLuint program;
    Bool has_wait_sync;
    unsigned long generation;
};

struct glamor_egl_external_pixmap {
    glamor_external base;
    struct glamor_egl_external_context *context;
    GLuint src_tex;
    GLuint dst_tex;
    GLuint fb;
    int width;
    int height;
    unsigned long generation;
};

static const char glamor_egl_external_vs[] =
    "attribute vec2 position;\n"
    "varying vec2 texcoord;\n"
    "void main() {\n"
    "    texcoord = position;\n"
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
    glLinkProgram(program);

    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

static void
glamor_egl_external_update(glamor_external *external)
{
    static const GLfloat positions[] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    struct glamor_egl_external_pixmap *pixmap =
        (struct glamor_egl_external_pixmap *) external;
    struct glamor_egl_external_context *context = pixmap->context;
    EGLSyncKHR sync;

    if (pixmap->generation == context->generation)
        return;
    pixmap->generation = context->generation;

    glamor_make_current(glamor_get_screen_private(context->screen));
    sync = glamor_egl_external_fence(context);
    glamor_egl_external_enter(context);
    glamor_egl_external_wait(context, sync);

    glBindFramebuffer(GL_FRAMEBUFFER, pixmap->fb);
    glViewport(0, 0, pixmap->width, pixmap->height);
    glUseProgram(context->program);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, pixmap->src_tex);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, positions);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    sync = glamor_egl_external_fence(context);
    glamor_egl_external_leave(context);
    glamor_egl_external_wait(context, sync);
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
    free(pixmap);
}

Bool
glamor_egl_external_attach(struct glamor_egl_external_context *context,
                           PixmapPtr pixmap, EGLImageKHR src_image,
                           EGLImageKHR dst_image)
{
    glamor_pixmap_private *pixmap_priv = glamor_get_pixmap_private(pixmap);
    struct glamor_egl_external_pixmap *external;
    Bool complete;

    external = calloc(1, sizeof(*external));
    if (!external)
        return FALSE;

    external->base.update = glamor_egl_external_update;
    external->base.destroy = glamor_egl_external_destroy;
    external->context = context;
    external->width = pixmap->drawable.width;
    external->height = pixmap->drawable.height;

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
