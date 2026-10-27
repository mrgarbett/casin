// SPDX-License-Identifier: GPL-3.0-or-later
#include "cairo_render.h"
#include "../core/sequence.h"

#include <cairo-pdf.h>
#include <cairo-svg.h>

#include <algorithm>

namespace oc::view {

PangoFontDescription *fontDescription(const Font &font)
{
    PangoFontDescription *d = pango_font_description_from_string(
        font.mono ? "Monospace" : "Cantarell, Adwaita Sans, Inter, DejaVu Sans, Sans");
    pango_font_description_set_absolute_size(d, font.size * PANGO_SCALE);
    pango_font_description_set_weight(d, font.bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
    return d;
}

PangoTextMeasurer::PangoTextMeasurer()
{
    m_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
    m_cr = cairo_create(m_surface);
}

PangoTextMeasurer::~PangoTextMeasurer()
{
    for (auto &kv : m_layouts)
        g_object_unref(kv.second);
    cairo_destroy(m_cr);
    cairo_surface_destroy(m_surface);
}

PangoLayout *PangoTextMeasurer::layoutFor(const Font &font)
{
    auto key = std::make_tuple(font.size, font.bold, font.mono);
    auto it = m_layouts.find(key);
    if (it != m_layouts.end())
        return it->second;
    PangoLayout *layout = pango_cairo_create_layout(m_cr);
    PangoFontDescription *d = fontDescription(font);
    pango_layout_set_font_description(layout, d);
    pango_font_description_free(d);
    m_layouts[key] = layout;
    return layout;
}

double PangoTextMeasurer::width(const std::string &text, const Font &font)
{
    PangoLayout *l = layoutFor(font);
    pango_layout_set_text(l, text.c_str(), int(text.size()));
    PangoRectangle logical;
    pango_layout_get_extents(l, nullptr, &logical);
    return double(logical.width) / PANGO_SCALE;
}

double PangoTextMeasurer::ascent(const Font &font)
{
    PangoLayout *l = layoutFor(font);
    pango_layout_set_text(l, "Ag", 2);
    return double(pango_layout_get_baseline(l)) / PANGO_SCALE;
}

double PangoTextMeasurer::descent(const Font &font)
{
    PangoLayout *l = layoutFor(font);
    pango_layout_set_text(l, "Ag", 2);
    PangoRectangle logical;
    pango_layout_get_extents(l, nullptr, &logical);
    return double(logical.height - pango_layout_get_baseline(l)) / PANGO_SCALE;
}

static void setColor(cairo_t *cr, const Color &c)
{
    cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
}

void drawScene(cairo_t *cr, const Scene &scene)
{
    cairo_save(cr);
    if (!scene.background.isNone()) {
        setColor(cr, scene.background);
        cairo_rectangle(cr, 0, 0, scene.width, scene.height);
        cairo_fill(cr);
    }
    cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_BUTT);

    PangoLayout *layout = pango_cairo_create_layout(cr);
    std::tuple<double, bool, bool> currentFont{-1, false, false};

    for (const Item &item : scene.items) {
        if (const PathItem *p = std::get_if<PathItem>(&item)) {
            if (p->points.size() < 2)
                continue;
            cairo_new_path(cr);
            cairo_move_to(cr, p->points[0].x, p->points[0].y);
            for (size_t i = 1; i < p->points.size(); ++i)
                cairo_line_to(cr, p->points[i].x, p->points[i].y);
            if (p->closed)
                cairo_close_path(cr);
            if (!p->fill.isNone()) {
                setColor(cr, p->fill);
                if (!p->stroke.isNone())
                    cairo_fill_preserve(cr);
                else
                    cairo_fill(cr);
            }
            if (!p->stroke.isNone()) {
                setColor(cr, p->stroke);
                cairo_set_line_width(cr, p->lineWidth);
                if (p->dashed) {
                    const double dash[] = {3, 3};
                    cairo_set_dash(cr, dash, 2, 0);
                }
                cairo_stroke(cr);
                if (p->dashed)
                    cairo_set_dash(cr, nullptr, 0, 0);
            }
            cairo_new_path(cr);
        } else if (const TextItem *t = std::get_if<TextItem>(&item)) {
            auto key = std::make_tuple(t->font.size, t->font.bold, t->font.mono);
            if (key != currentFont) {
                PangoFontDescription *d = fontDescription(t->font);
                pango_layout_set_font_description(layout, d);
                pango_font_description_free(d);
                currentFont = key;
            }
            pango_layout_set_text(layout, t->text.c_str(), int(t->text.size()));
            PangoRectangle logical;
            pango_layout_get_extents(layout, nullptr, &logical);
            const double w = double(logical.width) / PANGO_SCALE;
            const double h = double(logical.height) / PANGO_SCALE;
            const double baseline = double(pango_layout_get_baseline(layout)) / PANGO_SCALE;
            double dx = 0, dy = 0;
            if (t->halign == HAlign::Center)
                dx = -w / 2;
            else if (t->halign == HAlign::Right)
                dx = -w;
            if (t->valign == VAlign::Baseline)
                dy = -baseline;
            else if (t->valign == VAlign::Middle)
                dy = -h / 2;
            cairo_save(cr);
            cairo_translate(cr, t->pos.x, t->pos.y);
            if (t->angle != 0)
                cairo_rotate(cr, t->angle);
            cairo_move_to(cr, dx, dy);
            setColor(cr, t->color);
            pango_cairo_show_layout(cr, layout);
            cairo_restore(cr);
        }
    }
    g_object_unref(layout);
    cairo_restore(cr);
}

bool exportScene(const Scene &scene, const std::string &path, std::string *error, double scale)
{
    const std::string ext = fileExtension(path);
    cairo_surface_t *surface = nullptr;
    if (ext == "png") {
        surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, int(scene.width * scale), int(scene.height * scale));
    } else if (ext == "svg") {
        surface = cairo_svg_surface_create(path.c_str(), scene.width, scene.height);
    } else if (ext == "pdf") {
        surface = cairo_pdf_surface_create(path.c_str(), scene.width, scene.height);
    } else {
        if (error)
            *error = "unsupported export format: ." + ext + " (use .png, .svg or .pdf)";
        return false;
    }
    cairo_t *cr = cairo_create(surface);
    if (ext == "png")
        cairo_scale(cr, scale, scale);
    drawScene(cr, scene);
    cairo_destroy(cr);
    cairo_status_t status = CAIRO_STATUS_SUCCESS;
    if (ext == "png")
        status = cairo_surface_write_to_png(surface, path.c_str());
    cairo_surface_finish(surface);
    if (status == CAIRO_STATUS_SUCCESS)
        status = cairo_surface_status(surface);
    cairo_surface_destroy(surface);
    if (status != CAIRO_STATUS_SUCCESS) {
        if (error)
            *error = std::string("export failed: ") + cairo_status_to_string(status);
        return false;
    }
    return true;
}

bool exportPdfPages(const std::vector<Scene> &pages, const std::string &path, double paperW, double paperH,
                    double margin, std::string *error)
{
    cairo_surface_t *surface = cairo_pdf_surface_create(path.c_str(), paperW, paperH);
    cairo_t *cr = cairo_create(surface);
    for (const Scene &page : pages) {
        cairo_save(cr);
        cairo_translate(cr, margin, margin);
        const double sc = std::min((paperW - 2 * margin) / page.width, (paperH - 2 * margin) / page.height);
        cairo_scale(cr, sc, sc);
        drawScene(cr, page);
        cairo_restore(cr);
        cairo_show_page(cr);
    }
    cairo_destroy(cr);
    cairo_surface_finish(surface);
    const cairo_status_t status = cairo_surface_status(surface);
    cairo_surface_destroy(surface);
    if (status != CAIRO_STATUS_SUCCESS) {
        if (error)
            *error = std::string("export failed: ") + cairo_status_to_string(status);
        return false;
    }
    return true;
}

} // namespace oc::view
