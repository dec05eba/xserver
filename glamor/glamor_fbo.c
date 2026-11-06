/*
 * Copyright © 2009 Intel Corporation
 * Copyright © 1998 Keith Packard
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Zhigang Gong <zhigang.gong@gmail.com>
 *
 */

#include <stdlib.h>

#include "glamor_priv.h"

#define GLAMOR_FBO_LARGE_ALIGN 64

void
glamor_free_fbo(glamor_screen_private *glamor_priv, glamor_pixmap_fbo *fbo)
{
    glamor_make_current(glamor_priv);

    if (fbo->fb)
        glDeleteFramebuffers(1, &fbo->fb);
    if (fbo->tex)
        glDeleteTextures(1, &fbo->tex);
    if (fbo->external)
        fbo->external->destroy(fbo->external);

    free(fbo);
}

void
glamor_destroy_fbo(glamor_screen_private *glamor_priv,
                   glamor_pixmap_fbo *fbo)
{
    if (!glamor_fbo_cache_put(glamor_priv, fbo))
        glamor_free_fbo(glamor_priv, fbo);
}

Bool
glamor_pixmap_is_external(PixmapPtr pixmap)
{
    glamor_pixmap_private *pixmap_priv = glamor_get_pixmap_private(pixmap);

    return pixmap_priv && pixmap_priv->fbo && pixmap_priv->fbo->external;
}

void
glamor_external_update(PixmapPtr pixmap, const BoxRec *box)
{
    glamor_external *external;

    if (!glamor_pixmap_is_external(pixmap))
        return;

    external = glamor_get_pixmap_private(pixmap)->fbo->external;
    external->update(external, box);
}

Bool
glamor_external_copy(PixmapPtr src, PixmapPtr dst, BoxPtr box, int nbox,
                     int dst_dx, int dst_dy, int src_dx, int src_dy)
{
    glamor_external *external;

    if (!glamor_pixmap_is_external(src) || glamor_pixmap_is_external(dst) ||
        src == dst)
        return FALSE;

    external = glamor_get_pixmap_private(src)->fbo->external;
    return external->copy(external, dst, box, nbox, dst_dx, dst_dy, src_dx,
                          src_dy);
}

static int
glamor_pixmap_ensure_fb(glamor_screen_private *glamor_priv,
                        glamor_pixmap_fbo *fbo)
{
    int status, err = 0;

    glamor_make_current(glamor_priv);

    if (fbo->fb == 0)
        glGenFramebuffers(1, &fbo->fb);
    assert(fbo->tex != 0);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo->fb);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, fbo->tex, 0);
    status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        const char *str;

        switch (status) {
        case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
            str = "incomplete attachment";
            break;
        case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
            str = "incomplete/missing attachment";
            break;
        case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER:
            str = "incomplete draw buffer";
            break;
        case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER:
            str = "incomplete read buffer";
            break;
        case GL_FRAMEBUFFER_UNSUPPORTED:
            str = "unsupported";
            break;
        case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE:
            str = "incomplete multiple";
            break;
        default:
            str = "unknown error";
            break;
        }

        glamor_fallback("glamor: Failed to create fbo, %s\n", str);
        err = -1;
    }

    return err;
}

glamor_pixmap_fbo *
glamor_create_fbo_from_tex(glamor_screen_private *glamor_priv,
                           PixmapPtr pixmap, int w, int h, GLint tex, int flag)
{
    const struct glamor_format *f = glamor_format_for_pixmap(pixmap);
    glamor_pixmap_fbo *fbo;

    fbo = calloc(1, sizeof(*fbo));
    if (fbo == NULL)
        return NULL;

    fbo->tex = tex;
    fbo->width = w;
    fbo->height = h;
    fbo->is_red = f->format == GL_RED;

    if (flag != GLAMOR_CREATE_FBO_NO_FBO) {
        if (glamor_pixmap_ensure_fb(glamor_priv, fbo) != 0) {
            glamor_destroy_fbo(glamor_priv, fbo);
            fbo = NULL;
        }
    }

    return fbo;
}

static void
glamor_reset_texture_parameters(glamor_screen_private *glamor_priv, const struct glamor_format *f)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    if (!glamor_priv->has_texture_swizzle)
        return;

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_BLUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, f->format == GL_RED ? GL_RED : GL_ALPHA);
}

static glamor_pixmap_fbo *
glamor_reuse_cached_fbo(glamor_screen_private *glamor_priv, const struct glamor_format *f, int w, int h, int flag)
{
    glamor_pixmap_fbo *fbo = glamor_fbo_cache_take(glamor_priv, f, w, h);

    if (!fbo)
        return NULL;

    if (flag != GLAMOR_CREATE_FBO_NO_FBO && !fbo->fb && glamor_pixmap_ensure_fb(glamor_priv, fbo) != 0) {
        glamor_free_fbo(glamor_priv, fbo);
        return NULL;
    }

    glamor_make_current(glamor_priv);
    glBindTexture(GL_TEXTURE_2D, fbo->tex);
    glamor_reset_texture_parameters(glamor_priv, f);
    return fbo;
}

static int
_glamor_create_tex(glamor_screen_private *glamor_priv,
                   PixmapPtr pixmap, int w, int h)
{
    const struct glamor_format *f = glamor_format_for_pixmap(pixmap);
    unsigned int tex;

    glamor_make_current(glamor_priv);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glamor_reset_texture_parameters(glamor_priv, f);
    glamor_priv->suppress_gl_out_of_memory_logging = true;
    glTexImage2D(GL_TEXTURE_2D, 0, f->internalformat, w, h, 0,
                 f->format, f->type, NULL);
    glamor_priv->suppress_gl_out_of_memory_logging = false;

    if (glGetError() == GL_OUT_OF_MEMORY) {
        if (!glamor_priv->logged_any_fbo_allocation_failure) {
            LogMessageVerb(X_WARNING, 0, "glamor: Failed to allocate %dx%d "
                           "FBO due to GL_OUT_OF_MEMORY.\n", w, h);
            LogMessageVerb(X_WARNING, 0,
                           "glamor: Expect reduced performance.\n");
            glamor_priv->logged_any_fbo_allocation_failure = true;
        }
        glDeleteTextures(1, &tex);
        return 0;
    }

    return tex;
}

glamor_pixmap_fbo *
glamor_create_fbo(glamor_screen_private *glamor_priv,
                  PixmapPtr pixmap, int w, int h, int flag)
{
    const struct glamor_format *f = glamor_format_for_pixmap(pixmap);
    glamor_pixmap_fbo *fbo = glamor_reuse_cached_fbo(glamor_priv, f, w, h, flag);
    GLint tex;

    if (fbo)
        return fbo;

    tex = _glamor_create_tex(glamor_priv, pixmap, w, h);
    if (!tex) /* Texture creation failed due to GL_OUT_OF_MEMORY */
        return NULL;

    fbo = glamor_create_fbo_from_tex(glamor_priv, pixmap, w, h, tex, flag);
    if (fbo)
        fbo->owned_format = f;
    return fbo;
}

static void
glamor_clear_fbo_rect(glamor_screen_private *glamor_priv, glamor_pixmap_fbo *fbo,
                      const struct glamor_format *format, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0)
        return;

    glamor_make_current(glamor_priv);
    if (glamor_priv->has_clear_texture) {
        glClearTexSubImage(fbo->tex, 0, x, y, 0, w, h, 1, format->format, format->type, NULL);
        return;
    }

    glamor_set_destination_pixmap_fbo(glamor_priv, fbo, 0, 0, fbo->width, fbo->height);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w, h);
    glClearColor(0.0, 0.0, 0.0, 0.0);
    glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_SCISSOR_TEST);
}

static void
glamor_clear_fbo_margin(glamor_screen_private *glamor_priv, glamor_pixmap_fbo *fbo, int w, int h)
{
    glamor_clear_fbo_rect(glamor_priv, fbo, fbo->owned_format, w, 0, fbo->width - w, fbo->height);
    glamor_clear_fbo_rect(glamor_priv, fbo, fbo->owned_format, 0, h, w, fbo->height - h);
}

static Bool
glamor_usage_allows_margin(int usage)
{
    return usage == 0 || usage == CREATE_PIXMAP_USAGE_SCRATCH;
}

static int
glamor_fbo_size_class(int size)
{
    int granularity = GLAMOR_FBO_LARGE_ALIGN;

    while (granularity * 16 < size)
        granularity *= 2;
    return ALIGN(size, granularity);
}

glamor_pixmap_fbo *
glamor_create_pixmap_fbo(glamor_screen_private *glamor_priv, PixmapPtr pixmap, int w, int h, int usage)
{
    int storage_w = w, storage_h = h;
    glamor_pixmap_fbo *fbo;

    if (glamor_usage_allows_margin(usage) && glamor_fbo_size_is_large(w, h) &&
        w >= GLAMOR_FBO_LARGE_ALIGN && h >= GLAMOR_FBO_LARGE_ALIGN) {
        storage_w = glamor_fbo_size_class(w);
        storage_h = glamor_fbo_size_class(h);
        if (!glamor_check_fbo_size(glamor_priv, storage_w, storage_h)) {
            storage_w = w;
            storage_h = h;
        }
    }

    fbo = glamor_create_fbo(glamor_priv, pixmap, storage_w, storage_h, usage);
    if (fbo && (storage_w != w || storage_h != h))
        glamor_clear_fbo_margin(glamor_priv, fbo, w, h);
    return fbo;
}

/**
 * Create storage for the w * h region, using FBOs of the GL's maximum
 * supported size.
 */
glamor_pixmap_fbo *
glamor_create_fbo_array(glamor_screen_private *glamor_priv,
                        PixmapPtr pixmap, int flag,
                         int block_w, int block_h,
                         glamor_pixmap_private *priv)
{
    int w = pixmap->drawable.width;
    int h = pixmap->drawable.height;
    int block_wcnt;
    int block_hcnt;
    glamor_pixmap_fbo **fbo_array;
    BoxPtr box_array;
    int i, j;

    priv->block_w = block_w;
    priv->block_h = block_h;

    block_wcnt = (w + block_w - 1) / block_w;
    block_hcnt = (h + block_h - 1) / block_h;

    box_array = calloc(block_wcnt * block_hcnt, sizeof(box_array[0]));
    if (box_array == NULL)
        return NULL;

    fbo_array = calloc(block_wcnt * block_hcnt, sizeof(glamor_pixmap_fbo *));
    if (fbo_array == NULL) {
        free(box_array);
        return FALSE;
    }
    for (i = 0; i < block_hcnt; i++) {
        int block_y1, block_y2;
        int fbo_w, fbo_h;

        block_y1 = i * block_h;
        block_y2 = (block_y1 + block_h) > h ? h : (block_y1 + block_h);
        fbo_h = block_y2 - block_y1;

        for (j = 0; j < block_wcnt; j++) {
            box_array[i * block_wcnt + j].x1 = j * block_w;
            box_array[i * block_wcnt + j].y1 = block_y1;
            box_array[i * block_wcnt + j].x2 =
                (j + 1) * block_w > w ? w : (j + 1) * block_w;
            box_array[i * block_wcnt + j].y2 = block_y2;
            fbo_w =
                box_array[i * block_wcnt + j].x2 - box_array[i * block_wcnt +
                                                             j].x1;
            fbo_array[i * block_wcnt + j] = glamor_create_fbo(glamor_priv,
                                                              pixmap,
                                                              fbo_w, fbo_h,
                                                              GLAMOR_CREATE_PIXMAP_FIXUP);
            if (fbo_array[i * block_wcnt + j] == NULL)
                goto cleanup;
        }
    }

    priv->box = box_array[0];
    priv->box_array = box_array;
    priv->fbo_array = fbo_array;
    priv->block_wcnt = block_wcnt;
    priv->block_hcnt = block_hcnt;
    return fbo_array[0];

 cleanup:
    for (i = 0; i < block_wcnt * block_hcnt; i++)
        if (fbo_array[i])
            glamor_destroy_fbo(glamor_priv, fbo_array[i]);
    free(box_array);
    free(fbo_array);
    return NULL;
}

void
glamor_pixmap_clear_fbo(glamor_screen_private *glamor_priv, glamor_pixmap_fbo *fbo,
                        const struct glamor_format *pixmap_format)
{
    assert(fbo->fb != 0 && fbo->tex != 0);

    glamor_clear_fbo_rect(glamor_priv, fbo, pixmap_format, 0, 0, fbo->width, fbo->height);
}

glamor_pixmap_fbo *
glamor_pixmap_detach_fbo(glamor_pixmap_private *pixmap_priv)
{
    glamor_pixmap_fbo *fbo;

    if (pixmap_priv == NULL)
        return NULL;

    fbo = pixmap_priv->fbo;
    if (fbo == NULL)
        return NULL;

    pixmap_priv->fbo = NULL;
    return fbo;
}

/* The pixmap must not be attached to another fbo. */
void
glamor_pixmap_attach_fbo(PixmapPtr pixmap, glamor_pixmap_fbo *fbo)
{
    glamor_pixmap_private *pixmap_priv;

    pixmap_priv = glamor_get_pixmap_private(pixmap);

    if (pixmap_priv->fbo)
        return;

    pixmap_priv->fbo = fbo;
    fbo->shared = pixmap_priv->type == GLAMOR_TEXTURE_DRM;

    switch (pixmap_priv->type) {
    case GLAMOR_TEXTURE_ONLY:
    case GLAMOR_TEXTURE_DRM:
        pixmap_priv->gl_fbo = GLAMOR_FBO_NORMAL;
        pixmap->devPrivate.ptr = NULL;
    default:
        break;
    }
}

void
glamor_pixmap_destroy_fbo(PixmapPtr pixmap)
{
    ScreenPtr screen = pixmap->drawable.pScreen;
    glamor_screen_private *glamor_priv = glamor_get_screen_private(screen);
    glamor_pixmap_private *priv = glamor_get_pixmap_private(pixmap);
    glamor_pixmap_fbo *fbo;

    if (glamor_pixmap_priv_is_large(priv)) {
        int i;

        for (i = 0; i < priv->block_wcnt * priv->block_hcnt; i++)
            glamor_destroy_fbo(glamor_priv, priv->fbo_array[i]);
        free(priv->fbo_array);
        priv->fbo_array = NULL;
    }
    else {
        fbo = glamor_pixmap_detach_fbo(priv);
        if (fbo)
            glamor_destroy_fbo(glamor_priv, fbo);
    }
}

Bool
glamor_pixmap_ensure_fbo(PixmapPtr pixmap, int flag)
{
    glamor_screen_private *glamor_priv;
    glamor_pixmap_private *pixmap_priv;
    glamor_pixmap_fbo *fbo;

    glamor_priv = glamor_get_screen_private(pixmap->drawable.pScreen);
    pixmap_priv = glamor_get_pixmap_private(pixmap);
    if (pixmap_priv->fbo == NULL) {

        fbo = glamor_create_fbo(glamor_priv, pixmap, pixmap->drawable.width,
                                pixmap->drawable.height, flag);
        if (fbo == NULL)
            return FALSE;

        glamor_pixmap_attach_fbo(pixmap, fbo);
    }
    else {
        /* We do have a fbo, but it may lack of fb or tex. */
        if (!pixmap_priv->fbo->tex)
            pixmap_priv->fbo->tex =
                _glamor_create_tex(glamor_priv, pixmap, pixmap->drawable.width,
                                   pixmap->drawable.height);

        if (flag != GLAMOR_CREATE_FBO_NO_FBO && pixmap_priv->fbo->fb == 0)
            if (glamor_pixmap_ensure_fb(glamor_priv, pixmap_priv->fbo) != 0)
                return FALSE;
    }

    return TRUE;
}

void
glamor_pixmap_exchange_fbos(PixmapPtr front, PixmapPtr back)
{
    glamor_pixmap_private *front_priv, *back_priv;
    glamor_pixmap_fbo *temp_fbo;

    front_priv = glamor_get_pixmap_private(front);
    back_priv = glamor_get_pixmap_private(back);
    temp_fbo = front_priv->fbo;
    front_priv->fbo = back_priv->fbo;
    back_priv->fbo = temp_fbo;
}

Bool
glamor_pixmap_trim_fbo(PixmapPtr pixmap)
{
    glamor_pixmap_private *priv = glamor_get_pixmap_private(pixmap);
    BoxRec box = { 0, 0, pixmap->drawable.width, pixmap->drawable.height };
    PixmapPtr exact;

    if (!glamor_pixmap_priv_is_oversized(pixmap, priv))
        return TRUE;

    if (priv->prepared)
        return FALSE;

    exact = glamor_create_pixmap(pixmap->drawable.pScreen, box.x2, box.y2, pixmap->drawable.depth,
                                 GLAMOR_CREATE_NO_LARGE);
    if (!exact)
        return FALSE;

    if (!glamor_pixmap_has_fbo(exact)) {
        glamor_destroy_pixmap(exact);
        return FALSE;
    }

    glamor_copy(&pixmap->drawable, &exact->drawable, NULL, &box, 1, 0, 0, FALSE, FALSE, 0, NULL);
    glamor_pixmap_exchange_fbos(pixmap, exact);
    glamor_destroy_pixmap(exact);
    return TRUE;
}
