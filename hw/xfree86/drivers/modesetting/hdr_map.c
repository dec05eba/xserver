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
#include <stdlib.h>
#include <string.h>

#include <xf86.h>

#include "hdr_map.h"

#define METADATA_CHROMATICITY_SCALE 50000.0
#define PQ_MAX_LUMINANCE 10000.0

#define TONE_MAP_KNEE 0.75

#define GAMUT_THRESHOLD_MIN 0.8
#define GAMUT_THRESHOLD_MAX 0.99
#define GAMUT_LIMIT_MIN 1.03
#define GAMUT_POWER 1.2

typedef struct {
    double to_display[9];
    double from_display[9];
    double threshold[3];
    double limit[3];
} ms_hdr_gamut_rec;

static const double pq_m1 = 2610.0 / 16384.0;
static const double pq_m2 = 2523.0 / 4096.0 * 128.0;
static const double pq_c1 = 3424.0 / 4096.0;
static const double pq_c2 = 2413.0 / 4096.0 * 32.0;
static const double pq_c3 = 2392.0 / 4096.0 * 32.0;

static const double d65[2] = { 0.3127, 0.3290 };
static const double bt709_primaries[6] = { 0.640, 0.330, 0.300, 0.600, 0.150, 0.060 };
static const double bt2020_primaries[6] = { 0.708, 0.292, 0.170, 0.797, 0.131, 0.046 };
static const double identity_matrix[9] = {
    1.0, 0.0, 0.0,
    0.0, 1.0, 0.0,
    0.0, 0.0, 1.0,
};

double
ms_hdr_pq_eotf(double value)
{
    const double p = pow(value, 1.0 / pq_m2);

    return PQ_MAX_LUMINANCE * pow(fmax(p - pq_c1, 0.0) / (pq_c2 - pq_c3 * p), 1.0 / pq_m1);
}

double
ms_hdr_pq_inverse_eotf(double nits)
{
    const double y = pow(nits / PQ_MAX_LUMINANCE, pq_m1);

    return pow((pq_c1 + pq_c2 * y) / (1.0 + pq_c3 * y), pq_m2);
}

double
ms_hdr_tone_map(double value, double peak)
{
    double t, peak_t;

    if (value <= TONE_MAP_KNEE || peak <= 1.0)
        return value;

    t = (value - TONE_MAP_KNEE) / (1.0 - TONE_MAP_KNEE);
    peak_t = (peak - TONE_MAP_KNEE) / (1.0 - TONE_MAP_KNEE);
    return TONE_MAP_KNEE + (1.0 - TONE_MAP_KNEE) * t * (1.0 + t / (peak_t * peak_t)) / (1.0 + t);
}

void
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
ms_hdr_matrix_apply(const double *matrix, const double *in, double *out)
{
    int row;

    for (row = 0; row < 3; row++)
        out[row] = matrix[row * 3] * in[0] + matrix[row * 3 + 1] * in[1] +
            matrix[row * 3 + 2] * in[2];
}

static Bool
ms_hdr_matrix_invert(const double *m, double *out)
{
    double det = m[0] * (m[4] * m[8] - m[5] * m[7]) -
        m[1] * (m[3] * m[8] - m[5] * m[6]) +
        m[2] * (m[3] * m[7] - m[4] * m[6]);

    if (fabs(det) < 1e-12)
        return FALSE;

    out[0] = (m[4] * m[8] - m[5] * m[7]) / det;
    out[1] = (m[2] * m[7] - m[1] * m[8]) / det;
    out[2] = (m[1] * m[5] - m[2] * m[4]) / det;
    out[3] = (m[5] * m[6] - m[3] * m[8]) / det;
    out[4] = (m[0] * m[8] - m[2] * m[6]) / det;
    out[5] = (m[2] * m[3] - m[0] * m[5]) / det;
    out[6] = (m[3] * m[7] - m[4] * m[6]) / det;
    out[7] = (m[1] * m[6] - m[0] * m[7]) / det;
    out[8] = (m[0] * m[4] - m[1] * m[3]) / det;
    return TRUE;
}

static Bool
ms_hdr_rgb_to_xyz(const double *primaries, double *matrix)
{
    const double white[3] = { d65[0] / d65[1], 1.0, (1.0 - d65[0] - d65[1]) / d65[1] };
    double columns[9], inverse[9], scale[3];
    int i;

    for (i = 0; i < 3; i++) {
        double x = primaries[i * 2], y = primaries[i * 2 + 1];

        columns[i] = x / y;
        columns[3 + i] = 1.0;
        columns[6 + i] = (1.0 - x - y) / y;
    }

    if (!ms_hdr_matrix_invert(columns, inverse))
        return FALSE;

    ms_hdr_matrix_apply(inverse, white, scale);
    for (i = 0; i < 9; i++)
        matrix[i] = columns[i] * scale[i % 3];
    return TRUE;
}

int
ms_hdr_metadata_peak(const struct hdr_output_metadata *metadata)
{
    if (!metadata)
        return 0;
    if (metadata->hdmi_metadata_type1.max_cll)
        return metadata->hdmi_metadata_type1.max_cll;
    return metadata->hdmi_metadata_type1.max_display_mastering_luminance;
}

/* The content's peak when it's brighter than the display's, otherwise 0. */
int
ms_hdr_tone_map_peak(const struct hdr_output_metadata *content, int display_peak)
{
    int peak = fmin(ms_hdr_metadata_peak(content), PQ_MAX_LUMINANCE);

    return display_peak && peak > display_peak ? peak : 0;
}

static double
ms_hdr_cross(const double *a, const double *b, const double *point)
{
    return (b[0] - a[0]) * (point[1] - a[1]) - (b[1] - a[1]) * (point[0] - a[0]);
}

Bool
ms_hdr_gamut_valid(const double *primaries)
{
    double side[3];
    int i;

    for (i = 0; i < 6; i++) {
        if (primaries[i] <= 0.0 || primaries[i] >= 1.0)
            return FALSE;
    }

    for (i = 0; i < 3; i++)
        side[i] = ms_hdr_cross(primaries + i * 2, primaries + (i + 1) % 3 * 2, d65);

    return (side[0] > 0.0 && side[1] > 0.0 && side[2] > 0.0) ||
        (side[0] < 0.0 && side[1] < 0.0 && side[2] < 0.0);
}

static Bool
ms_hdr_metadata_gamut(const struct hdr_metadata_infoframe *info, double *primaries)
{
    int order[3] = { 0, 1, 2 };
    int i;

    for (i = 1; i < 3; i++) {
        if (info->display_primaries[i].x > info->display_primaries[order[0]].x)
            order[0] = i;
    }
    order[1] = (order[0] + 1) % 3;
    order[2] = (order[0] + 2) % 3;
    if (info->display_primaries[order[2]].y > info->display_primaries[order[1]].y) {
        int green = order[2];

        order[2] = order[1];
        order[1] = green;
    }

    for (i = 0; i < 3; i++) {
        primaries[i * 2] = info->display_primaries[order[i]].x / METADATA_CHROMATICITY_SCALE;
        primaries[i * 2 + 1] = info->display_primaries[order[i]].y / METADATA_CHROMATICITY_SCALE;
    }
    return ms_hdr_gamut_valid(primaries);
}

/* Monitors often list the sRGB primaries in the base EDID block. They describe
 * the monitor's SDR mode, not the colours its panel shows in HDR.
 */
static Bool
ms_hdr_display_gamut(const struct hdr_metadata_infoframe *info, double *primaries)
{
    int i;

    if (!ms_hdr_metadata_gamut(info, primaries))
        return FALSE;

    for (i = 0; i < 6; i++) {
        if (fabs(primaries[i] - bt709_primaries[i]) > 0.0025)
            return TRUE;
    }
    return FALSE;
}

static void
ms_hdr_store_primaries(const double *primaries, uint16_t (*out)[2])
{
    int i;

    for (i = 0; i < 6; i++)
        out[i / 2][i % 2] = lrint(primaries[i] * METADATA_CHROMATICITY_SCALE);
}

static void
ms_hdr_load_primaries(const uint16_t (*primaries)[2], double *out)
{
    int i;

    for (i = 0; i < 6; i++)
        out[i] = primaries[i / 2][i % 2] / METADATA_CHROMATICITY_SCALE;
}

static double
ms_hdr_gamut_distance(const double *color, int channel)
{
    double peak = fmax(fmax(color[0], color[1]), color[2]);

    return peak > 0.0 ? (peak - color[channel]) / peak : 0.0;
}

static Bool
ms_hdr_gamut_distances(const double *xyz_to_display, const double *primaries,
                       double *distances)
{
    double to_xyz[9], in_display[9];
    int channel, i;

    if (!ms_hdr_rgb_to_xyz(primaries, to_xyz))
        return FALSE;

    ms_hdr_matrix_multiply(xyz_to_display, to_xyz, in_display);
    for (channel = 0; channel < 3; channel++) {
        distances[channel] = 0.0;
        for (i = 0; i < 3; i++) {
            const double primary[3] = { in_display[i], in_display[3 + i], in_display[6 + i] };

            distances[channel] = fmax(distances[channel], ms_hdr_gamut_distance(primary, channel));
        }
    }
    return TRUE;
}

/* Colours are compressed towards the achromatic axis in the display's RGB,
 * like the ACES reference gamut compression. The furthest content primary
 * lands on the edge of the display's gamut, and colours within the BT.709
 * gamut, such as the SDR desktop, are left alone where the display allows it.
 */
static Bool
ms_hdr_gamut_init(const ms_hdr_pq_map_rec *map, ms_hdr_gamut_rec *gamut)
{
    double content[6], display_gamut[6], display_to_xyz[9], xyz_to_display[9];
    double bt2020_to_xyz[9], limits[3], thresholds[3];
    Bool compress = FALSE;
    int i;

    ms_hdr_load_primaries(map->content_primaries, content);
    ms_hdr_load_primaries(map->display_primaries, display_gamut);
    if (!ms_hdr_rgb_to_xyz(display_gamut, display_to_xyz) ||
        !ms_hdr_matrix_invert(display_to_xyz, xyz_to_display) ||
        !ms_hdr_rgb_to_xyz(bt2020_primaries, bt2020_to_xyz) ||
        !ms_hdr_gamut_distances(xyz_to_display, content, limits) ||
        !ms_hdr_gamut_distances(xyz_to_display, bt709_primaries, thresholds))
        return FALSE;

    ms_hdr_matrix_multiply(xyz_to_display, bt2020_to_xyz, gamut->to_display);
    if (!ms_hdr_matrix_invert(gamut->to_display, gamut->from_display))
        return FALSE;

    for (i = 0; i < 3; i++) {
        gamut->threshold[i] = fmin(fmax(thresholds[i], GAMUT_THRESHOLD_MIN), GAMUT_THRESHOLD_MAX);
        gamut->limit[i] = limits[i] > GAMUT_LIMIT_MIN ? limits[i] : 0.0;
        compress |= gamut->limit[i] > 0.0;
    }
    return compress;
}

static void
ms_hdr_gamut_identity(ms_hdr_gamut_rec *gamut)
{
    memset(gamut, 0, sizeof(*gamut));
    memcpy(gamut->to_display, identity_matrix, sizeof(identity_matrix));
    memcpy(gamut->from_display, identity_matrix, sizeof(identity_matrix));
}

static double
ms_hdr_gamut_compress(double distance, double threshold, double limit)
{
    double scale, normalized;

    if (distance <= threshold)
        return distance;

    scale = (limit - threshold) /
        pow(pow((1.0 - threshold) / (limit - threshold), -GAMUT_POWER) - 1.0, 1.0 / GAMUT_POWER);
    normalized = (distance - threshold) / scale;
    return threshold + scale * normalized /
        pow(1.0 + pow(normalized, GAMUT_POWER), 1.0 / GAMUT_POWER);
}

Bool
ms_hdr_pq_map_init(ms_hdr_pq_map_rec *map,
                   const struct hdr_output_metadata *content,
                   const struct hdr_output_metadata *output, Bool tone_map)
{
    double content_gamut[6], display_gamut[6];
    ms_hdr_gamut_rec gamut;

    memset(map, 0, sizeof(*map));

    map->display_peak = output->hdmi_metadata_type1.max_cll;
    if (tone_map)
        map->content_peak = ms_hdr_tone_map_peak(content, map->display_peak);
    map->tone_map = map->content_peak != 0;

    if (ms_hdr_display_gamut(&output->hdmi_metadata_type1, display_gamut)) {
        if (!ms_hdr_metadata_gamut(&content->hdmi_metadata_type1, content_gamut))
            memcpy(content_gamut, bt2020_primaries, sizeof(content_gamut));

        ms_hdr_store_primaries(content_gamut, map->content_primaries);
        ms_hdr_store_primaries(display_gamut, map->display_primaries);
        map->gamut_map = ms_hdr_gamut_init(map, &gamut);
        if (!map->gamut_map) {
            memset(map->content_primaries, 0, sizeof(map->content_primaries));
            memset(map->display_primaries, 0, sizeof(map->display_primaries));
        }
    }

    if (!map->tone_map && !map->gamut_map)
        map->display_peak = 0;
    return map->tone_map || map->gamut_map;
}

/* The ITU-R BT.2390 EETF. The further the content's peak is above the
 * display's, the lower the knee, so highlights keep their contrast.
 */
double
ms_hdr_pq_map_tone(const ms_hdr_pq_map_rec *map, double pq)
{
    double content, display, value, knee, t;

    if (!map->tone_map)
        return pq;

    content = ms_hdr_pq_inverse_eotf(map->content_peak);
    display = ms_hdr_pq_inverse_eotf(map->display_peak) / content;
    value = fmin(pq / content, 1.0);
    knee = fmax(1.5 * display - 0.5, 0.0);
    if (value <= knee)
        return pq;

    t = (value - knee) / (1.0 - knee);
    return content * ((2.0 * t * t * t - 3.0 * t * t + 1.0) * knee +
                      (t * t * t - 2.0 * t * t + t) * (1.0 - knee) +
                      (-2.0 * t * t * t + 3.0 * t * t) * display);
}

static double
ms_hdr_pq_map_shaper_range(const ms_hdr_pq_map_rec *map)
{
    return map->display_peak ? ms_hdr_pq_inverse_eotf(map->display_peak) : 1.0;
}

double
ms_hdr_pq_map_shaper(const ms_hdr_pq_map_rec *map, double pq)
{
    return fmin(ms_hdr_pq_map_tone(map, pq) / ms_hdr_pq_map_shaper_range(map), 1.0);
}

static void
ms_hdr_gamut_map(const ms_hdr_gamut_rec *gamut, double *color)
{
    double native[3], peak;
    int i;

    ms_hdr_matrix_apply(gamut->to_display, color, native);
    peak = fmax(fmax(native[0], native[1]), native[2]);
    if (peak <= 0.0) {
        color[0] = color[1] = color[2] = 0.0;
        return;
    }

    for (i = 0; i < 3; i++) {
        if (gamut->limit[i] > 0.0)
            native[i] = peak - peak *
                ms_hdr_gamut_compress((peak - native[i]) / peak, gamut->threshold[i],
                                      gamut->limit[i]);
    }

    ms_hdr_matrix_apply(gamut->from_display, native, color);
    for (i = 0; i < 3; i++)
        color[i] = fmax(color[i], 0.0);
}

/* The lookup is indexed by the output of ms_hdr_pq_map_shaper, so it maps the
 * gamut of colours that are already tone mapped.
 */
Bool
ms_hdr_pq_map_lut3d(const ms_hdr_pq_map_rec *map, int size, double *values)
{
    double shaper_range = ms_hdr_pq_map_shaper_range(map);
    ms_hdr_gamut_rec gamut;
    double *nits = calloc(size, sizeof(*nits));
    int r, g, b, i;

    if (!nits)
        return FALSE;

    if (!map->gamut_map || !ms_hdr_gamut_init(map, &gamut))
        ms_hdr_gamut_identity(&gamut);

    for (i = 0; i < size; i++)
        nits[i] = ms_hdr_pq_eotf(shaper_range * i / (size - 1));

    for (b = 0; b < size; b++) {
        for (g = 0; g < size; g++) {
            for (r = 0; r < size; r++) {
                double *out = values + ((b * size + g) * size + r) * 3;

                out[0] = nits[r];
                out[1] = nits[g];
                out[2] = nits[b];
                ms_hdr_gamut_map(&gamut, out);
                for (i = 0; i < 3; i++)
                    out[i] = ms_hdr_pq_inverse_eotf(out[i]);
            }
        }
    }

    free(nits);
    return TRUE;
}
