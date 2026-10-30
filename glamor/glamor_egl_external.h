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

#ifndef GLAMOR_EGL_EXTERNAL_H
#define GLAMOR_EGL_EXTERNAL_H

struct glamor_egl_external_context;

struct glamor_egl_external_context *
glamor_egl_external_context_create(ScreenPtr screen, EGLDisplay egl_display);

void
glamor_egl_external_context_destroy(struct glamor_egl_external_context *context);

Bool
glamor_egl_external_attach(struct glamor_egl_external_context *context,
                           PixmapPtr pixmap, EGLImageKHR src_image,
                           EGLImageKHR dst_image, int dmabuf_fd);

#endif /* GLAMOR_EGL_EXTERNAL_H */
