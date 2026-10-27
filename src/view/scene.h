// SPDX-License-Identifier: GPL-3.0-or-later
// A tiny retained drawing model. Layout code (maps, sequence view) produces a
// Scene; each frontend (cairo on Linux, CoreGraphics on macOS) draws it.
#pragma once

#include <string>
#include <variant>
#include <vector>

namespace oc::view {

struct Color {
    double r = 0, g = 0, b = 0, a = 1;
    bool isNone() const { return a <= 0; }
    Color withAlpha(double alpha) const { return {r, g, b, alpha}; }
    static Color none() { return {0, 0, 0, 0}; }
    static Color rgb(int r, int g, int b, double a = 1.0) { return {r / 255.0, g / 255.0, b / 255.0, a}; }
    // "#rrggbb" or a handful of CSS names; falls back to `fallback`.
    static Color parse(const std::string &s, Color fallback);
    double luminance() const;   // relative luminance, 0..1
    Color mix(const Color &o, double t) const;
};

struct Point {
    double x = 0, y = 0;
};

struct Rect {
    double x = 0, y = 0, w = 0, h = 0;
    bool contains(Point p) const { return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h; }
    bool intersects(const Rect &o) const { return x < o.x + o.w && o.x < x + w && y < o.y + o.h && o.y < y + h; }
    Rect adjusted(double d) const { return {x - d, y - d, w + 2 * d, h + 2 * d}; }
};

struct PathItem {
    std::vector<Point> points;
    bool closed = false;
    Color fill = Color::none();
    Color stroke = Color::none();
    double lineWidth = 1.0;
    bool dashed = false;
};

enum class HAlign { Left, Center, Right };
enum class VAlign { Baseline, Middle, Top };

struct Font {
    double size = 11;
    bool bold = false;
    bool mono = false;
};

struct TextItem {
    std::string text;
    Point pos;
    Font font;
    Color color;
    HAlign halign = HAlign::Left;
    VAlign valign = VAlign::Baseline;
    double angle = 0;   // radians, clockwise, rotated around pos
};

using Item = std::variant<PathItem, TextItem>;

struct Scene {
    double width = 0, height = 0;
    Color background = Color::none();
    std::vector<Item> items;

    void path(PathItem p) { items.emplace_back(std::move(p)); }
    void text(TextItem t) { items.emplace_back(std::move(t)); }
    void line(Point a, Point b, Color c, double width = 1.0, bool dashed = false);
    void rect(const Rect &r, Color fill, Color stroke = Color::none(), double width = 1.0);
    void polygon(std::vector<Point> pts, Color fill, Color stroke = Color::none(), double width = 1.0);
    void label(const std::string &s, Point p, Font f, Color c, HAlign h = HAlign::Left,
               VAlign v = VAlign::Baseline, double angle = 0);
};

// Frontends supply text metrics so layout can avoid overlaps.
class TextMeasurer {
public:
    virtual ~TextMeasurer() = default;
    virtual double width(const std::string &text, const Font &font) = 0;
    virtual double ascent(const Font &font) { return font.size * 0.78; }
    virtual double descent(const Font &font) { return font.size * 0.22; }
    double height(const Font &font) { return ascent(font) + descent(font); }
};

// Rough metrics for tests and environments without a font backend.
class ApproxTextMeasurer : public TextMeasurer {
public:
    double width(const std::string &text, const Font &font) override;
};

bool pointInPolygon(const std::vector<Point> &poly, Point p);

} // namespace oc::view
