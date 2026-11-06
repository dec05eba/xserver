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
 * FROM, OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "glamor_priv.h"

#define GLAMOR_FBO_CACHE_EXPIRE_MS 1000

static size_t
glamor_fbo_bytes(const glamor_pixmap_fbo *fbo)
{
    return (size_t) fbo->width * fbo->height * (PICT_FORMAT_BPP(fbo->owned_format->render_format) / 8);
}

static glamor_fbo_pool *
glamor_fbo_pool_for(glamor_screen_private *glamor_priv, int w, int h)
{
    return glamor_fbo_size_is_large(w, h) ? &glamor_priv->fbo_cache.large : &glamor_priv->fbo_cache.small;
}

static void
glamor_fbo_pool_init(glamor_fbo_pool *pool, int max_count, size_t max_bytes)
{
    xorg_list_init(&pool->fbos);
    pool->count = 0;
    pool->bytes = 0;
    pool->max_count = max_count;
    pool->max_bytes = max_bytes;
}

static void
glamor_fbo_pool_remove(glamor_fbo_pool *pool, glamor_pixmap_fbo *fbo)
{
    xorg_list_del(&fbo->cache_entry);
    pool->count--;
    pool->bytes -= glamor_fbo_bytes(fbo);
}

static void
glamor_fbo_pool_evict_oldest(glamor_screen_private *glamor_priv, glamor_fbo_pool *pool)
{
    glamor_pixmap_fbo *oldest = xorg_list_last_entry(&pool->fbos, glamor_pixmap_fbo, cache_entry);

    glamor_fbo_pool_remove(pool, oldest);
    glamor_free_fbo(glamor_priv, oldest);
}

static void
glamor_fbo_pool_expire(glamor_screen_private *glamor_priv, glamor_fbo_pool *pool, CARD32 now)
{
    while (pool->count) {
        glamor_pixmap_fbo *oldest = xorg_list_last_entry(&pool->fbos, glamor_pixmap_fbo, cache_entry);

        if ((CARD32) (now - oldest->cached_time) < GLAMOR_FBO_CACHE_EXPIRE_MS)
            break;
        glamor_fbo_pool_evict_oldest(glamor_priv, pool);
    }
}

static void
glamor_fbo_pool_fini(glamor_screen_private *glamor_priv, glamor_fbo_pool *pool)
{
    while (pool->count)
        glamor_fbo_pool_evict_oldest(glamor_priv, pool);
}

void
glamor_fbo_cache_init(glamor_screen_private *glamor_priv)
{
    glamor_fbo_pool_init(&glamor_priv->fbo_cache.small, 256, 32 * 1024 * 1024);
    glamor_fbo_pool_init(&glamor_priv->fbo_cache.large, 16, 128 * 1024 * 1024);
}

void
glamor_fbo_cache_fini(glamor_screen_private *glamor_priv)
{
    glamor_fbo_pool_fini(glamor_priv, &glamor_priv->fbo_cache.small);
    glamor_fbo_pool_fini(glamor_priv, &glamor_priv->fbo_cache.large);
}

void
glamor_fbo_cache_expire(glamor_screen_private *glamor_priv)
{
    CARD32 now = GetTimeInMillis();

    glamor_fbo_pool_expire(glamor_priv, &glamor_priv->fbo_cache.small, now);
    glamor_fbo_pool_expire(glamor_priv, &glamor_priv->fbo_cache.large, now);
}

Bool
glamor_fbo_cache_put(glamor_screen_private *glamor_priv, glamor_pixmap_fbo *fbo)
{
    glamor_fbo_pool *pool;

    if (!fbo->owned_format || fbo->shared || fbo->external)
        return FALSE;

    pool = glamor_fbo_pool_for(glamor_priv, fbo->width, fbo->height);
    if (glamor_fbo_bytes(fbo) > pool->max_bytes)
        return FALSE;

    while (pool->count >= pool->max_count || pool->bytes + glamor_fbo_bytes(fbo) > pool->max_bytes)
        glamor_fbo_pool_evict_oldest(glamor_priv, pool);

    fbo->cached_time = GetTimeInMillis();
    xorg_list_add(&fbo->cache_entry, &pool->fbos);
    pool->count++;
    pool->bytes += glamor_fbo_bytes(fbo);
    return TRUE;
}

glamor_pixmap_fbo *
glamor_fbo_cache_take(glamor_screen_private *glamor_priv, const struct glamor_format *f, int w, int h)
{
    glamor_fbo_pool *pool = glamor_fbo_pool_for(glamor_priv, w, h);
    glamor_pixmap_fbo *fbo;

    xorg_list_for_each_entry(fbo, &pool->fbos, cache_entry) {
        if (fbo->owned_format == f && fbo->width == w && fbo->height == h) {
            glamor_fbo_pool_remove(pool, fbo);
            return fbo;
        }
    }
    return NULL;
}
