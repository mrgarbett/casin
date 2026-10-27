// SPDX-License-Identifier: GPL-3.0-or-later
#include "scene.h"

#include <cmath>
#include <cstdlib>

namespace oc::view {

Color Color::parse(const std::string &s, Color fallback)
{
    if (s.size() == 7 && s[0] == '#') {
        char *end = nullptr;
        unsigned long v = std::strtoul(s.c_str() + 1, &end, 16);
        if (end && *end == 0)
            return rgb(int((v >> 16) & 0xff), int((v >> 8) & 0xff), int(v & 0xff));
    }
    if (s.size() == 4 && s[0] == '#') {
        char *end = nullptr;
        unsigned long v = std::strtoul(s.c_str() + 1, &end, 16);
        if (end && *end == 0)
            return rgb(int(((v >> 8) & 0xf) * 17), int(((v >> 4) & 0xf) * 17), int((v & 0xf) * 17));
    }
    struct Named { const char *name; int r, g, b; };
    static const Named names[] = {
        {"black", 0, 0, 0},       {"white", 255, 255, 255}, {"red", 255, 0, 0},
        {"green", 0, 128, 0},     {"blue", 0, 0, 255},      {"yellow", 255, 255, 0},
        {"cyan", 0, 255, 255},    {"magenta", 255, 0, 255}, {"orange", 255, 165, 0},
        {"purple", 128, 0, 128},  {"pink", 255, 192, 203},  {"gray", 128, 128, 128},
        {"grey", 128, 128, 128},  {"brown", 165, 42, 42},   {"lightblue", 173, 216, 230},
        {"lightgreen", 144, 238, 144},
    };
    for (const Named &n : names)
        if (s == n.name)
            return rgb(n.r, n.g, n.b);
    return fallback;
}

double Color::luminance() const
{
    auto lin = [](double c) { return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
}

Color Color::mix(const Color &o, double t) const
{
    return {r + (o.r - r) * t, g + (o.g - g) * t, b + (o.b - b) * t, a + (o.a - a) * t};
}

void Scene::line(Point a, Point b, Color c, double width, bool dashed)
{
    PathItem p;
    p.points = {a, b};
    p.stroke = c;
    p.lineWidth = width;
    p.dashed = dashed;
    path(std::move(p));
}

void Scene::rect(const Rect &r, Color fill, Color stroke, double width)
{
    polygon({{r.x, r.y}, {r.x + r.w, r.y}, {r.x + r.w, r.y + r.h}, {r.x, r.y + r.h}}, fill, stroke, width);
}

void Scene::polygon(std::vector<Point> pts, Color fill, Color stroke, double width)
{
    PathItem p;
    p.points = std::move(pts);
    p.closed = true;
    p.fill = fill;
    p.stroke = stroke;
    p.lineWidth = width;
    path(std::move(p));
}

void Scene::label(const std::string &s, Point p, Font f, Color c, HAlign h, VAlign v, double angle)
{
    TextItem t;
    t.text = s;
    t.pos = p;
    t.font = f;
    t.color = c;
    t.halign = h;
    t.valign = v;
    t.angle = angle;
    text(std::move(t));
}

double ApproxTextMeasurer::width(const std::string &text, const Font &font)
{
    return double(text.size()) * font.size * (font.mono ? 0.6 : (font.bold ? 0.62 : 0.56));
}

bool pointInPolygon(const std::vector<Point> &poly, Point p)
{
    bool inside = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        const Point &a = poly[i], &b = poly[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
            inside = !inside;
    }
    return inside;
}

} // namespace oc::view
