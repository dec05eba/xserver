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

#include <math.h>
#include <string.h>

#include <xf86.h>
#include <xf86Crtc.h>
#include <property.h>
#include <propertyst.h>
#include <compositeext.h>

#include "driver.h"
#include "drmmode_display.h"

#define HDMI_STATIC_METADATA_TYPE1 0
#define HDMI_EOTF_SMPTE_ST2084 2

#define CTA_EXTENSION_TAG 0x02
#define CTA_DATA_BLOCK_EXTENDED 7
#define CTA_EXTENDED_HDR_STATIC_METADATA 6
#define CTA_EOTF_SMPTE_ST2084 (1 << 2)

#define SDR_GAMMA 2.2

static Atom hdr_output_metadata_atom;

const struct hdr_output_metadata *
ms_hdr_window_metadata(WindowPtr window)
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

        metadata = ms_hdr_window_metadata(child);
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

    if (!ms->output_mode_dirty || !scrn->vtSema || !screen->root)
        return;

    ms->output_mode_dirty = FALSE;

    for (i = 0; i < config->num_crtc; i++) {
        xf86CrtcPtr crtc = config->crtc[i];
        drmmode_crtc_private_ptr drmmode_crtc = crtc->driver_private;

        if (!crtc->enabled)
            continue;

        if (ms->drmmode.tearfree_enable)
            drmmode_crtc->tearfree.scanout.output_mode_dirty = TRUE;
        else
            drmmode_crtc_set_hdr_output_metadata(crtc,
                ms_fullscreen_hdr_output_metadata(screen->root, &crtc->bounds),
                FALSE);
    }
}

void
ms_hdr_disable(ScrnInfoPtr scrn)
{
    xf86CrtcConfigPtr config = XF86_CRTC_CONFIG_PTR(scrn);
    int i;

    for (i = 0; i < config->num_crtc; i++)
        drmmode_crtc_disable_hdr(config->crtc[i]);
}

Bool
ms_hdr_edid_supports_pq(const uint8_t *edid, size_t size)
{
    size_t block;

    for (block = 128; block + 128 <= size; block += 128) {
        const uint8_t *ext = edid + block;
        int end = ext[2];
        int i, len;

        if (ext[0] != CTA_EXTENSION_TAG || end > 127)
            continue;

        for (i = 4; i < end; i += len + 1) {
            len = ext[i] & 0x1f;
            if (i + len >= end)
                break;

            if (ext[i] >> 5 == CTA_DATA_BLOCK_EXTENDED && len >= 2 &&
                ext[i + 1] == CTA_EXTENDED_HDR_STATIC_METADATA)
                return (ext[i + 2] & CTA_EOTF_SMPTE_ST2084) != 0;
        }
    }

    return FALSE;
}

void
ms_hdr_desktop_metadata(int white_level, struct hdr_output_metadata *metadata)
{
    *metadata = (struct hdr_output_metadata) {
        .metadata_type = HDMI_STATIC_METADATA_TYPE1,
        .hdmi_metadata_type1 = {
            .eotf = HDMI_EOTF_SMPTE_ST2084,
            .metadata_type = HDMI_STATIC_METADATA_TYPE1,
            .display_primaries = {
                { .x = 32000, .y = 16500 },
                { .x = 15000, .y = 30000 },
                { .x = 7500, .y = 3000 },
            },
            .white_point = { .x = 15635, .y = 16450 },
            .max_display_mastering_luminance = white_level,
            .max_cll = white_level,
            .max_fall = white_level,
        },
    };
}

static const double bt709_to_bt2020[9] = {
    0.6274040, 0.3292820, 0.0433136,
    0.0690970, 0.9195400, 0.0113612,
    0.0163916, 0.0880132, 0.8955950,
};

static const double bt2020_to_bt709[9] = {
    1.6604903, -0.5876391, -0.0728516,
    -0.1245500, 1.1328999, -0.0083480,
    -0.0181511, -0.1005787, 1.1187299,
};

static const double pq_m1 = 2610.0 / 16384.0;
static const double pq_m2 = 2523.0 / 4096.0 * 128.0;
static const double pq_c1 = 3424.0 / 4096.0;
static const double pq_c2 = 2413.0 / 4096.0 * 32.0;
static const double pq_c3 = 2392.0 / 4096.0 * 32.0;

static uint16_t
ms_hdr_lut_value(double value)
{
    return lrint(fmin(fmax(value, 0.0), 1.0) * UINT16_MAX);
}

static double
ms_hdr_sample_ramp(const uint16_t *ramp, int size, double x)
{
    double pos = x * (size - 1);
    int i = min((int)pos, size - 2);
    double t = pos - i;

    return (ramp[i] * (1.0 - t) + ramp[i + 1] * t) / UINT16_MAX;
}

static uint16_t
ms_hdr_linearize_ramp(const uint16_t *ramp, int size, double x)
{
    return ms_hdr_lut_value(pow(ms_hdr_sample_ramp(ramp, size, x), SDR_GAMMA));
}

static double
ms_hdr_pq_eotf(double value)
{
    const double p = pow(value, 1.0 / pq_m2);

    return 10000.0 * pow(fmax(p - pq_c1, 0.0) / (pq_c2 - pq_c3 * p), 1.0 / pq_m1);
}

static double
ms_hdr_pq_inverse_eotf(double nits)
{
    const double y = pow(nits / 10000.0, pq_m1);

    return pow((pq_c1 + pq_c2 * y) / (1.0 + pq_c3 * y), pq_m2);
}

static double
ms_hdr_tone_map(double value, double peak)
{
    const double knee = 0.75;
    double t, peak_t;

    if (value <= knee || peak <= 1.0)
        return value;

    t = (value - knee) / (1.0 - knee);
    peak_t = (peak - knee) / (1.0 - knee);
    return knee + (1.0 - knee) * t * (1.0 + t / (peak_t * peak_t)) / (1.0 + t);
}

static double
ms_hdr_content_peak(const struct hdr_output_metadata *metadata, int white_level)
{
    double peak = 0.0;

    if (metadata) {
        peak = metadata->hdmi_metadata_type1.max_cll;
        if (!peak)
            peak = metadata->hdmi_metadata_type1.max_display_mastering_luminance;
    }
    if (!peak)
        peak = 1000.0;

    return fmin(fmax(peak, white_level), 10000.0);
}

static double
ms_hdr_ctm_to_double(uint64_t value)
{
    double magnitude = (value & ~(1ULL << 63)) / 4294967296.0;

    return value >> 63 ? -magnitude : magnitude;
}

static uint64_t
ms_hdr_double_to_ctm(double value)
{
    uint64_t magnitude = llround(fabs(value) * 4294967296.0);

    return value < 0 ? magnitude | (1ULL << 63) : magnitude;
}

static void
ms_hdr_matrix_multiply(const double *a, const double *b, double *out)
{
    int row, col, i;

    for (row = 0; row < 3; row++) {
        for (col = 0; col < 3; col++) {
            out[row * 3 + col] = 0.0;
            for (i = 0; i < 3; i++)
                out[row * 3 + col] += a[row * 3 + i] * b[i * 3 + col];
        }
    }
}

static void
ms_hdr_ctm_to_matrix(const struct drm_color_ctm *ctm, double *matrix)
{
    int i;

    for (i = 0; i < 9; i++)
        matrix[i] = ms_hdr_ctm_to_double(ctm->matrix[i]);
}

static void
ms_hdr_matrix_to_ctm(const double *matrix, struct drm_color_ctm *ctm)
{
    int i;

    for (i = 0; i < 9; i++)
        ctm->matrix[i] = ms_hdr_double_to_ctm(matrix[i]);
}

static void
ms_hdr_desktop_matrix(const struct drm_color_ctm *user_ctm, double *matrix)
{
    double user[9];

    ms_hdr_ctm_to_matrix(user_ctm, user);
    ms_hdr_matrix_multiply(bt709_to_bt2020, user, matrix);
}

void
ms_hdr_desktop_color(xf86CrtcPtr crtc, const struct drm_color_ctm *user_ctm,
                     int white_level, ms_hdr_color_rec *color)
{
    double matrix[9];
    int i;

    for (i = 0; i < color->degamma_size; i++) {
        double x = (double)i / (color->degamma_size - 1);

        color->degamma[i] = (struct drm_color_lut) {
            .red = ms_hdr_linearize_ramp(crtc->gamma_red, crtc->gamma_size, x),
            .green = ms_hdr_linearize_ramp(crtc->gamma_green, crtc->gamma_size, x),
            .blue = ms_hdr_linearize_ramp(crtc->gamma_blue, crtc->gamma_size, x),
        };
    }

    for (i = 0; i < color->gamma_size; i++) {
        double x = (double)i / (color->gamma_size - 1);
        uint16_t value =
            ms_hdr_lut_value(ms_hdr_pq_inverse_eotf(x * white_level));

        color->gamma[i] = (struct drm_color_lut) {
            .red = value,
            .green = value,
            .blue = value,
        };
    }

    ms_hdr_desktop_matrix(user_ctm, matrix);
    ms_hdr_matrix_to_ctm(matrix, &color->ctm);
}

void
ms_hdr_tonemap_color(xf86CrtcPtr crtc, const struct drm_color_ctm *user_ctm,
                     const struct hdr_output_metadata *metadata,
                     int white_level, ms_hdr_color_rec *color)
{
    double peak = ms_hdr_content_peak(metadata, white_level);
    double relative_peak = peak / white_level;
    double user[9], matrix[9];
    int i;

    for (i = 0; i < color->degamma_size; i++) {
        double x = (double)i / (color->degamma_size - 1);
        uint16_t value = ms_hdr_lut_value(ms_hdr_pq_eotf(x) / peak);

        color->degamma[i] = (struct drm_color_lut) {
            .red = value,
            .green = value,
            .blue = value,
        };
    }

    for (i = 0; i < color->gamma_size; i++) {
        double x = (double)i / (color->gamma_size - 1) * relative_peak;
        double sdr = pow(fmin(ms_hdr_tone_map(x, relative_peak), 1.0),
                         1.0 / SDR_GAMMA);

        color->gamma[i] = (struct drm_color_lut) {
            .red = ms_hdr_lut_value(ms_hdr_sample_ramp(crtc->gamma_red,
                                                       crtc->gamma_size, sdr)),
            .green = ms_hdr_lut_value(ms_hdr_sample_ramp(crtc->gamma_green,
                                                         crtc->gamma_size, sdr)),
            .blue = ms_hdr_lut_value(ms_hdr_sample_ramp(crtc->gamma_blue,
                                                        crtc->gamma_size, sdr)),
        };
    }

    ms_hdr_ctm_to_matrix(user_ctm, user);
    ms_hdr_matrix_multiply(user, bt2020_to_bt709, matrix);
    ms_hdr_matrix_to_ctm(matrix, &color->ctm);
}

#ifdef HAVE_GBM
static void
ms_hdr_set_conversion_matrix(const double *matrix,
                             glamor_color_conversion *conversion)
{
    int i;

    for (i = 0; i < 9; i++)
        conversion->matrix[i] = matrix[i];
}

void
ms_hdr_pq_to_sdr_conversion(const struct hdr_output_metadata *metadata,
                            int white_level,
                            glamor_color_conversion *conversion)
{
    conversion->src = GLAMOR_COLOR_PQ;
    conversion->dst = GLAMOR_COLOR_SDR;
    conversion->sdr_white = white_level;
    conversion->peak = ms_hdr_content_peak(metadata, white_level);
    ms_hdr_set_conversion_matrix(bt2020_to_bt709, conversion);
}

void
ms_hdr_sdr_to_pq_conversion(const struct drm_color_ctm *user_ctm,
                            int white_level,
                            glamor_color_conversion *conversion)
{
    double matrix[9];

    conversion->src = GLAMOR_COLOR_SDR;
    conversion->dst = GLAMOR_COLOR_PQ;
    conversion->sdr_white = white_level;
    conversion->peak = 10000.0;
    ms_hdr_desktop_matrix(user_ctm, matrix);
    ms_hdr_set_conversion_matrix(matrix, conversion);
}
#endif

static void
ms_hdr_post_validate_tree(WindowPtr parent, WindowPtr child, VTKind kind)
{
    ScreenPtr screen = (parent ? parent : child)->drawable.pScreen;
    modesettingPtr ms = modesettingPTR(xf86ScreenToScrn(screen));

    ms->output_mode_dirty = TRUE;

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

    if (rec->prop->propertyName != hdr_output_metadata_atom ||
        rec->win->drawable.pScreen != screen)
        return;

    modesettingPTR(xf86ScreenToScrn(screen))->output_mode_dirty = TRUE;
    CompositeServerSetHdrWindow(rec->win, rec->state != PropertyDelete &&
                                ms_hdr_window_metadata(rec->win));
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
