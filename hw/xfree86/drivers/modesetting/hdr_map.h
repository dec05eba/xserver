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

#ifndef MS_HDR_MAP_H
#define MS_HDR_MAP_H

#include <stdint.h>
#include <X11/Xdefs.h>
#include <drm_mode.h>

/* How PQ content is mapped into the range of a monitor. Luminance is in nits,
 * primaries are red, green and blue in hdr_output_metadata units. Everything
 * is zero when the content fits the monitor.
 */
typedef struct {
    Bool tone_map;
    uint16_t content_peak;
    uint16_t display_peak;

    Bool gamut_map;
    uint16_t content_primaries[3][2];
    uint16_t display_primaries[3][2];
} ms_hdr_pq_map_rec;

double ms_hdr_pq_eotf(double value);
double ms_hdr_pq_inverse_eotf(double nits);
double ms_hdr_tone_map(double value, double peak);
void ms_hdr_matrix_multiply(const double *a, const double *b, double *out);

int ms_hdr_metadata_peak(const struct hdr_output_metadata *metadata);
int ms_hdr_tone_map_peak(const struct hdr_output_metadata *content, int display_peak);
Bool ms_hdr_gamut_valid(const double *primaries);

Bool ms_hdr_pq_map_init(ms_hdr_pq_map_rec *map,
                        const struct hdr_output_metadata *content,
                        const struct hdr_output_metadata *output, Bool tone_map);
double ms_hdr_pq_map_tone(const ms_hdr_pq_map_rec *map, double pq);
double ms_hdr_pq_map_shaper(const ms_hdr_pq_map_rec *map, double pq);
Bool ms_hdr_pq_map_lut3d(const ms_hdr_pq_map_rec *map, int size, double *values);

#endif
