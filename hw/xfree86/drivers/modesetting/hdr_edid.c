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

#include "driver.h"

#define EDID_BLOCK_SIZE 128
#define CTA_EXTENSION_TAG 0x02
#define CTA_DATA_BLOCK_EXTENDED 7
#define CTA_EXTENDED_VENDOR_SPECIFIC_VIDEO 1
#define CTA_EXTENDED_HDR_STATIC_METADATA 6
#define CTA_EOTF_SMPTE_ST2084 (1 << 2)

#define HDMI_STATIC_METADATA_TYPE1 0
#define HDMI_EOTF_SMPTE_ST2084 2

#define METADATA_CHROMATICITY_SCALE 50000.0
#define METADATA_MIN_LUMINANCE_SCALE 10000.0
#define DEFAULT_MAX_LUMINANCE 1000

typedef struct {
    Bool pq;
    double max_luminance;
    double max_fall;
    double min_luminance;
} ms_hdr_edid_luminance_rec;

static const uint8_t dolby_oui[] = { 0x46, 0xd0, 0x00 };

static double
ms_hdr_edid_luminance(uint8_t code)
{
    return 50.0 * pow(2.0, code / 32.0);
}

static uint16_t
ms_hdr_edid_chromaticity(const uint8_t *edid, int high, int low, int shift)
{
    int value = (edid[high] << 2) | ((edid[low] >> shift) & 0x3);

    return lrint(value / 1024.0 * METADATA_CHROMATICITY_SCALE);
}

static void
ms_hdr_edid_parse_primaries(const uint8_t *edid,
                            struct hdr_metadata_infoframe *info)
{
    info->display_primaries[0].x = ms_hdr_edid_chromaticity(edid, 27, 25, 6);
    info->display_primaries[0].y = ms_hdr_edid_chromaticity(edid, 28, 25, 4);
    info->display_primaries[1].x = ms_hdr_edid_chromaticity(edid, 29, 25, 2);
    info->display_primaries[1].y = ms_hdr_edid_chromaticity(edid, 30, 25, 0);
    info->display_primaries[2].x = ms_hdr_edid_chromaticity(edid, 31, 26, 6);
    info->display_primaries[2].y = ms_hdr_edid_chromaticity(edid, 32, 26, 4);
    info->white_point.x = ms_hdr_edid_chromaticity(edid, 33, 26, 2);
    info->white_point.y = ms_hdr_edid_chromaticity(edid, 34, 26, 0);
}

static void
ms_hdr_edid_parse_static(const uint8_t *data, int len,
                         ms_hdr_edid_luminance_rec *luminance)
{
    luminance->pq = (data[1] & CTA_EOTF_SMPTE_ST2084) != 0;
    if (len >= 4 && data[3])
        luminance->max_luminance = ms_hdr_edid_luminance(data[3]);
    if (len >= 5 && data[4])
        luminance->max_fall = ms_hdr_edid_luminance(data[4]);
    if (len >= 6)
        luminance->min_luminance = luminance->max_luminance *
            pow(data[5] / 255.0, 2.0) / 100.0;
}

static void
ms_hdr_edid_parse_dolby(const uint8_t *data, int len,
                        ms_hdr_edid_luminance_rec *luminance)
{
    switch (data[0] >> 5) {
    case 0:
        if (len < 16)
            return;
        luminance->max_luminance =
            ms_hdr_pq_eotf(((data[15] << 4) | (data[13] & 0xf)) / 4095.0);
        luminance->min_luminance =
            ms_hdr_pq_eotf(((data[14] << 4) | (data[13] >> 4)) / 4095.0);
        break;
    case 1:
        if (len < 3)
            return;
        luminance->max_luminance = 100 + (data[1] >> 1) * 50;
        luminance->min_luminance = pow((data[2] >> 1) / 127.0, 2.0);
        break;
    case 2:
        if (len < 3)
            return;
        luminance->max_luminance =
            ms_hdr_pq_eotf((2055 + 65 * (data[2] >> 3)) / 4095.0);
        luminance->min_luminance =
            ms_hdr_pq_eotf(20 * (data[1] >> 3) / 4095.0);
        break;
    }
}

static void
ms_hdr_edid_parse_cta(const uint8_t *ext, ms_hdr_edid_luminance_rec *hdr_static,
                      ms_hdr_edid_luminance_rec *dolby)
{
    int end = ext[2];
    int i, len;

    for (i = 4; i < end; i += len + 1) {
        const uint8_t *data = ext + i + 1;

        len = ext[i] & 0x1f;
        if (i + len >= end)
            break;

        if (ext[i] >> 5 != CTA_DATA_BLOCK_EXTENDED || len < 2)
            continue;

        if (data[0] == CTA_EXTENDED_HDR_STATIC_METADATA)
            ms_hdr_edid_parse_static(data, len, hdr_static);
        else if (data[0] == CTA_EXTENDED_VENDOR_SPECIFIC_VIDEO && len >= 5 &&
                 memcmp(data + 1, dolby_oui, sizeof(dolby_oui)) == 0)
            ms_hdr_edid_parse_dolby(data + 4, len - 4, dolby);
    }
}

Bool
ms_hdr_parse_edid(const uint8_t *edid, size_t size,
                  struct hdr_output_metadata *metadata)
{
    struct hdr_metadata_infoframe *info = &metadata->hdmi_metadata_type1;
    ms_hdr_edid_luminance_rec hdr_static = { 0 }, dolby = { 0 };
    const ms_hdr_edid_luminance_rec *luminance;
    size_t block;

    *metadata = (struct hdr_output_metadata) {
        .metadata_type = HDMI_STATIC_METADATA_TYPE1,
        .hdmi_metadata_type1 = {
            .eotf = HDMI_EOTF_SMPTE_ST2084,
            .metadata_type = HDMI_STATIC_METADATA_TYPE1,
            .max_display_mastering_luminance = DEFAULT_MAX_LUMINANCE,
        },
    };

    if (!edid || size < EDID_BLOCK_SIZE)
        return FALSE;

    ms_hdr_edid_parse_primaries(edid, info);

    for (block = EDID_BLOCK_SIZE; block + EDID_BLOCK_SIZE <= size;
         block += EDID_BLOCK_SIZE) {
        const uint8_t *ext = edid + block;

        if (ext[0] == CTA_EXTENSION_TAG && ext[2] < EDID_BLOCK_SIZE)
            ms_hdr_edid_parse_cta(ext, &hdr_static, &dolby);
    }

    luminance = dolby.max_luminance > 0 ? &dolby : &hdr_static;
    if (luminance->max_luminance > 0) {
        info->max_display_mastering_luminance = lrint(luminance->max_luminance);
        info->max_cll = lrint(luminance->max_luminance);
    }
    info->min_display_mastering_luminance =
        lrint(luminance->min_luminance * METADATA_MIN_LUMINANCE_SCALE);
    info->max_fall = lrint(hdr_static.max_fall);

    return hdr_static.pq;
}
