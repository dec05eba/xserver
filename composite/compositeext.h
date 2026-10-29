/*
 * Copyright © 2009 NVIDIA Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#ifndef _COMPOSITEEXT_H_
#define _COMPOSITEEXT_H_

#include "misc.h"
#include "scrnintstr.h"
#include "picture.h"

extern _X_EXPORT Bool CompositeRegisterAlternateVisuals(ScreenPtr pScreen,
                                                        VisualID * vids,
                                                        int nVisuals);

extern _X_EXPORT Bool CompositeRegisterImplicitRedirectionException(ScreenPtr pScreen,
                                                                    VisualID parentVisual,
                                                                    VisualID winVisual);


extern _X_EXPORT Bool compIsAlternateVisual(ScreenPtr pScreen, XID visual);
Bool CompositeIsImplicitRedirectException(ScreenPtr pScreen,
                                          XID parentVisual, XID winVisual);
extern _X_EXPORT RESTYPE CompositeClientWindowType;

typedef Bool (*CompositeServerPaintHdrProcPtr) (PicturePtr pDst, int x, int y,
                                                WindowPtr pWin,
                                                RegionPtr pRegion,
                                                RegionPtr pExposed,
                                                CARD16 opacity,
                                                void *closure);

extern _X_EXPORT Bool CompositeServerInit(ScreenPtr pScreen,
                                          CompositeServerPaintHdrProcPtr PaintHdr);

extern _X_EXPORT Bool CompositeServerActive(ScreenPtr pScreen);

extern _X_EXPORT Bool CompositeIsServerRoot(WindowPtr pWin);

extern _X_EXPORT Bool CompositeIsServerRedirected(WindowPtr pWin);

extern _X_EXPORT void CompositeServerFlush(ScreenPtr pScreen,
                                           RegionPtr pDamage);

extern _X_EXPORT void CompositeServerPaint(ScreenPtr pScreen, PicturePtr pDst,
                                           int x, int y, RegionPtr pRegion,
                                           void *closure);

extern _X_EXPORT WindowPtr CompositeServerScanoutWindow(ScreenPtr pScreen,
                                                        BoxPtr pBox);

extern _X_EXPORT void CompositeServerSetHdrWindow(WindowPtr pWin, Bool hdr);

extern _X_EXPORT WindowPtr CompositeServerHdrWindow(ScreenPtr pScreen,
                                                    BoxPtr pBox);

extern _X_EXPORT void CompositeServerPresented(WindowPtr pWin,
                                               PixmapPtr pPixmap);

#endif                          /* _COMPOSITEEXT_H_ */
