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

#include "dri3_priv.h"
#include "gcstruct.h"

typedef struct dri3_offload_pixmap {
    PixmapPtr native;
    Bool stale;
} dri3_offload_pixmap_rec, *dri3_offload_pixmap_ptr;

typedef struct dri3_offload_client {
    Bool checked;
    Bool offloaded;
} dri3_offload_client_rec, *dri3_offload_client_ptr;

static DevPrivateKeyRec dri3_offload_pixmap_key;
static DevPrivateKeyRec dri3_offload_client_key;

static dri3_offload_pixmap_ptr
dri3_offload_pixmap(PixmapPtr pixmap)
{
    if (!dixPrivateKeyRegistered(&dri3_offload_pixmap_key))
        return NULL;

    return dixLookupPrivate(&pixmap->devPrivates, &dri3_offload_pixmap_key);
}

static Bool
dri3_offload_copy(PixmapPtr src, PixmapPtr dst)
{
    int width = src->drawable.width;
    int height = src->drawable.height;
    int stride = PixmapBytePad(width, src->drawable.depth);
    char *data = malloc((size_t) stride * height);
    GCPtr gc = GetScratchGC(dst->drawable.depth, dst->drawable.pScreen);

    if (data && gc) {
        src->drawable.pScreen->GetImage(&src->drawable, 0, 0, width, height,
                                        ZPixmap, ~0, data);
        ValidateGC(&dst->drawable, gc);
        gc->ops->PutImage(&dst->drawable, gc, dst->drawable.depth, 0, 0,
                          width, height, 0, ZPixmap, data);
    }

    if (gc)
        FreeScratchGC(gc);
    free(data);
    return data && gc;
}

static void
dri3_offload_update(PixmapPtr pixmap)
{
    dri3_offload_pixmap_ptr offload = dri3_offload_pixmap(pixmap);

    if (offload && offload->stale && dri3_offload_copy(offload->native, pixmap))
        offload->stale = FALSE;
}

static void
dri3_offload_source_validate(DrawablePtr drawable, int x, int y,
                             int width, int height,
                             unsigned int subWindowMode)
{
    ScreenPtr screen = drawable->pScreen;
    dri3_screen_priv_ptr ds = dri3_screen_priv(screen);

    if (drawable->type == DRAWABLE_PIXMAP)
        dri3_offload_update((PixmapPtr) drawable);
    else if (drawable->type == DRAWABLE_WINDOW)
        dri3_offload_update(screen->GetWindowPixmap((WindowPtr) drawable));

    unwrap(ds, screen, SourceValidate);
    screen->SourceValidate(drawable, x, y, width, height, subWindowMode);
    wrap(ds, screen, SourceValidate, dri3_offload_source_validate);
}

static Bool
dri3_offload_destroy_pixmap(PixmapPtr pixmap)
{
    ScreenPtr screen = pixmap->drawable.pScreen;
    dri3_screen_priv_ptr ds = dri3_screen_priv(screen);
    dri3_offload_pixmap_ptr offload = dri3_offload_pixmap(pixmap);
    Bool ret;

    if (offload && pixmap->refcnt == 1) {
        dixSetPrivate(&pixmap->devPrivates, &dri3_offload_pixmap_key, NULL);
        dixDestroyPixmap(offload->native, 0);
        free(offload);
    }

    unwrap(ds, screen, DestroyPixmap);
    ret = screen->DestroyPixmap(pixmap);
    wrap(ds, screen, DestroyPixmap, dri3_offload_destroy_pixmap);
    return ret;
}

Bool
dri3_offload_screen_init(ScreenPtr screen)
{
    dri3_screen_priv_ptr ds = dri3_screen_priv(screen);

    if (!dixRegisterPrivateKey(&dri3_offload_pixmap_key, PRIVATE_PIXMAP, 0) ||
        !dixRegisterPrivateKey(&dri3_offload_client_key, PRIVATE_CLIENT,
                               sizeof(dri3_offload_client_rec)))
        return FALSE;

    wrap(ds, screen, SourceValidate, dri3_offload_source_validate);
    wrap(ds, screen, DestroyPixmap, dri3_offload_destroy_pixmap);
    return TRUE;
}

void
dri3_offload_close_screen(ScreenPtr screen)
{
    dri3_screen_priv_ptr ds = dri3_screen_priv(screen);

    unwrap(ds, screen, SourceValidate);
    unwrap(ds, screen, DestroyPixmap);
}

Bool
dri3_offload_init(ScreenPtr screen, const dri3_offload_info_rec *info)
{
    dri3_screen_priv_ptr ds;

    if (!dixPrivateKeyRegistered(&dri3_screen_private_key) ||
        !(ds = dri3_screen_priv(screen)))
        return FALSE;

    ds->offload = info;
    return TRUE;
}

static ScreenPtr
dri3_offload_render_screen(ScreenPtr screen)
{
    int i;

    for (i = 0; i < screenInfo.numGPUScreens; i++) {
        ScreenPtr gpu = screenInfo.gpuscreens[i];
        dri3_screen_priv_ptr ds = dri3_screen_priv(gpu);

        if (gpu->current_primary == screen && ds && ds->offload)
            return gpu;
    }
    return NULL;
}

ScreenPtr
dri3_offload_screen(ClientPtr client, ScreenPtr screen)
{
    dri3_offload_client_ptr offload_client =
        dixLookupPrivate(&client->devPrivates, &dri3_offload_client_key);
    ScreenPtr render_screen = dri3_offload_render_screen(screen);

    if (!render_screen)
        return NULL;

    if (!offload_client->checked) {
        offload_client->offloaded =
            dri3_screen_priv(render_screen)->offload->renders_client(render_screen,
                                                                     client);
        offload_client->checked = TRUE;
    }
    return offload_client->offloaded ? render_screen : NULL;
}

Bool
dri3_offload_can_scanout_window(ScreenPtr render_screen, WindowPtr window)
{
    const dri3_offload_info_rec *info = dri3_screen_priv(render_screen)->offload;

    return info->can_scanout_window(render_screen, window);
}

int
dri3_offload_pixmap_from_fds(PixmapPtr *ppixmap, ScreenPtr screen,
                             ScreenPtr render_screen,
                             CARD8 num_fds, const int *fds,
                             CARD16 width, CARD16 height,
                             const CARD32 *strides, const CARD32 *offsets,
                             CARD8 depth, CARD8 bpp, CARD64 modifier)
{
    dri3_offload_pixmap_ptr offload;
    PixmapPtr native, pixmap;
    int rc;

    rc = dri3_pixmap_from_fds(&native, render_screen, num_fds, fds,
                              width, height, strides, offsets, depth, bpp,
                              modifier);
    if (rc != Success)
        return rc;

    offload = calloc(1, sizeof(*offload));
    pixmap = screen->CreatePixmap(screen, width, height, depth, 0);
    if (!offload || !pixmap) {
        if (pixmap)
            dixDestroyPixmap(pixmap, 0);
        dixDestroyPixmap(native, 0);
        free(offload);
        return BadAlloc;
    }

    offload->native = native;
    offload->stale = TRUE;
    dixSetPrivate(&pixmap->devPrivates, &dri3_offload_pixmap_key, offload);
    *ppixmap = pixmap;
    return Success;
}

PixmapPtr
dri3_offload_native_pixmap(PixmapPtr pixmap)
{
    dri3_offload_pixmap_ptr offload = dri3_offload_pixmap(pixmap);

    return offload ? offload->native : NULL;
}

Bool
dri3_offload_pixmap_suboptimal(WindowPtr window, PixmapPtr pixmap)
{
    ClientPtr client;
    ScreenPtr render_screen;

    if (!dixPrivateKeyRegistered(&dri3_offload_client_key) ||
        !pixmap->drawable.id ||
        !(client = clients[CLIENT_ID(pixmap->drawable.id)]))
        return FALSE;

    render_screen = dri3_offload_screen(client, window->drawable.pScreen);
    return render_screen &&
        (dri3_offload_pixmap(pixmap) != NULL) !=
        dri3_offload_can_scanout_window(render_screen, window);
}

void
dri3_offload_pixmap_presented(PixmapPtr pixmap)
{
    dri3_offload_pixmap_ptr offload = dri3_offload_pixmap(pixmap);

    if (offload)
        offload->stale = TRUE;
}
