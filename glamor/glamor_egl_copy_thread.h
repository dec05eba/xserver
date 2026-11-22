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

#ifndef GLAMOR_EGL_COPY_THREAD_H
#define GLAMOR_EGL_COPY_THREAD_H

struct glamor_egl_copy_thread;
struct glamor_egl_copy_job;

struct glamor_egl_copy_thread *
glamor_egl_copy_thread_create(EGLDisplay egl_display);

void
glamor_egl_copy_thread_destroy(struct glamor_egl_copy_thread *thread);

struct glamor_egl_copy_job *
glamor_egl_copy_job_create(struct glamor_egl_copy_thread *thread,
                           PixmapPtr dst, const glamor_egl_copy_op *ops,
                           int nops, Bool fence,
                           glamor_egl_copy_done_proc done, void *data);

void
glamor_egl_copy_thread_queue(struct glamor_egl_copy_thread *thread,
                             struct glamor_egl_copy_job *job,
                             int wait_fence_fd);

void
glamor_egl_copy_thread_cancel(struct glamor_egl_copy_thread *thread,
                              void *data);

void
glamor_egl_copy_thread_forget_image(struct glamor_egl_copy_thread *thread,
                                    uint64_t image_id);

#endif /* GLAMOR_EGL_COPY_THREAD_H */
