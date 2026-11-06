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
drmmode_plane_color_parse(int fd, uint64_t pipeline,
                          drmmode_plane_color_ptr color)
{
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
                                        &color->eotf);
            break;
        case 1:
            step += drmmode_colorop_use(fd, color, i, DRM_COLOROP_MULTIPLIER,
                                        "MULTIPLIER", NULL,
                                        &color->multiplier);
            break;
        case 2:
            step += drmmode_colorop_use(fd, color, i, DRM_COLOROP_CTM_3X4,
                                        "DATA", NULL, &color->matrix);
            break;
        case 3:
            step += drmmode_colorop_use(fd, color, i, DRM_COLOROP_1D_CURVE,
                                        "CURVE_1D_TYPE", "PQ 125 Inverse EOTF",
                                        &color->inverse_eotf);
            break;
        }
    }
    if (step < 4)
        return FALSE;

    for (i = 0; i < color->num_ops; i++) {
        if (!color->ops[i].bypass_prop)
            return FALSE;
    }

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

int
drmmode_plane_color_add_props(drmModeAtomicReq *req, xf86CrtcPtr crtc,
                              uint32_t plane_id, drmmode_plane_color_ptr color,
                              Bool sdr_to_pq, int sdr_white_level,
                              Bool reprogram)
{
    int i, ret;

    ret = drmmode_colorop_set(req, plane_id, color->pipeline_prop,
                              sdr_to_pq ? color->pipeline : 0);
    if (ret || !sdr_to_pq)
        return ret;

    if (drmmode_plane_color_update_ctm(crtc, &color->ctm, reprogram))
        return -1;

    for (i = 0; i < color->num_ops; i++) {
        Bool used = i == color->eotf.index || i == color->multiplier.index ||
            i == color->matrix.index || i == color->inverse_eotf.index;

        ret |= drmmode_colorop_set(req, color->ops[i].id,
                                   color->ops[i].bypass_prop, !used);
    }

    ret |= drmmode_colorop_set(req, color->ops[color->eotf.index].id,
                               color->eotf.value_prop, color->eotf.curve);
    ret |= drmmode_colorop_set(req, color->ops[color->multiplier.index].id,
                               color->multiplier.value_prop,
                               drmmode_s31_32(sdr_white_level /
                                              COLOROP_PQ_125_NITS));
    ret |= drmmode_colorop_set(req, color->ops[color->matrix.index].id,
                               color->matrix.value_prop, color->ctm.blob);
    ret |= drmmode_colorop_set(req, color->ops[color->inverse_eotf.index].id,
                               color->inverse_eotf.value_prop,
                               color->inverse_eotf.curve);
    return ret;
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
                              Bool sdr_to_pq, int sdr_white_level,
                              Bool reprogram)
{
    return 0;
}
#endif

void
drmmode_plane_color_fini(xf86CrtcPtr crtc, drmmode_plane_color_ptr color)
{
    drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

    if (color->ctm.blob)
        drmModeDestroyPropertyBlob(drmmode_crtc->drmmode->fd, color->ctm.blob);
    color->ctm.blob = 0;
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
