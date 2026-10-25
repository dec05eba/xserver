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

#ifdef HAVE_DIX_CONFIG_H
#include "dix-config.h"
#endif

#include <string.h>

#include <xf86.h>
#include <xf86Crtc.h>
#include <property.h>
#include <propertyst.h>

#include "driver.h"
#include "drmmode_display.h"

static Atom hdr_output_metadata_atom;

static const struct hdr_output_metadata *
ms_window_get_hdr_output_metadata(WindowPtr window)
{
    PropertyPtr prop;

    if (dixLookupProperty(&prop, window, hdr_output_metadata_atom,
                          serverClient, DixReadAccess) != Success)
        return NULL;

    if (prop->format != 8 || prop->size != sizeof(struct hdr_output_metadata))
        return NULL;

    return prop->data;
}

static const struct hdr_output_metadata *
ms_fullscreen_hdr_output_metadata(WindowPtr parent, BoxPtr box)
{
    WindowPtr child;

    for (child = parent->firstChild; child; child = child->nextSib) {
        const struct hdr_output_metadata *metadata;

        if (!child->viewable || child->drawable.class != InputOutput ||
            RegionContainsRect(&child->borderSize, box) != rgnIN)
            continue;

        metadata = ms_window_get_hdr_output_metadata(child);
        if (!metadata)
            metadata = ms_fullscreen_hdr_output_metadata(child, box);

        if (metadata || !child->overrideRedirect)
            return metadata;
    }

    return NULL;
}

void
ms_hdr_update(ScreenPtr screen)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn(screen);
    modesettingPtr ms = modesettingPTR(scrn);
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(scrn);
    int i;

    if (!ms->hdr_dirty || !scrn->vtSema || !screen->root)
        return;

    ms->hdr_dirty = FALSE;

    for (i = 0; i < config->num_crtc; i++) {
        xf86CrtcPtr crtc = config->crtc[i];

        if (!crtc->enabled)
            continue;

        drmmode_crtc_set_hdr_output_metadata(crtc,
            ms_fullscreen_hdr_output_metadata(screen->root, &crtc->bounds));
    }
}

void
ms_hdr_disable(ScrnInfoPtr scrn)
{
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(scrn);
    int i;

    for (i = 0; i < config->num_crtc; i++)
        drmmode_crtc_set_hdr_output_metadata(config->crtc[i], NULL);
}

static void
ms_hdr_post_validate_tree(WindowPtr parent, WindowPtr child, VTKind kind)
{
    ScreenPtr screen = (parent ? parent : child)->drawable.pScreen;
    modesettingPtr ms = modesettingPTR(xf86ScreenToScrn(screen));

    ms->hdr_dirty = TRUE;

    screen->PostValidateTree = ms->PostValidateTree;
    if (screen->PostValidateTree)
        screen->PostValidateTree(parent, child, kind);
    ms->PostValidateTree = screen->PostValidateTree;
    screen->PostValidateTree = ms_hdr_post_validate_tree;
}

static void
ms_hdr_property_state(CallbackListPtr *pcbl, void *closure, void *data)
{
    ScreenPtr screen = closure;
    PropertyStateRec *rec = data;

    if (rec->prop->propertyName == hdr_output_metadata_atom &&
        rec->win->drawable.pScreen == screen)
        modesettingPTR(xf86ScreenToScrn(screen))->hdr_dirty = TRUE;
}

Bool
ms_hdr_screen_init(ScreenPtr screen)
{
    modesettingPtr ms = modesettingPTR(xf86ScreenToScrn(screen));

    hdr_output_metadata_atom = MakeAtom("hdr_output_metadata",
                                        strlen("hdr_output_metadata"), TRUE);

    if (!AddCallback(&PropertyStateCallback, ms_hdr_property_state, screen))
        return FALSE;

    ms->PostValidateTree = screen->PostValidateTree;
    screen->PostValidateTree = ms_hdr_post_validate_tree;
    return TRUE;
}

void
ms_hdr_close_screen(ScreenPtr screen)
{
    modesettingPtr ms = modesettingPTR(xf86ScreenToScrn(screen));

    DeleteCallback(&PropertyStateCallback, ms_hdr_property_state, screen);

    if (screen->PostValidateTree == ms_hdr_post_validate_tree)
        screen->PostValidateTree = ms->PostValidateTree;
}
