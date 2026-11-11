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

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <xf86.h>
#include <xf86Crtc.h>

#include "driver.h"

static uint32_t
drmmode_colorop_prop(int fd, uint32_t object_id, uint32_t object_type,
                     const char *name, uint64_t *value)
{
    drmModeObjectPropertiesPtr props =
        drmModeObjectGetProperties(fd, object_id, object_type);
    uint32_t prop_id = 0;
    uint32_t i;

    for (i = 0; props && i < props->count_props && !prop_id; i++) {
        drmModePropertyPtr prop = drmModeGetProperty(fd, props->props[i]);

        if (prop && strcmp(prop->name, name) == 0) {
            prop_id = prop->prop_id;
            if (value)
                *value = props->prop_values[i];
        }
        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
    return prop_id;
}

static Bool
drmmode_colorop_enum(int fd, uint32_t prop_id, const char *name,
                     uint64_t *value)
{
    drmModePropertyPtr prop = drmModeGetProperty(fd, prop_id);
    Bool found = FALSE;
    int i;

    for (i = 0; prop && i < prop->count_enums && !found; i++) {
        if (strcmp(prop->enums[i].name, name) == 0) {
            *value = prop->enums[i].value;
            found = TRUE;
        }
    }
    drmModeFreeProperty(prop);
    return found;
}

static Bool
drmmode_colorop_value(int fd, uint32_t object_id, uint32_t object_type,
                      uint32_t prop_id, uint64_t *value)
{
    drmModeObjectPropertiesPtr props =
        drmModeObjectGetProperties(fd, object_id, object_type);
    Bool found = FALSE;
    uint32_t i;

    for (i = 0; props && i < props->count_props && !found; i++) {
        if (props->props[i] == prop_id) {
            *value = props->prop_values[i];
            found = TRUE;
        }
    }
    drmModeFreeObjectProperties(props);
    return found;
}

static int
drmmode_colorop_set(drmModeAtomicReq *req, uint32_t id, uint32_t prop,
                    uint64_t value)
{
    return drmModeAtomicAddProperty(req, id, prop, value) <= 0 ? -1 : 0;
}

static void
drmmode_plane_color_destroy_pq_map(int fd, drmmode_plane_pq_map_rec *pq_map)
{
    if (pq_map->shaper_blob)
        drmModeDestroyPropertyBlob(fd, pq_map->shaper_blob);
    if (pq_map->lut3d_blob)
        drmModeDestroyPropertyBlob(fd, pq_map->lut3d_blob);
    pq_map->shaper_blob = pq_map->lut3d_blob = 0;
}

#ifdef DRM_CLIENT_CAP_PLANE_COLOR_PIPELINE

/* The "PQ 125" curves map 10000 nits to 125.0 */
#define COLOROP_PQ_125_NITS 80.0

static Bool
drmmode_colorop_use(int fd, drmmode_plane_color_ptr color, int index,
                    uint64_t type, const char *value_name,
                    const char *curve_name, drmmode_colorop_use_ptr use)
{
    drmmode_colorop_ptr op = &color->ops[index];
    uint64_t op_type;

    if (!drmmode_colorop_prop(fd, op->id, DRM_MODE_OBJECT_COLOROP, "TYPE",
                              &op_type) || op_type != type)
        return FALSE;

    use->value_prop = drmmode_colorop_prop(fd, op->id, DRM_MODE_OBJECT_COLOROP,
                                           value_name, NULL);
    if (!use->value_prop ||
        (curve_name &&
         !drmmode_colorop_enum(fd, use->value_prop, curve_name, &use->curve)))
        return FALSE;

    use->index = index;
    return TRUE;
}

static Bool
drmmode_colorop_use_lut(int fd, drmmode_plane_color_ptr color, int index,
                        uint64_t type, uint64_t max_size,
                        drmmode_colorop_use_ptr use)
{
    if (!drmmode_colorop_use(fd, color, index, type, "DATA", NULL, use) ||
        !drmmode_colorop_prop(fd, color->ops[index].id, DRM_MODE_OBJECT_COLOROP,
                              "SIZE", &use->size) ||
        use->size < 2 || use->size > max_size) {
        use->index = -1;
        return FALSE;
    }
    return TRUE;
}

static void
drmmode_plane_color_parse_pq_map(int fd, drmmode_plane_color_ptr color)
{
    drmmode_plane_pq_map_rec *pq_map = &color->pq_map;
    int i;

    pq_map->shaper.index = pq_map->lut3d.index = -1;
    for (i = color->sdr_to_pq.inverse_eotf.index + 1; i < color->num_ops; i++) {
        if (pq_map->shaper.index < 0)
            drmmode_colorop_use_lut(fd, color, i, DRM_COLOROP_1D_LUT, 65536,
                                    &pq_map->shaper);
        else if (drmmode_colorop_use_lut(fd, color, i, DRM_COLOROP_3D_LUT, 65,
                                         &pq_map->lut3d))
            break;
    }
}

static Bool
drmmode_plane_color_parse(int fd, uint64_t pipeline,
                          drmmode_plane_color_ptr color)
{
    drmmode_plane_sdr_to_pq_rec *sdr_to_pq = &color->sdr_to_pq;
    uint64_t id = pipeline;
    int i, step = 0;

    color->num_ops = 0;
    while (id && color->num_ops < ARRAY_SIZE(color->ops)) {
        drmmode_colorop_ptr op = &color->ops[color->num_ops];

        op->id = id;
        op->bypass_prop = drmmode_colorop_prop(fd, id, DRM_MODE_OBJECT_COLOROP,
                                               "BYPASS", NULL);
        if (!drmmode_colorop_prop(fd, id, DRM_MODE_OBJECT_COLOROP, "NEXT", &id))
            return FALSE;
        color->num_ops++;
    }
    if (id)
        return FALSE;

    for (i = 0; i < color->num_ops && step < 4; i++) {
        switch (step) {
        case 0:
            step += drmmode_colorop_use(fd, color, i, DRM_COLOROP_1D_CURVE,
                                        "CURVE_1D_TYPE", "Gamma 2.2",
                                        &sdr_to_pq->eotf);
            break;
        case 1:
            step += drmmode_colorop_use(fd, color, i, DRM_COLOROP_MULTIPLIER,
                                        "MULTIPLIER", NULL,
                                        &sdr_to_pq->multiplier);
            break;
        case 2:
            step += drmmode_colorop_use(fd, color, i, DRM_COLOROP_CTM_3X4,
                                        "DATA", NULL, &sdr_to_pq->matrix);
            break;
        case 3:
            step += drmmode_colorop_use(fd, color, i, DRM_COLOROP_1D_CURVE,
                                        "CURVE_1D_TYPE", "PQ 125 Inverse EOTF",
                                        &sdr_to_pq->inverse_eotf);
            break;
        }
    }
    if (step < 4)
        return FALSE;

    for (i = 0; i < color->num_ops; i++) {
        if (!color->ops[i].bypass_prop)
            return FALSE;
    }

    drmmode_plane_color_parse_pq_map(fd, color);
    color->pipeline = pipeline;
    return TRUE;
}

static void
drmmode_plane_color_pipeline_init(int fd, uint32_t plane_id,
                                  drmmode_plane_color_ptr color)
{
    drmModePropertyPtr prop;
    uint32_t prop_id;
    int i;

    prop_id = drmmode_colorop_prop(fd, plane_id, DRM_MODE_OBJECT_PLANE,
                                   "COLOR_PIPELINE", NULL);
    prop = prop_id ? drmModeGetProperty(fd, prop_id) : NULL;
    for (i = 0; prop && i < prop->count_enums; i++) {
        if (prop->enums[i].value &&
            drmmode_plane_color_parse(fd, prop->enums[i].value, color)) {
            color->pipeline_prop = prop_id;
            break;
        }
    }
    drmModeFreeProperty(prop);
}

static uint64_t
drmmode_s31_32(double value)
{
    uint64_t magnitude = (uint64_t) ((value < 0 ? -value : value) *
                                     4294967296.0 + 0.5);

    return value < 0 ? magnitude | (1ULL << 63) : magnitude;
}

/* amdgpu resets a plane whenever any of its colorops changes, which turns
 * the commit into a full update that completes vblanks late. A new blob
 * counts as a change even when its contents are equal, so the matrix blob is
 * kept until it changes or the pipeline has to be reprogrammed.
 */
static int
drmmode_plane_color_update_ctm(xf86CrtcPtr crtc, drmmode_plane_ctm_rec *ctm,
                               Bool reprogram)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    int fd = drmmode_crtc->drmmode->fd;
    struct drm_color_ctm_3x4 wanted;
    double matrix[9];
    uint32_t blob;
    int i;

    ms_hdr_desktop_matrix(drmmode_crtc_user_ctm(crtc), matrix);
    memset(&wanted, 0, sizeof(wanted));
    for (i = 0; i < 9; i++)
        wanted.matrix[i / 3 * 4 + i % 3] = drmmode_s31_32(matrix[i]);

    if (ctm->blob && !reprogram &&
        memcmp(ctm->matrix, wanted.matrix, sizeof(ctm->matrix)) == 0)
        return 0;

    if (drmModeCreatePropertyBlob(fd, &wanted, sizeof(wanted), &blob))
        return -1;

    if (ctm->blob)
        drmModeDestroyPropertyBlob(fd, ctm->blob);
    ctm->blob = blob;
    memcpy(ctm->matrix, wanted.matrix, sizeof(ctm->matrix));
    return 0;
}

static int
drmmode_plane_color_use_ops(drmModeAtomicReq *req, drmmode_plane_color_ptr color,
                            uint32_t used)
{
    int i, ret = 0;

    for (i = 0; i < color->num_ops; i++)
        ret |= drmmode_colorop_set(req, color->ops[i].id, color->ops[i].bypass_prop,
                                   !(used & (1u << i)));
    return ret;
}

static int
drmmode_plane_color_add_sdr_to_pq(drmModeAtomicReq *req, xf86CrtcPtr crtc,
                                  drmmode_plane_color_ptr color, int sdr_white_level,
                                  Bool reprogram)
{
    drmmode_plane_sdr_to_pq_rec *sdr_to_pq = &color->sdr_to_pq;
    int ret;

    if (drmmode_plane_color_update_ctm(crtc, &sdr_to_pq->ctm, reprogram))
        return -1;

    ret = drmmode_plane_color_use_ops(req, color,
                                      1u << sdr_to_pq->eotf.index |
                                      1u << sdr_to_pq->multiplier.index |
                                      1u << sdr_to_pq->matrix.index |
                                      1u << sdr_to_pq->inverse_eotf.index);
    ret |= drmmode_colorop_set(req, color->ops[sdr_to_pq->eotf.index].id,
                               sdr_to_pq->eotf.value_prop, sdr_to_pq->eotf.curve);
    ret |= drmmode_colorop_set(req, color->ops[sdr_to_pq->multiplier.index].id,
                               sdr_to_pq->multiplier.value_prop,
                               drmmode_s31_32(sdr_white_level / COLOROP_PQ_125_NITS));
    ret |= drmmode_colorop_set(req, color->ops[sdr_to_pq->matrix.index].id,
                               sdr_to_pq->matrix.value_prop, sdr_to_pq->ctm.blob);
    ret |= drmmode_colorop_set(req, color->ops[sdr_to_pq->inverse_eotf.index].id,
                               sdr_to_pq->inverse_eotf.value_prop,
                               sdr_to_pq->inverse_eotf.curve);
    return ret;
}

static uint32_t
drmmode_lut32_value(double value)
{
    return llround(fmin(fmax(value, 0.0), 1.0) * UINT32_MAX);
}

static uint32_t
drmmode_lut32_blob(int fd, const struct drm_color_lut32 *lut, size_t count)
{
    uint32_t blob;

    if (drmModeCreatePropertyBlob(fd, lut, count * sizeof(*lut), &blob))
        return 0;
    return blob;
}

static Bool
drmmode_plane_color_uses_lut3d(const drmmode_plane_pq_map_rec *pq_map,
                               const ms_hdr_pq_map_rec *map)
{
    return pq_map->lut3d.index >= 0 && map->gamut_map;
}

static uint32_t
drmmode_plane_color_shaper_blob(int fd, const drmmode_plane_pq_map_rec *pq_map,
                                const ms_hdr_pq_map_rec *map)
{
    int size = pq_map->shaper.size;
    struct drm_color_lut32 *lut = calloc(size, sizeof(*lut));
    uint32_t blob = 0;
    int i;

    if (!lut)
        return 0;

    for (i = 0; i < size; i++) {
        double pq = (double) i / (size - 1);
        uint32_t value = drmmode_lut32_value(drmmode_plane_color_uses_lut3d(pq_map, map) ?
                                             ms_hdr_pq_map_shaper(map, pq) :
                                             ms_hdr_pq_map_tone(map, pq));

        lut[i] = (struct drm_color_lut32) {
            .red = value,
            .green = value,
            .blue = value,
        };
    }

    blob = drmmode_lut32_blob(fd, lut, size);
    free(lut);
    return blob;
}

static uint32_t
drmmode_plane_color_lut3d_blob(int fd, const drmmode_plane_pq_map_rec *pq_map,
                               const ms_hdr_pq_map_rec *map)
{
    int size = pq_map->lut3d.size;
    int count = size * size * size;
    struct drm_color_lut32 *lut = calloc(count, sizeof(*lut));
    double *values = calloc(count * 3, sizeof(*values));
    uint32_t blob = 0;
    int i;

    if (lut && values && ms_hdr_pq_map_lut3d(map, size, values)) {
        for (i = 0; i < count; i++) {
            lut[i] = (struct drm_color_lut32) {
                .red = drmmode_lut32_value(values[i * 3]),
                .green = drmmode_lut32_value(values[i * 3 + 1]),
                .blue = drmmode_lut32_value(values[i * 3 + 2]),
            };
        }
        blob = drmmode_lut32_blob(fd, lut, count);
    }

    free(values);
    free(lut);
    return blob;
}

/* Like the matrix blob, the lookup table blobs are kept until the map changes
 * or the pipeline has to be reprogrammed.
 */
static int
drmmode_plane_color_update_pq_map(int fd, drmmode_plane_pq_map_rec *pq_map,
                                  const ms_hdr_pq_map_rec *map, Bool reprogram)
{
    drmmode_plane_pq_map_rec updated = *pq_map;

    if (pq_map->shaper_blob && !reprogram &&
        memcmp(&pq_map->map, map, sizeof(*map)) == 0)
        return 0;

    updated.map = *map;
    updated.shaper_blob = drmmode_plane_color_shaper_blob(fd, pq_map, map);
    updated.lut3d_blob = drmmode_plane_color_uses_lut3d(pq_map, map) ?
        drmmode_plane_color_lut3d_blob(fd, pq_map, map) : 0;
    if (!updated.shaper_blob ||
        (drmmode_plane_color_uses_lut3d(pq_map, map) && !updated.lut3d_blob)) {
        drmmode_plane_color_destroy_pq_map(fd, &updated);
        return -1;
    }

    drmmode_plane_color_destroy_pq_map(fd, pq_map);
    *pq_map = updated;
    return 0;
}

static int
drmmode_plane_color_add_pq_map(drmModeAtomicReq *req, xf86CrtcPtr crtc,
                               drmmode_plane_color_ptr color,
                               const ms_hdr_pq_map_rec *map, Bool reprogram)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_plane_pq_map_rec *pq_map = &color->pq_map;
    Bool lut3d = drmmode_plane_color_uses_lut3d(pq_map, map);
    int ret;

    if (drmmode_plane_color_update_pq_map(drmmode_crtc->drmmode->fd, pq_map, map,
                                          reprogram))
        return -1;

    ret = drmmode_plane_color_use_ops(req, color, 1u << pq_map->shaper.index |
                                      (lut3d ? 1u << pq_map->lut3d.index : 0));
    ret |= drmmode_colorop_set(req, color->ops[pq_map->shaper.index].id,
                               pq_map->shaper.value_prop, pq_map->shaper_blob);
    if (lut3d)
        ret |= drmmode_colorop_set(req, color->ops[pq_map->lut3d.index].id,
                                   pq_map->lut3d.value_prop, pq_map->lut3d_blob);
    return ret;
}

int
drmmode_plane_color_add_props(drmModeAtomicReq *req, xf86CrtcPtr crtc,
                              uint32_t plane_id, drmmode_plane_color_ptr color,
                              drmmode_plane_color_mode mode,
                              const drmmode_hdr_state_rec *state, Bool reprogram)
{
    int ret = drmmode_colorop_set(req, plane_id, color->pipeline_prop,
                                  mode == DRMMODE_PLANE_COLOR_BYPASS ? 0 : color->pipeline);

    if (ret || mode == DRMMODE_PLANE_COLOR_BYPASS)
        return ret;
    if (mode == DRMMODE_PLANE_COLOR_SDR_TO_PQ)
        return drmmode_plane_color_add_sdr_to_pq(req, crtc, color, state->sdr_white_level,
                                                 reprogram);
    return drmmode_plane_color_add_pq_map(req, crtc, color, &state->pq_map, reprogram);
}

#else
static void
drmmode_plane_color_pipeline_init(int fd, uint32_t plane_id,
                                  drmmode_plane_color_ptr color)
{
}

int
drmmode_plane_color_add_props(drmModeAtomicReq *req, xf86CrtcPtr crtc,
                              uint32_t plane_id, drmmode_plane_color_ptr color,
                              drmmode_plane_color_mode mode,
                              const drmmode_hdr_state_rec *state, Bool reprogram)
{
    return 0;
}
#endif

void
drmmode_plane_color_fini(xf86CrtcPtr crtc, drmmode_plane_color_ptr color)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    int fd = drmmode_crtc->drmmode->fd;

    if (color->sdr_to_pq.ctm.blob)
        drmModeDestroyPropertyBlob(fd, color->sdr_to_pq.ctm.blob);
    color->sdr_to_pq.ctm.blob = 0;
    drmmode_plane_color_destroy_pq_map(fd, &color->pq_map);
}

Bool
drmmode_plane_color_maps_pq(const drmmode_plane_color_rec *color,
                            const ms_hdr_pq_map_rec *map)
{
    const drmmode_plane_pq_map_rec *pq_map = &color->pq_map;

    if (!color->pipeline_prop || pq_map->shaper.index < 0)
        return FALSE;
    return map->tone_map || (map->gamut_map && pq_map->lut3d.index >= 0);
}

static void
drmmode_plane_degamma_tf_init(int fd, uint32_t plane_id,
                              drmmode_plane_degamma_tf_ptr tf)
{
    uint32_t prop = drmmode_colorop_prop(fd, plane_id, DRM_MODE_OBJECT_PLANE,
                                         "NV_PLANE_DEGAMMA_TF", NULL);

    if (prop &&
        drmmode_colorop_enum(fd, prop, "Default", &tf->default_value) &&
        drmmode_colorop_enum(fd, prop, "Linear", &tf->linear_value))
        tf->prop = prop;
}

void
drmmode_plane_color_init(xf86CrtcPtr crtc, uint32_t plane_id,
                         drmmode_plane_color_ptr color)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    drmmode_ptr drmmode = drmmode_crtc->drmmode;

    if (modesettingPTR(crtc->scrn)->atomic_modeset)
        drmmode_plane_degamma_tf_init(drmmode->fd, plane_id,
                                      &color->degamma_tf);
    if (drmmode->plane_color_pipeline)
        drmmode_plane_color_pipeline_init(drmmode->fd, plane_id, color);
}

/* nvidia-drm turns the CRTC's DEGAMMA_LUT into an input LUT on each plane.
 * When the CRTC colour properties change it makes new input LUTs, but only
 * hands them to the display when the degamma state of the plane itself
 * changes. The plane keeps reading its previous LUT, which nvidia-drm
 * meanwhile reuses for later LUTs of any CRTC, so monitors show wrong or
 * inverted colours. Switching between the equivalent "Default" and "Linear"
 * transfer functions changes the degamma state of the plane.
 */
int
drmmode_plane_color_refresh_degamma(drmModeAtomicReq *req, xf86CrtcPtr crtc,
                                    uint32_t plane_id,
                                    const drmmode_plane_color_rec *color)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;
    const drmmode_plane_degamma_tf_rec *tf = &color->degamma_tf;
    uint64_t value;

    if (!tf->prop)
        return 0;

    if (!drmmode_colorop_value(drmmode_crtc->drmmode->fd, plane_id,
                               DRM_MODE_OBJECT_PLANE, tf->prop, &value))
        return -1;

    return drmmode_colorop_set(req, plane_id, tf->prop,
                               value == tf->default_value ?
                               tf->linear_value : tf->default_value);
}

Bool
drmmode_plane_color_supported(xf86CrtcPtr crtc)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    return drmmode_crtc->plane_color.pipeline_prop != 0;
}
