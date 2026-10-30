// SPDX-License-Identifier: GPL-3.0-or-later
#include "gelview.h"

#include "view/maplayout.h"

#include <algorithm>
#include <cmath>

namespace oc::gtk {

namespace {

const double kRunSeconds = 0.9;
const double kPi = 3.14159265358979323846;

struct Rgb {
    double r, g, b;
};

// The slab's colours and the bands' core and glow under each light.
struct StainColors {
    Rgb gelTop, gelBottom, core, glow;
};

StainColors colors(GelStain s)
{
    if (s == GelStain::UV)   // ethidium bromide under UV: pink-white bands on violet-black
        return {{0.17, 0.12, 0.21}, {0.10, 0.07, 0.13}, {1.0, 0.94, 0.97}, {1.0, 0.36, 0.64}};
    // SYBR Safe under blue light: green-white bands on teal-black
    return {{0.07, 0.13, 0.13}, {0.04, 0.08, 0.08}, {0.93, 1.0, 0.86}, {0.36, 1.0, 0.55}};
}

void roundedRect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    r = std::min({r, w / 2, h / 2});
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -kPi / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, kPi / 2);
    cairo_arc(cr, x + r, y + h - r, r, kPi / 2, kPi);
    cairo_arc(cr, x + r, y + r, r, kPi, 3 * kPi / 2);
    cairo_close_path(cr);
}

enum class Align { Left, Center, Right };

// Text in the widget's font, `size` points, centred on y.
void text(cairo_t *cr, GtkWidget *widget, const std::string &s, double size, int weight, double r, double g, double b,
          double a, double x, double y, Align align, bool tabular = false)
{
    PangoLayout *layout = gtk_widget_create_pango_layout(widget, s.c_str());
    PangoFontDescription *font = pango_font_description_copy(pango_context_get_font_description(
        pango_layout_get_context(layout)));
    pango_font_description_set_size(font, int(size * PANGO_SCALE));
    pango_font_description_set_weight(font, PangoWeight(weight));
    pango_layout_set_font_description(layout, font);
    if (tabular) {
        PangoAttrList *attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs, pango_attr_font_features_new("tnum"));
        pango_layout_set_attributes(layout, attrs);
        pango_attr_list_unref(attrs);
    }
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(layout, &tw, &th);
    const double tx = align == Align::Left ? x : align == Align::Center ? x - tw / 2.0 : x - tw;
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_move_to(cr, tx, y - th / 2.0);
    pango_cairo_show_layout(cr, layout);
    pango_font_description_free(font);
    g_object_unref(layout);
}

double textWidth(GtkWidget *widget, const std::string &s, double size, int weight)
{
    PangoLayout *layout = gtk_widget_create_pango_layout(widget, s.c_str());
    PangoFontDescription *font = pango_font_description_copy(pango_context_get_font_description(
        pango_layout_get_context(layout)));
    pango_font_description_set_size(font, int(size * PANGO_SCALE));
    pango_font_description_set_weight(font, PangoWeight(weight));
    pango_layout_set_font_description(layout, font);
    int tw = 0;
    pango_layout_get_pixel_size(layout, &tw, nullptr);
    pango_font_description_free(font);
    g_object_unref(layout);
    return tw;
}

// "500", "1.5 kb", "10 kb".
std::string sizeText(int bp)
{
    if (bp >= 1000 && bp % 1000 == 0)
        return std::to_string(bp / 1000) + " kb";
    if (bp >= 1000 && bp % 100 == 0)
        return std::to_string(bp / 1000) + "." + std::to_string(bp / 100 % 10) + " kb";
    return std::to_string(bp);
}

double bandHeight(const GelBandModel &b) { return 3.5 + 3 * b.intensity; }

// Agarose isn't glass: a faint, fixed grain over the slab.
const std::vector<std::pair<double, double>> &grain()
{
    static const std::vector<std::pair<double, double>> points = [] {
        std::vector<std::pair<double, double>> out;
        guint32 seed = 12345;
        auto next = [&] {
            seed = seed * 1664525u + 1013904223u;
            return double(seed >> 8) / double(1 << 24);
        };
        for (int i = 0; i < 420; ++i) {
            const double x = next();
            out.push_back({x, next()});
        }
        return out;
    }();
    return points;
}

bool animationsEnabled()
{
    gboolean on = TRUE;
    g_object_get(gtk_settings_get_default(), "gtk-enable-animations", &on, nullptr);
    return on;
}

} // namespace

GelView::GelView()
{
    m_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(m_area, TRUE);
    gtk_widget_set_vexpand(m_area, TRUE);
    gtk_widget_set_size_request(m_area, 380, 360);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(m_area), &GelView::draw, this, nullptr);
    GtkEventController *move = gtk_event_controller_motion_new();
    g_signal_connect(move, "motion", G_CALLBACK(&GelView::motion), this);
    g_signal_connect(move, "leave", G_CALLBACK(&GelView::leave), this);
    gtk_widget_add_controller(m_area, move);
    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(&GelView::pressed), this);
    gtk_widget_add_controller(m_area, GTK_EVENT_CONTROLLER(click));
}

GelView::~GelView()
{
    if (m_tick)
        gtk_widget_remove_tick_callback(m_area, m_tick);
}

void GelView::setLanes(std::vector<GelLaneModel> lanes, const std::set<int> &running)
{
    const std::vector<double> old = m_progress;
    m_lanes = std::move(lanes);
    m_progress.assign(m_lanes.size(), 1);
    for (size_t i = 0; i < m_lanes.size(); ++i)
        m_progress[i] = running.count(int(i)) ? 0 : (i < old.size() ? old[i] : 1);
    if (!animationsEnabled())
        std::fill(m_progress.begin(), m_progress.end(), 1);
    if (std::any_of(m_progress.begin(), m_progress.end(), [](double p) { return p < 1; }))
        startRunning();
    if (m_hover.lane >= int(m_lanes.size()))
        m_hover = {};
    gtk_widget_queue_draw(m_area);
}

void GelView::setStain(GelStain stain)
{
    m_stain = stain;
    gtk_widget_queue_draw(m_area);
}

void GelView::setSelectedLane(int lane)
{
    m_selected = lane;
    gtk_widget_queue_draw(m_area);
}

void GelView::setBottomInset(int inset)
{
    m_bottomInset = inset;
    gtk_widget_queue_draw(m_area);
}

void GelView::startRunning()
{
    if (m_tick)
        return;
    m_lastFrame = 0;
    m_tick = gtk_widget_add_tick_callback(m_area, &GelView::tick, this, nullptr);
}

gboolean GelView::tick(GtkWidget *, GdkFrameClock *clock, gpointer p)
{
    auto *self = static_cast<GelView *>(p);
    const gint64 now = gdk_frame_clock_get_frame_time(clock);
    const double dt = self->m_lastFrame ? std::min(0.05, (now - self->m_lastFrame) / 1e6) : 0;
    self->m_lastFrame = now;
    bool running = false;
    for (double &t : self->m_progress) {
        t = std::min(1.0, t + dt / kRunSeconds);
        running = running || t < 1;
    }
    gtk_widget_queue_draw(self->m_area);
    if (running)
        return G_SOURCE_CONTINUE;
    self->m_tick = 0;
    return G_SOURCE_REMOVE;
}

double GelView::eased(int lane) const
{
    const double t = lane >= 0 && lane < int(m_progress.size()) ? m_progress[size_t(lane)] : 1;
    return 1 - std::pow(1 - t, 3);
}

GelView::Geometry GelView::geometry(double w, double h) const
{
    const double ax = 24, ay = 36, aw = std::max(100.0, w - 48), ah = std::max(120.0, h - 36 - m_bottomInset);
    const double labelW = 60, gap = 14;
    const double n = std::max<size_t>(1, m_lanes.size());
    const double laneW = std::min(64.0, std::max(26.0, (aw - labelW - gap * (n + 1)) / n));
    const double slabW = n * laneW + (n + 1) * gap;
    const double slabX = ax + labelW + std::max(0.0, (aw - labelW - slabW) / 2);
    const double slabY = ay + 24, slabH = ah - 24;
    return {slabX, slabY, slabW, slabH, laneW, gap, slabY + 34, slabY + slabH - 22};
}

void GelView::draw(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer p)
{
    static_cast<GelView *>(p)->render(cr, w, h, true);
}

void GelView::render(cairo_t *cr, double w, double h, bool withHover) const
{
    const StainColors c = colors(m_stain);
    // The imager's dark box, a little lighter in the middle.
    cairo_pattern_t *backdrop = cairo_pattern_create_radial(w / 2, h * 0.45, 0, w / 2, h * 0.45, std::max(w, h) * 0.7);
    cairo_pattern_add_color_stop_rgb(backdrop, 0, 0.12, 0.12, 0.12);
    cairo_pattern_add_color_stop_rgb(backdrop, 1, 0.04, 0.04, 0.04);
    cairo_set_source(cr, backdrop);
    cairo_paint(cr);
    cairo_pattern_destroy(backdrop);

    const Geometry g = geometry(w, h);
    // A soft shadow under the slab: rings fading out, a little lower.
    for (int i = 12; i >= 1; --i) {
        roundedRect(cr, g.slabX - i, g.slabY - i + 6, g.slabW + 2 * i, g.slabH + 2 * i, 12 + i);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.05);
        cairo_fill(cr);
    }
    // The slab, with a lit top edge and a faint grain.
    roundedRect(cr, g.slabX, g.slabY, g.slabW, g.slabH, 12);
    cairo_pattern_t *slab = cairo_pattern_create_linear(0, g.slabY, 0, g.slabY + g.slabH);
    cairo_pattern_add_color_stop_rgb(slab, 0, c.gelTop.r, c.gelTop.g, c.gelTop.b);
    cairo_pattern_add_color_stop_rgb(slab, 1, c.gelBottom.r, c.gelBottom.g, c.gelBottom.b);
    cairo_set_source(cr, slab);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(slab);
    cairo_save(cr);
    cairo_clip(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.035);
    for (const auto &[gx, gy] : grain())
        cairo_rectangle(cr, std::floor(g.slabX + gx * g.slabW), std::floor(g.slabY + gy * g.slabH), 1, 1);
    cairo_fill(cr);
    cairo_restore(cr);
    roundedRect(cr, g.slabX + 0.5, g.slabY + 0.5, g.slabW - 1, g.slabH - 1, 12);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    const Hit hover = withHover ? m_hover : Hit{};
    for (size_t i = 0; i < m_lanes.size(); ++i) {
        const GelLaneModel &lane = m_lanes[i];
        const double x = g.laneX(int(i));
        // The lane being edited, and the one under the pointer.
        const double lit = int(i) == m_selected ? 0.07 : (hover.lane == int(i) ? 0.035 : 0);
        if (lit > 0) {
            roundedRect(cr, x - 4, g.slabY + 6, g.laneW + 8, g.slabH - 12, 7);
            cairo_set_source_rgba(cr, 1, 1, 1, lit);
            cairo_fill(cr);
        }
        // Its name over the slab.
        text(cr, m_area, lane.name, 9, PANGO_WEIGHT_SEMIBOLD, 1, 1, 1, int(i) == m_selected ? 1 : 0.6,
             x + g.laneW / 2, g.slabY - 13, Align::Center);
        // The well: a slot cut into the gel.
        roundedRect(cr, x + 2, g.slabY + 14, g.laneW - 4, 8, 2);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
        cairo_fill(cr);
        cairo_rectangle(cr, x + 4, g.slabY + 21, g.laneW - 8, 1);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
        cairo_fill(cr);

        const double e = eased(int(i));
        const double fadeIn = std::min(1.0, (i < m_progress.size() ? m_progress[i] : 1) * 4);
        for (size_t b = 0; b < lane.bands.size(); ++b) {
            const GelBandModel &band = lane.bands[b];
            const double bh = bandHeight(band);
            drawBand(cr, x + 3, g.y(band.run, e) - bh / 2, g.laneW - 6, bh, band.intensity * fadeIn,
                     hover.lane == int(i) && hover.band == int(b));
        }
    }

    // The ladder's sizes on the left, the reference bands bolder.
    if (!m_lanes.empty() && m_lanes.front().ladder) {
        const double e = eased(0);
        std::vector<GelBandModel> bands = m_lanes.front().bands;
        std::sort(bands.begin(), bands.end(), [](const GelBandModel &a, const GelBandModel &b) { return a.run < b.run; });
        double lastY = -1e9;
        for (const GelBandModel &band : bands) {
            const double y = g.y(band.run, e);
            if (y - lastY < 11)
                continue;
            lastY = y;
            text(cr, m_area, sizeText(band.bp), 7.5, band.reference ? PANGO_WEIGHT_SEMIBOLD : PANGO_WEIGHT_NORMAL, 1,
                 1, 1, (band.reference ? 0.8 : 0.5) * e, g.slabX - 10, y, Align::Right, true);
        }
    }
    if (m_lanes.size() <= 1)
        text(cr, m_area, "Add a lane to run a sample", 10, PANGO_WEIGHT_NORMAL, 1, 1, 1, 0.5, g.slabX + g.slabW / 2,
             g.slabY + g.slabH / 2, Align::Center);
    if (withHover && hover.lane >= 0 && hover.band >= 0)
        drawCallout(cr, g, w);
}

// A band: brightest along its middle, fading at the lane's edges, with a
// glow in the stain's colour that grows with the amount of DNA.
void GelView::drawBand(cairo_t *cr, double x, double y, double w, double h, double intensity, bool hovered) const
{
    if (intensity <= 0.01)
        return;
    const StainColors c = colors(m_stain);
    const double a = std::min(1.0, intensity * (hovered ? 1.25 : 1));
    // The glow: rings of the stain's colour, fainter outwards.
    const int rings = int(2 + 5 * a);
    for (int i = rings; i >= 1; --i) {
        const double grow = i * 1.6;
        roundedRect(cr, x - grow, y - grow, w + 2 * grow, h + 2 * grow, h / 2 + grow);
        cairo_set_source_rgba(cr, c.glow.r, c.glow.g, c.glow.b, 0.11 * a * (1 - double(i) / (rings + 1)));
        cairo_fill(cr);
    }
    roundedRect(cr, x, y, w, h, h / 2);
    cairo_pattern_t *core = cairo_pattern_create_linear(x, 0, x + w, 0);
    cairo_pattern_add_color_stop_rgba(core, 0, c.core.r, c.core.g, c.core.b, 0.45 * a);
    cairo_pattern_add_color_stop_rgba(core, 0.3, c.core.r, c.core.g, c.core.b, a);
    cairo_pattern_add_color_stop_rgba(core, 0.7, c.core.r, c.core.g, c.core.b, a);
    cairo_pattern_add_color_stop_rgba(core, 1, c.core.r, c.core.g, c.core.b, 0.45 * a);
    cairo_set_source(cr, core);
    cairo_fill(cr);
    cairo_pattern_destroy(core);
    if (hovered) {
        roundedRect(cr, x - 3, y - 3, w + 6, h + 6, h / 2 + 3);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.55);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    }
}

// The band's size and what it is, in a dark bubble beside the lane.
void GelView::drawCallout(cairo_t *cr, const Geometry &g, double width) const
{
    const GelBandModel &b = m_lanes[size_t(m_hover.lane)].bands[size_t(m_hover.band)];
    const std::string title = view::withThousands(b.bp) + " bp";
    const double tw = std::max(textWidth(m_area, title, 9, PANGO_WEIGHT_SEMIBOLD),
                               b.caption.empty() ? 0 : textWidth(m_area, b.caption, 8.5, PANGO_WEIGHT_NORMAL));
    const double w = tw + 20, h = b.caption.empty() ? 26 : 42;
    const double y = g.y(b.run, eased(m_hover.lane));
    double x = g.laneX(m_hover.lane) + g.laneW + 10;
    if (x + w > width - 8)
        x = g.laneX(m_hover.lane) - 10 - w;
    roundedRect(cr, x, y - h / 2, w, h, 9);
    cairo_set_source_rgba(cr, 0.06, 0.06, 0.06, 0.88);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.16);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    text(cr, m_area, title, 9, PANGO_WEIGHT_SEMIBOLD, 1, 1, 1, 1, x + 10, y - h / 2 + 13, Align::Left, true);
    if (!b.caption.empty())
        text(cr, m_area, b.caption, 8.5, PANGO_WEIGHT_NORMAL, 1, 1, 1, 0.65, x + 10, y - h / 2 + 30, Align::Left);
}

GelView::Hit GelView::hit(double x, double y) const
{
    const Geometry g = geometry(gtk_widget_get_width(m_area), gtk_widget_get_height(m_area));
    if (x < g.slabX || x > g.slabX + g.slabW || y < g.slabY - 24 || y > g.slabY + g.slabH + 24)
        return {};
    Hit out;
    for (size_t i = 0; i < m_lanes.size(); ++i)
        if (std::abs(x - (g.laneX(int(i)) + g.laneW / 2)) <= (g.laneW + g.gap) / 2)
            out.lane = int(i);
    if (out.lane < 0)
        return {};
    double best = 1e9;
    const auto &bands = m_lanes[size_t(out.lane)].bands;
    for (size_t b = 0; b < bands.size(); ++b) {
        const double d = std::abs(y - g.y(bands[b].run, eased(out.lane)));
        if (d <= std::max(5.0, bandHeight(bands[b]) / 2 + 3) && d < best) {
            best = d;
            out.band = int(b);
        }
    }
    return out;
}

void GelView::motion(GtkEventControllerMotion *, double x, double y, gpointer p)
{
    auto *self = static_cast<GelView *>(p);
    const Hit h = self->hit(x, y);
    if (h.lane != self->m_hover.lane || h.band != self->m_hover.band) {
        self->m_hover = h;
        gtk_widget_queue_draw(self->m_area);
    }
    gtk_widget_set_cursor_from_name(self->m_area, h.lane >= 0 ? "pointer" : nullptr);
}

void GelView::leave(GtkEventControllerMotion *, gpointer p)
{
    auto *self = static_cast<GelView *>(p);
    self->m_hover = {};
    gtk_widget_queue_draw(self->m_area);
}

void GelView::pressed(GtkGestureClick *, int, double x, double y, gpointer p)
{
    auto *self = static_cast<GelView *>(p);
    const Hit h = self->hit(x, y);
    if (h.lane < 0)
        return;
    if (self->onSelectLane)
        self->onSelectLane(h.lane);
    if (h.band >= 0 && self->onPickBand)
        self->onPickBand(h.lane, h.band);
}

GBytes *GelView::snapshotPng()
{
    std::fill(m_progress.begin(), m_progress.end(), 1);
    // Without the floating controls' room: the picture is the gel alone.
    const int inset = m_bottomInset;
    m_bottomInset = 16;
    const int w = std::max(1, gtk_widget_get_width(m_area));
    const int h = std::max(200, gtk_widget_get_height(m_area) - inset + 16);
    const int scale = std::max(1, gtk_widget_get_scale_factor(m_area));
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w * scale, h * scale);
    cairo_t *cr = cairo_create(surface);
    cairo_scale(cr, scale, scale);
    render(cr, w, h, false);
    m_bottomInset = inset;
    cairo_destroy(cr);
    GByteArray *png = g_byte_array_new();
    cairo_surface_write_to_png_stream(
        surface,
        [](void *out, const unsigned char *data, unsigned int length) {
            g_byte_array_append(static_cast<GByteArray *>(out), data, length);
            return CAIRO_STATUS_SUCCESS;
        },
        png);
    cairo_surface_destroy(surface);
    gtk_widget_queue_draw(m_area);
    return g_byte_array_free_to_bytes(png);
}

} // namespace oc::gtk
