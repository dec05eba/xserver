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
#include <dix-config.h>
#endif

#include "compint.h"

typedef void (*CompServerDeferredProc) (CompServerPtr server,
                                        CompServerDirectWindowPtr dw,
                                        WindowPtr pWin);

static CompServerDirectWindowPtr
compServerFindDirectWindow(CompServerPtr server, WindowPtr pWin)
{
    CompServerDirectWindowPtr dw;

    xorg_list_for_each_entry(dw, &server->directWindows, entry) {
        if (dw->pWin == pWin)
            return dw;
    }
    return NULL;
}

static CompServerDirectWindowPtr
compServerAddDirectWindow(CompServerPtr server, WindowPtr pWin)
{
    CompServerDirectWindowPtr dw = compServerFindDirectWindow(server, pWin);

    if (dw)
        return dw;

    dw = calloc(1, sizeof(*dw));
    if (!dw)
        return NULL;

    dw->pWin = pWin;
    xorg_list_add(&dw->entry, &server->directWindows);
    return dw;
}

static void
compServerFreeDirectWindow(CompServerDirectWindowPtr dw)
{
    xorg_list_del(&dw->entry);
    free(dw);
}

static void
compServerReleaseDirectWindow(CompServerDirectWindowPtr dw)
{
    if (!dw->hdr && !dw->deferred.active)
        compServerFreeDirectWindow(dw);
}

void
CompositeServerSetHdrWindow(WindowPtr pWin, Bool hdr)
{
    CompServerPtr server = compServerGet(pWin->drawable.pScreen);
    CompServerDirectWindowPtr dw;

    if (!server || !pWin->parent)
        return;

    dw = hdr ? compServerAddDirectWindow(server, pWin) :
        compServerFindDirectWindow(server, pWin);
    if (!dw)
        return;

    dw->hdr = hdr;
    if (pWin->viewable)
        compServerAddDamage(server, &pWin->borderSize);
    compServerReleaseDirectWindow(dw);
}

static Bool
compServerCanDefer(CompServerPtr server, WindowPtr pWin)
{
    CompServerDirectWindowPtr dw;

    if (CompositeIsServerRoot(pWin->parent) || wBorderWidth(pWin) ||
        !CompositeIsServerRedirected(pWin))
        return FALSE;

    if (!compServerWindowHasAlpha(pWin))
        return TRUE;

    dw = compServerFindDirectWindow(server, pWin);
    return dw && dw->hdr;
}

static void
compServerFlushDirectWindow(CompServerDirectWindowPtr dw)
{
    WindowPtr pWin = dw->pWin;
    RegionRec region;

    if (!pWin->viewable || pWin->redirectDraw == RedirectDrawNone ||
        CompositeIsServerRoot(pWin->parent) ||
        (!dw->deferred.stale && dw->deferred.x == pWin->drawable.x &&
         dw->deferred.y == pWin->drawable.y))
        return;

    RegionNull(&region);
    RegionCopy(&region, &pWin->borderSize);
    RegionTranslate(&region, -pWin->drawable.x, -pWin->drawable.y);
    compWindowUpdateParent(pWin, &region);
    RegionUninit(&region);

    dw->deferred.stale = FALSE;
    dw->deferred.x = pWin->drawable.x;
    dw->deferred.y = pWin->drawable.y;
}

static void
compServerUndefer(CompServerDirectWindowPtr dw)
{
    compServerFlushDirectWindow(dw);
    dw->deferred.active = FALSE;
    compServerReleaseDirectWindow(dw);
}

Bool
compServerDeferUpdate(WindowPtr pWin)
{
    CompServerPtr server = compServerGet(pWin->drawable.pScreen);
    CompWindowPtr cw = GetCompWindow(pWin);
    CompServerDirectWindowPtr dw;
    RegionRec region;

    if (!server)
        return FALSE;

    if (!compServerCanDefer(server, pWin)) {
        dw = compServerFindDirectWindow(server, pWin);
        if (dw && dw->deferred.active)
            compServerUndefer(dw);
        return FALSE;
    }

    dw = compServerAddDirectWindow(server, pWin);
    if (!dw)
        return FALSE;

    dw->deferred.active = TRUE;
    dw->deferred.stale = TRUE;

    RegionNull(&region);
    RegionCopy(&region, DamageRegion(cw->damage));
    RegionTranslate(&region, pWin->drawable.x, pWin->drawable.y);
    RegionIntersect(&region, &region, &cw->borderClip);
    DamageDamageRegion(&pWin->parent->drawable, &region);
    RegionUninit(&region);
    DamageEmpty(cw->damage);
    return TRUE;
}

int
compServerTreeLevel(WindowPtr pWin)
{
    int level = 0;

    while ((pWin = pWin->parent))
        level++;
    return level;
}

static void
compServerForEachDeferred(CompServerPtr server, WindowPtr pWin,
                          CompServerDeferredProc proc)
{
    CompServerDirectWindowPtr dw, tmp;
    int level = 0;

    xorg_list_for_each_entry(dw, &server->directWindows, entry) {
        if (dw->deferred.active)
            level = max(level, compServerTreeLevel(dw->pWin));
    }

    for (; level > 0; level--) {
        xorg_list_for_each_entry_safe(dw, tmp, &server->directWindows, entry) {
            if (dw->deferred.active && compServerTreeLevel(dw->pWin) == level)
                (*proc) (server, dw, pWin);
        }
    }
}

static void
compServerFlushInferior(CompServerPtr server, CompServerDirectWindowPtr dw,
                        WindowPtr pAncestor)
{
    if (IsParent(pAncestor, dw->pWin))
        compServerFlushDirectWindow(dw);
}

static void
compServerUndeferInTree(CompServerPtr server, CompServerDirectWindowPtr dw,
                        WindowPtr pTree)
{
    if (dw->pWin == pTree || IsParent(pTree, dw->pWin))
        compServerUndefer(dw);
}

static void
compServerUndeferIneligible(CompServerPtr server,
                            CompServerDirectWindowPtr dw, WindowPtr pUnused)
{
    if (!compServerCanDefer(server, dw->pWin))
        compServerUndefer(dw);
}

void
compServerFlushInferiors(WindowPtr pWin)
{
    CompServerPtr server = compServerGet(pWin->drawable.pScreen);

    if (server && pWin->parent)
        compServerForEachDeferred(server, pWin, compServerFlushInferior);
}

void
compServerUndeferTree(WindowPtr pWin)
{
    CompServerPtr server = compServerGet(pWin->drawable.pScreen);

    if (server)
        compServerForEachDeferred(server, pWin, compServerUndeferInTree);
}

void
compServerUpdateDeferred(CompServerPtr server)
{
    compServerForEachDeferred(server, NULL, compServerUndeferIneligible);
}

void
compServerForgetDeferred(WindowPtr pWin)
{
    CompServerPtr server = compServerGet(pWin->drawable.pScreen);
    CompServerDirectWindowPtr dw;

    if (!server)
        return;

    dw = compServerFindDirectWindow(server, pWin);
    if (dw && dw->deferred.active) {
        dw->deferred.active = FALSE;
        compServerReleaseDirectWindow(dw);
    }
}

void
compServerDestroyDirectWindow(CompServerPtr server, WindowPtr pWin)
{
    CompServerDirectWindowPtr dw = compServerFindDirectWindow(server, pWin);

    if (dw)
        compServerFreeDirectWindow(dw);
}

void
compServerFreeDirectWindows(CompServerPtr server)
{
    CompServerDirectWindowPtr dw, tmp;

    xorg_list_for_each_entry_safe(dw, tmp, &server->directWindows, entry)
        compServerFreeDirectWindow(dw);
}
