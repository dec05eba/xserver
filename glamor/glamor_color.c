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

#include "glamor_priv.h"
#include "glamor_transform.h"

struct color_args {
    glamor_pixmap_fbo *src;
    int dx, dy;
    Bool src_alpha;
    float opacity;
    const glamor_color_conversion *conversion;
};

static Bool
use_color(DrawablePtr drawable, GCPtr gc, glamor_program *prog, void *arg)
{
    struct color_args *args = arg;
    glamor_screen_private *glamor_priv =
        glamor_get_screen_private(drawable->pScreen);
    glamor_color_program *color = &glamor_priv->color_prog;
    const glamor_color_conversion *conversion = args->conversion;
    GLfloat matrix[9];
    int row, col;

    glamor_bind_texture(glamor_priv, GL_TEXTURE0, args->src, FALSE);

    glUniform2f(prog->fill_offset_uniform, args->dx, args->dy);
    glUniform2f(prog->fill_size_inv_uniform, 1.0f / args->src->width,
                1.0f / args->src->height);

    for (row = 0; row < 3; row++) {
        for (col = 0; col < 3; col++)
            matrix[col * 3 + row] = conversion->matrix[row * 3 + col];
    }

    glUniform1f(color->src_pq_uniform,
                conversion->src == GLAMOR_COLOR_PQ ? 1.0f : 0.0f);
    glUniform1f(color->dst_pq_uniform,
                conversion->dst == GLAMOR_COLOR_PQ ? 1.0f : 0.0f);
    glUniform1f(color->src_alpha_uniform, args->src_alpha ? 1.0f : 0.0f);
    glUniformMatrix3fv(color->matrix_uniform, 1, GL_FALSE, matrix);
    glUniform1f(color->sdr_white_uniform, conversion->sdr_white);
    glUniform1f(color->peak_uniform, conversion->peak);
    glUniform1f(color->opacity_uniform, args->opacity);

    return TRUE;
}

static const glamor_facet glamor_facet_color = {
    .name = "color",
    .vs_vars = "in vec2 primitive;\n",
    .vs_exec = (GLAMOR_POS(gl_Position, primitive.xy)
                "       fill_pos = (fill_offset + primitive.xy) * fill_size_inv;\n"),
    .fs_vars = ("#if defined(GL_ES) && defined(GL_FRAGMENT_PRECISION_HIGH)\n"
                "precision highp float;\n"
                "#endif\n"
                "uniform float src_pq;\n"
                "uniform float dst_pq;\n"
                "uniform float src_alpha;\n"
                "uniform mat3 conversion;\n"
                "uniform float sdr_white;\n"
                "uniform float peak;\n"
                "uniform float opacity;\n"
                "vec3 pq_to_nits(vec3 e) {\n"
                "       vec3 p = pow(clamp(e, 0.0, 1.0), vec3(1.0 / 78.84375));\n"
                "       return 10000.0 * pow(max(p - 0.8359375, 0.0) / (18.8515625 - 18.6875 * p),\n"
                "                            vec3(1.0 / 0.1593017578125));\n"
                "}\n"
                "vec3 nits_to_pq(vec3 l) {\n"
                "       vec3 y = pow(clamp(l / 10000.0, 0.0, 1.0), vec3(0.1593017578125));\n"
                "       return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), vec3(78.84375));\n"
                "}\n"
                "vec3 tone_map(vec3 c) {\n"
                "       float knee = 0.75;\n"
                "       float m = max(max(c.r, c.g), c.b);\n"
                "       float p = peak / sdr_white;\n"
                "       if (m <= knee || p <= 1.0)\n"
                "               return c;\n"
                "       float t = (m - knee) / (1.0 - knee);\n"
                "       float tp = (p - knee) / (1.0 - knee);\n"
                "       float mapped = knee + (1.0 - knee) * t * (1.0 + t / (tp * tp)) / (1.0 + t);\n"
                "       return c * (mapped / m);\n"
                "}\n"),
    .fs_exec = ("       vec4 s = texture(sampler, fill_pos);\n"
                "       float a = src_alpha > 0.5 ? s.a : 1.0;\n"
                "       vec3 c = s.rgb / max(a, 0.000001);\n"
                "       vec3 l = src_pq > 0.5 ? pq_to_nits(c) : pow(c, vec3(2.2)) * sdr_white;\n"
                "       l = max(conversion * l, 0.0);\n"
                "       vec3 e = dst_pq > 0.5 ? nits_to_pq(l) :\n"
                "                pow(min(tone_map(l / sdr_white), 1.0), vec3(1.0 / 2.2));\n"
                "       a *= opacity;\n"
                "       frag_color = vec4(e * a, a);\n"),
    .locations = glamor_program_location_fillsamp | glamor_program_location_fillpos,
    .use = use_color,
};

static Bool
glamor_build_color_program(ScreenPtr screen, glamor_color_program *color)
{
    if (!glamor_build_program(screen, &color->prog, &glamor_facet_color,
                              NULL, NULL, NULL))
        return FALSE;

    color->src_pq_uniform = glGetUniformLocation(color->prog.prog, "src_pq");
    color->dst_pq_uniform = glGetUniformLocation(color->prog.prog, "dst_pq");
    color->src_alpha_uniform = glGetUniformLocation(color->prog.prog,
                                                    "src_alpha");
    color->matrix_uniform = glGetUniformLocation(color->prog.prog,
                                                 "conversion");
    color->sdr_white_uniform = glGetUniformLocation(color->prog.prog,
                                                    "sdr_white");
    color->peak_uniform = glGetUniformLocation(color->prog.prog, "peak");
    color->opacity_uniform = glGetUniformLocation(color->prog.prog, "opacity");
    return TRUE;
}

Bool
glamor_convert_color(PixmapPtr src, PixmapPtr dst, RegionPtr region,
                     int dx, int dy, Bool src_alpha, CARD16 opacity,
                     const glamor_color_conversion *conversion)
{
    ScreenPtr screen = dst->drawable.pScreen;
    glamor_screen_private *glamor_priv = glamor_get_screen_private(screen);
    glamor_pixmap_private *src_priv = glamor_get_pixmap_private(src);
    glamor_pixmap_private *dst_priv = glamor_get_pixmap_private(dst);
    glamor_color_program *color = &glamor_priv->color_prog;
    Bool blend = src_alpha || opacity != 0xffff;
    BoxPtr box = RegionRects(region);
    int nbox = RegionNumRects(region);
    struct color_args args = {
        .src_alpha = src_alpha,
        .opacity = opacity / 65535.0f,
        .conversion = conversion,
    };
    int src_box_index, dst_box_index;
    int dst_off_x, dst_off_y;
    char *vbo_offset;
    GLshort *v;
    Bool ret = FALSE;
    int n;

    if (!nbox)
        return TRUE;

    if (!GLAMOR_PIXMAP_PRIV_HAS_FBO(src_priv) ||
        !GLAMOR_PIXMAP_PRIV_HAS_FBO(dst_priv))
        return FALSE;

    glamor_external_update(src);
    glamor_make_current(glamor_priv);

    if (color->prog.failed)
        return FALSE;

    if (!color->prog.prog && !glamor_build_color_program(screen, color))
        return FALSE;

    if (!glamor_set_alu(&dst->drawable, GXcopy))
        return FALSE;

    v = glamor_get_vbo_space(screen, nbox * 8 * sizeof (int16_t), &vbo_offset);

    glEnableVertexAttribArray(GLAMOR_VERTEX_POS);
    glVertexAttribPointer(GLAMOR_VERTEX_POS, 2, GL_SHORT, GL_FALSE,
                          2 * sizeof (GLshort), vbo_offset);

    for (n = 0; n < nbox; n++) {
        v[0] = box->x1; v[1] = box->y1;
        v[2] = box->x1; v[3] = box->y2;
        v[4] = box->x2; v[5] = box->y2;
        v[6] = box->x2; v[7] = box->y1;

        v += 8;
        box++;
    }

    glamor_put_vbo_space(screen);

    if (blend) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    }
    glEnable(GL_SCISSOR_TEST);

    glamor_pixmap_loop(src_priv, src_box_index) {
        BoxPtr src_box = glamor_pixmap_box_at(src_priv, src_box_index);

        args.dx = dx - src_box->x1;
        args.dy = dy - src_box->y1;
        args.src = glamor_pixmap_fbo_at(src_priv, src_box_index);

        if (!glamor_use_program(&dst->drawable, NULL, &color->prog, &args))
            goto bail;

        glamor_pixmap_loop(dst_priv, dst_box_index) {
            if (!glamor_set_destination_drawable(&dst->drawable, dst_box_index,
                                                 FALSE, FALSE,
                                                 color->prog.matrix_uniform,
                                                 &dst_off_x, &dst_off_y))
                goto bail;

            glScissor(dst_off_x - args.dx, dst_off_y - args.dy,
                      src_box->x2 - src_box->x1, src_box->y2 - src_box->y1);

            glamor_glDrawArrays_GL_QUADS(glamor_priv, nbox);
        }
    }

    ret = TRUE;

bail:
    glDisable(GL_SCISSOR_TEST);
    if (blend)
        glDisable(GL_BLEND);
    glDisableVertexAttribArray(GLAMOR_VERTEX_POS);

    return ret;
}
