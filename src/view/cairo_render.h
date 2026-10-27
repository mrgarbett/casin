// SPDX-License-Identifier: GPL-3.0-or-later
// Draws a Scene with cairo + Pango (Linux frontend, and headless export).
#pragma once

#include "scene.h"

#include <cairo.h>
#include <pango/pangocairo.h>

#include <map>
#include <tuple>
#include <string>
#include <vector>

namespace oc::view {

class PangoTextMeasurer : public TextMeasurer {
public:
    PangoTextMeasurer();
    ~PangoTextMeasurer() override;
    PangoTextMeasurer(const PangoTextMeasurer &) = delete;
    PangoTextMeasurer &operator=(const PangoTextMeasurer &) = delete;

    double width(const std::string &text, const Font &font) override;
    double ascent(const Font &font) override;
    double descent(const Font &font) override;

private:
    PangoLayout *layoutFor(const Font &font);
    cairo_surface_t *m_surface;
    cairo_t *m_cr;
    std::map<std::tuple<double, bool, bool>, PangoLayout *> m_layouts;
};

PangoFontDescription *fontDescription(const Font &font);
void drawScene(cairo_t *cr, const Scene &scene);

// Writes the scene to .png, .svg or .pdf (chosen by extension). PNG is
// rendered at `scale` (2 = HiDPI).
bool exportScene(const Scene &scene, const std::string &path, std::string *error = nullptr, double scale = 2.0);

// Writes printed pages to a PDF of the given paper size (points), each page
// scaled to fit inside the margin.
bool exportPdfPages(const std::vector<Scene> &pages, const std::string &path, double paperW = 595, double paperH = 842,
                    double margin = 36, std::string *error = nullptr);

} // namespace oc::view
