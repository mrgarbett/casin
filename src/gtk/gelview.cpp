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

// The light box behind the gel (at its top and bottom), a band's middle and
// the glow around it, under each stain and light.
struct StainColors {
    Rgb gelTop, gelBottom, core, glow;
    bool tubes;     // the light box's tubes show through the gel
    double grain;   // a camera's grain: stronger on a black-and-white print
};

StainColors colors(GelStain s)
{
    switch (s) {
    case GelStain::SybrSafe:
        return {{0.06, 0.12, 0.16}, {0.03, 0.07, 0.09}, {0.90, 1.0, 0.80}, {0.35, 1.0, 0.45}, true, 0.035};
    case GelStain::BlackAndWhite:
        return {{0.09, 0.09, 0.09}, {0.04, 0.04, 0.04}, {1, 1, 1}, {0.85, 0.85, 0.85}, false, 0.07};
    case GelStain::EthidiumUV:
        break;
    }
    return {{0.20, 0.10, 0.22}, {0.11, 0.05, 0.13}, {1.0, 0.90, 0.82}, {1.0, 0.42, 0.22}, true, 0.035};
}

void roundedRect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    r = std::max(0.0, std::min({r, w / 2, h / 2}));
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -kPi / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, kPi / 2);
    cairo_arc(cr, x + r, y + h - r, r, kPi / 2, kPi);
    cairo_arc(cr, x + r, y + r, r, kPi, 3 * kPi / 2);
    cairo_close_path(cr);
}

// A quadratic curve from the current point, as cairo draws cubics.
void quadTo(cairo_t *cr, double cx, double cy, double x, double y)
{
    double x0, y0;
    cairo_get_current_point(cr, &x0, &y0);
    cairo_curve_to(cr, x0 + 2.0 / 3 * (cx - x0), y0 + 2.0 / 3 * (cy - y0), x + 2.0 / 3 * (cx - x), y + 2.0 / 3 * (cy - y),
                   x, y);
}

enum class Align { Left, Center, Right };

PangoLayout *layoutFor(GtkWidget *widget, const std::string &s, double px, int weight, bool tabular)
{
    PangoLayout *layout = gtk_widget_create_pango_layout(widget, s.c_str());
    PangoFontDescription *font = pango_font_description_copy(pango_context_get_font_description(
        pango_layout_get_context(layout)));
    pango_font_description_set_absolute_size(font, px * PANGO_SCALE);
    pango_font_description_set_weight(font, PangoWeight(weight));
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    if (tabular) {
        PangoAttrList *attrs = pango_attr_list_new();
        pango_attr_list_insert(attrs, pango_attr_font_features_new("tnum"));
        pango_layout_set_attributes(layout, attrs);
        pango_attr_list_unref(attrs);
    }
    return layout;
}

// Text in the widget's font, `px` pixels, centred on y.
void text(cairo_t *cr, GtkWidget *widget, const std::string &s, double px, int weight, double r, double g, double b,
          double a, double x, double y, Align align, bool tabular = false)
{
    PangoLayout *layout = layoutFor(widget, s, px, weight, tabular);
    int tw = 0, th = 0;
    pango_layout_get_pixel_size(layout, &tw, &th);
    const double tx = align == Align::Left ? x : align == Align::Center ? x - tw / 2.0 : x - tw;
    cairo_set_source_rgba(cr, r, g, b, a);
    cairo_move_to(cr, tx, y - th / 2.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
}

double textWidth(GtkWidget *widget, const std::string &s, double px, int weight)
{
    PangoLayout *layout = layoutFor(widget, s, px, weight, false);
    int tw = 0;
    pango_layout_get_pixel_size(layout, &tw, nullptr);
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

// A band past the bottom has run off the gel; one near the edge fades out.
double onGel(double position) { return std::clamp((1.03 - position) / 0.05, 0.0, 1.0); }

// Lanes are never loaded quite evenly: a fixed, slight difference per lane.
double loading(int lane)
{
    static const double wobble[] = {1, 0.94, 1.03, 0.97, 1.05, 0.95, 1.01, 0.96, 1.04, 0.98, 1.02};
    return wobble[size_t(lane) % (sizeof wobble / sizeof wobble[0])];
}

// A fixed scatter of points, 0..1, for the grain.
const std::vector<std::pair<double, double>> &grain()
{
    static const std::vector<std::pair<double, double>> points = [] {
        std::vector<std::pair<double, double>> out;
        guint32 seed = 12345;
        auto next = [&] {
            seed = seed * 1664525u + 1013904223u;
            return double(seed >> 8) / double(1 << 24);
        };
        for (int i = 0; i < 700; ++i) {
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

const char *gelStainTitle(GelStain stain)
{
    switch (stain) {
    case GelStain::SybrSafe:
        return "SYBR Safe · Blue Light";
    case GelStain::BlackAndWhite:
        return "Black & White";
    case GelStain::EthidiumUV:
        break;
    }
    return "EtBr · UV";
}

const std::vector<GelComb> &gelCombs()
{
    static const std::vector<GelComb> combs{
        {"8 wells · 1.5 mm", 8, 6.0, 1.5},    {"10 wells · 1.0 mm", 10, 5.0, 1.0}, {"12 wells · 1.0 mm", 12, 4.0, 1.0},
        {"15 wells · 0.75 mm", 15, 3.2, 0.75}, {"20 wells · 0.75 mm", 20, 2.3, 0.75},
    };
    return combs;
}

GelView::GelView()
    : m_comb(gelCombs()[1])
{
    // Held here as well as by the sheet: the wizard is deleted after its
    // dialog is gone, and the run's tick callback is taken off a live widget.
    m_area = GTK_WIDGET(g_object_ref_sink(gtk_drawing_area_new()));
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
    g_object_unref(m_area);
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

void GelView::setComb(const GelComb &comb)
{
    m_comb = comb;
    gtk_widget_queue_draw(m_area);
}

void GelView::setRunLevel(double level)
{
    m_runLevel = std::clamp(level, 0.01, 1.0);
    gtk_widget_queue_draw(m_area);
}

void GelView::setDyeRuns(std::vector<double> runs)
{
    m_dyeRuns = std::move(runs);
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

// How far lane `lane` has got, eased and scaled by the run level.
double GelView::reach(int lane) const
{
    const double t = lane >= 0 && lane < int(m_progress.size()) ? m_progress[size_t(lane)] : 1;
    return (1 - std::pow(1 - t, 3)) * m_runLevel;
}

// Thicker for more DNA (and a thicker comb); wider and softer the further it
// ran, as bands diffuse.
double GelView::bandHeight(const GelBandModel &b, double position, double scale) const
{
    return ((2.5 + 4.5 * b.intensity) * m_comb.plug() + 2.5 * std::max(0.0, position)) * std::min(1.5, scale);
}

GelView::Geometry GelView::geometry(double w, double h) const
{
    // Above the controls at the bottom.
    const double ax = 24, ay = 24, aw = std::max(100.0, w - 48), ah = std::max(120.0, h - ay - m_bottomInset);
    // Lanes grow to fill the pane (a few lanes make a wide gel, not a narrow
    // strip), with the gaps and the size labels growing with them; many
    // lanes shrink until the whole comb fits beside the ladder's sizes.
    const double n = double(std::max<size_t>(1, m_lanes.size()));
    const bool ladder = std::any_of(m_lanes.begin(), m_lanes.end(), [](const GelLaneModel &l) { return l.ladder; });
    const double unit = (aw - (ladder ? 60 : 0)) / (n * 1.25 + 0.25);
    double laneW = std::clamp(unit, 12.0, 120.0);
    const double scale = std::max(1.0, laneW / 64);
    double gap = std::clamp(laneW * 0.25, 4.0, 28.0);
    // The ladder's sizes sit left of the slab; the slab is centred in the
    // pane, and only moves over when they wouldn't fit beside it.
    const double labelW = ladder ? 60 * std::min(1.4, scale) : 0;
    double slabW = n * laneW + (n + 1) * gap;
    if (labelW + slabW > aw) {
        const double f = (aw - labelW) / slabW;
        laneW *= f;
        gap *= f;
        slabW = aw - labelW;
    }
    const double slabX = ax + std::max(labelW, (aw - slabW) / 2);
    // The gel is as long whatever is loaded: its run is the pane's height,
    // so the same fragment sits at the same place on every gel.
    const double slabY = ay + 24, slabH = ah - 24;
    return {slabX, slabY, slabW, slabH, laneW, gap, scale, labelW, slabY + 34, slabY + slabH - 22};
}

void GelView::draw(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer p)
{
    static_cast<GelView *>(p)->render(cr, w, h, false);
}

void GelView::render(cairo_t *cr, double w, double h, bool forPicture) const
{
    // The imager's dark box, a little lighter in the middle.
    cairo_pattern_t *backdrop = cairo_pattern_create_radial(w / 2, h * 0.45, 0, w / 2, h * 0.45, std::max(w, h) * 0.7);
    cairo_pattern_add_color_stop_rgb(backdrop, 0, 0.13, 0.13, 0.13);
    cairo_pattern_add_color_stop_rgb(backdrop, 1, 0.03, 0.03, 0.03);
    cairo_set_source(cr, backdrop);
    cairo_paint(cr);
    cairo_pattern_destroy(backdrop);

    const Geometry g = geometry(w, h);
    drawSlab(cr, g);

    // A well for each lane (the picture is cropped to the lanes loaded, so
    // the gel sits in the middle): dark inside, the far wall shaded, a lit
    // lip below.
    const double wellH = (5 + 3 * m_comb.thickness) * std::min(1.5, g.scale);
    const double wellY = g.slabY + 14;
    for (int i = 0; i < std::max<int>(1, int(m_lanes.size())); ++i) {
        const double x = g.laneX(i) + 2, ww = g.laneW - 4;
        roundedRect(cr, x, wellY, ww, wellH, 1.5);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.75);
        cairo_fill(cr);
        cairo_pattern_t *wall = cairo_pattern_create_linear(0, wellY, 0, wellY + wellH / 2);
        cairo_pattern_add_color_stop_rgba(wall, 0, 0, 0, 0, 0.5);
        cairo_pattern_add_color_stop_rgba(wall, 1, 0, 0, 0, 0);
        cairo_rectangle(cr, x, wellY, ww, wellH / 2);
        cairo_set_source(cr, wall);
        cairo_fill(cr);
        cairo_pattern_destroy(wall);
        cairo_rectangle(cr, x + 1, wellY + wellH, ww - 2, 1);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.16);
        cairo_fill(cr);
        cairo_rectangle(cr, x, wellY - 1, ww, 1);
        cairo_set_source_rgba(cr, 1, 1, 1, 0.05);
        cairo_fill(cr);
    }

    const Hit hover = forPicture ? Hit{} : m_hover;
    const int selected = forPicture ? -1 : m_selected;
    const double namePx = std::min(16.0, 11 * g.scale);
    for (size_t i = 0; i < m_lanes.size(); ++i) {
        const GelLaneModel &lane = m_lanes[i];
        const double x = g.laneX(int(i));
        // The lane being edited, and the one under the pointer.
        const double lit = int(i) == selected ? 0.07 : (hover.lane == int(i) ? 0.035 : 0);
        if (lit > 0) {
            roundedRect(cr, x - 4, g.slabY + 6, g.laneW + 8, g.slabH - 12, 7);
            cairo_set_source_rgba(cr, 1, 1, 1, lit);
            cairo_fill(cr);
        }
        text(cr, m_area, lane.name, namePx, PANGO_WEIGHT_SEMIBOLD, 1, 1, 1,
             int(i) == selected || forPicture ? 1 : 0.6, x + g.laneW / 2, g.slabY - 13, Align::Center);

        const double far = reach(int(i));
        const double fadeIn = std::min(1.0, (i < m_progress.size() ? m_progress[i] : 1) * 4);
        // The loading dyes quench the light a little: soft shadows in the gel.
        for (double dye : m_dyeRuns) {
            const double a = 0.22 * onGel(dye * far) * fadeIn;
            if (a <= 0.01)
                continue;
            const double y = g.y(dye, far);
            for (int k = 4; k >= 0; --k) {
                roundedRect(cr, x + 4 - k, y - 3 - k, g.laneW - 8 + 2 * k, 6 + 2 * k, 3 + k);
                cairo_set_source_rgba(cr, 0, 0, 0, a * (k == 0 ? 0.6 : 0.12));
                cairo_fill(cr);
            }
        }
        for (size_t b = 0; b < lane.bands.size(); ++b) {
            const GelBandModel &band = lane.bands[b];
            const double position = band.run * far;
            const double visible = onGel(position);
            if (visible <= 0)
                continue;
            const double bh = bandHeight(band, position, g.scale);
            drawBand(cr, x + 3, g.y(band.run, far) - bh / 2, g.laneW - 6, bh,
                     band.intensity * fadeIn * visible * loading(int(i)), hover.lane == int(i) && hover.band == int(b));
        }
    }

    // The first ladder's sizes on the left, the reference bands bolder.
    const auto ladder = std::find_if(m_lanes.begin(), m_lanes.end(), [](const GelLaneModel &l) { return l.ladder; });
    if (ladder != m_lanes.end()) {
        const int li = int(ladder - m_lanes.begin());
        const double far = reach(li);
        std::vector<GelBandModel> bands = ladder->bands;
        std::sort(bands.begin(), bands.end(), [](const GelBandModel &a, const GelBandModel &b) { return a.run < b.run; });
        const double px = std::min(14.0, 10 * g.scale);
        const double full = far / std::max(0.01, m_runLevel);
        double lastY = -1e9;
        for (const GelBandModel &band : bands) {
            const double y = g.y(band.run, far);
            if (onGel(band.run * far) <= 0.5 || y - lastY < 11 * std::min(1.4, g.scale))
                continue;
            lastY = y;
            text(cr, m_area, sizeText(band.bp), px, band.reference ? PANGO_WEIGHT_SEMIBOLD : PANGO_WEIGHT_NORMAL, 1, 1,
                 1, (band.reference ? 0.8 : 0.5) * full, g.slabX - 10, y, Align::Right, true);
        }
    }
    if (m_lanes.size() <= 1 && !forPicture)
        text(cr, m_area, "Add a lane to run a sample", 13, PANGO_WEIGHT_NORMAL, 1, 1, 1, 0.5, g.slabX + g.slabW / 2,
             g.slabY + g.slabH / 2, Align::Center);
    if (hover.lane >= 0 && hover.band >= 0)
        drawCallout(cr, g, w);
}

// The slab on the light box: a cut edge that catches the light, a slight
// thickness below it, the tubes faintly through it, free stain hazing the
// top, and the camera's grain.
void GelView::drawSlab(cairo_t *cr, const Geometry &g) const
{
    const StainColors c = colors(m_stain);
    // A soft shadow under it: rings fading out, a little lower.
    for (int i = 14; i >= 1; --i) {
        roundedRect(cr, g.slabX - i, g.slabY - i + 8, g.slabW + 2 * i, g.slabH + 2 * i, 10 + i);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.05);
        cairo_fill(cr);
    }
    roundedRect(cr, g.slabX, g.slabY, g.slabW, g.slabH, 10);
    cairo_set_source_rgb(cr, c.gelBottom.r, c.gelBottom.g, c.gelBottom.b);
    cairo_fill(cr);
    // Its thickness: a darker lip along the bottom edge.
    roundedRect(cr, g.slabX + 2, g.slabY + g.slabH - 4, g.slabW - 4, 5, 3);
    cairo_set_source_rgba(cr, 0, 0, 0, 0.35);
    cairo_fill(cr);
    roundedRect(cr, g.slabX, g.slabY, g.slabW, g.slabH, 10);
    cairo_pattern_t *slab = cairo_pattern_create_linear(0, g.slabY, 0, g.slabY + g.slabH);
    cairo_pattern_add_color_stop_rgb(slab, 0, c.gelTop.r, c.gelTop.g, c.gelTop.b);
    cairo_pattern_add_color_stop_rgb(slab, 1, c.gelBottom.r, c.gelBottom.g, c.gelBottom.b);
    cairo_set_source(cr, slab);
    cairo_fill_preserve(cr);
    cairo_pattern_destroy(slab);

    cairo_save(cr);
    cairo_clip(cr);
    if (c.tubes) {
        // The light box's tubes, running across under the gel.
        for (double y = g.slabY + 18; y < g.slabY + g.slabH; y += 56) {
            cairo_pattern_t *tube = cairo_pattern_create_linear(0, y, 0, y + 26);
            cairo_pattern_add_color_stop_rgba(tube, 0, c.glow.r, c.glow.g, c.glow.b, 0);
            cairo_pattern_add_color_stop_rgba(tube, 0.5, c.glow.r, c.glow.g, c.glow.b, 0.06);
            cairo_pattern_add_color_stop_rgba(tube, 1, c.glow.r, c.glow.g, c.glow.b, 0);
            cairo_rectangle(cr, g.slabX, y, g.slabW, 26);
            cairo_set_source(cr, tube);
            cairo_fill(cr);
            cairo_pattern_destroy(tube);
        }
    }
    // Stain that hasn't bound anything glows faintly near the wells.
    cairo_pattern_t *haze = cairo_pattern_create_linear(0, g.slabY, 0, g.slabY + g.slabH * 0.35);
    cairo_pattern_add_color_stop_rgba(haze, 0, c.glow.r, c.glow.g, c.glow.b, 0.08);
    cairo_pattern_add_color_stop_rgba(haze, 1, c.glow.r, c.glow.g, c.glow.b, 0);
    cairo_rectangle(cr, g.slabX, g.slabY, g.slabW, g.slabH * 0.35);
    cairo_set_source(cr, haze);
    cairo_fill(cr);
    cairo_pattern_destroy(haze);
    cairo_set_source_rgba(cr, 1, 1, 1, c.grain);
    for (const auto &[gx, gy] : grain())
        cairo_rectangle(cr, std::floor(g.slabX + gx * g.slabW), std::floor(g.slabY + gy * g.slabH), 1, 1);
    cairo_fill(cr);
    cairo_restore(cr);

    // The cut edge, catching the light along the top more than the sides.
    roundedRect(cr, g.slabX + 0.5, g.slabY + 0.5, g.slabW - 1, g.slabH - 1, 10);
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.09);
    cairo_stroke_preserve(cr);
    cairo_save(cr);
    cairo_rectangle(cr, g.slabX - 1, g.slabY - 1, g.slabW + 2, 13);
    cairo_clip(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.12);
    cairo_stroke(cr);
    cairo_restore(cr);
    cairo_new_path(cr);
}

// A band: brightest along its middle, fading at the lane's edges and smiling
// slightly, with a glow in the stain's colour that grows with the amount of DNA.
void GelView::drawBand(cairo_t *cr, double x, double y, double w, double h, double intensity, bool hovered) const
{
    if (intensity <= 0.01)
        return;
    const StainColors c = colors(m_stain);
    const double a = std::min(1.0, intensity * (hovered ? 1.25 : 1));
    const double smile = std::min(1.5, w * 0.03), r = h / 2;
    auto shape = [&](double grow) {
        const double x0 = x - grow, x1 = x + w + grow, y0 = y - grow, y1 = y + h + grow, rr = r + grow;
        cairo_new_sub_path(cr);
        cairo_move_to(cr, x0 + rr, y0);
        quadTo(cr, (x0 + x1) / 2, y0 + 2 * smile, x1 - rr, y0);
        cairo_arc(cr, x1 - rr, (y0 + y1) / 2, rr, -kPi / 2, kPi / 2);
        quadTo(cr, (x0 + x1) / 2, y1 + 2 * smile, x0 + rr, y1);
        cairo_arc(cr, x0 + rr, (y0 + y1) / 2, rr, kPi / 2, 3 * kPi / 2);
        cairo_close_path(cr);
    };
    // The glow: rings of the stain's colour, fainter outwards, wider for more DNA.
    const int rings = int(3 + 6 * a);
    for (int i = rings; i >= 1; --i) {
        shape(i * 1.5);
        cairo_set_source_rgba(cr, c.glow.r, c.glow.g, c.glow.b, 0.13 * a * (1 - double(i) / (rings + 1)));
        cairo_fill(cr);
    }
    shape(0);
    cairo_pattern_t *core = cairo_pattern_create_linear(x, 0, x + w, 0);
    cairo_pattern_add_color_stop_rgba(core, 0, c.core.r, c.core.g, c.core.b, 0.4 * a);
    cairo_pattern_add_color_stop_rgba(core, 0.3, c.core.r, c.core.g, c.core.b, a);
    cairo_pattern_add_color_stop_rgba(core, 0.7, c.core.r, c.core.g, c.core.b, a);
    cairo_pattern_add_color_stop_rgba(core, 1, c.core.r, c.core.g, c.core.b, 0.4 * a);
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
    const double tw = std::max(textWidth(m_area, title, 12, PANGO_WEIGHT_SEMIBOLD),
                               b.caption.empty() ? 0 : textWidth(m_area, b.caption, 11, PANGO_WEIGHT_NORMAL));
    const double w = tw + 20, h = b.caption.empty() ? 26 : 42;
    const double y = g.y(b.run, reach(m_hover.lane));
    double x = g.laneX(m_hover.lane) + g.laneW + 10;
    if (x + w > width - 8)
        x = g.laneX(m_hover.lane) - 10 - w;
    roundedRect(cr, x, y - h / 2, w, h, 9);
    cairo_set_source_rgba(cr, 0.06, 0.06, 0.06, 0.88);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.16);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    text(cr, m_area, title, 12, PANGO_WEIGHT_SEMIBOLD, 1, 1, 1, 1, x + 10, y - h / 2 + 13, Align::Left, true);
    if (!b.caption.empty())
        text(cr, m_area, b.caption, 11, PANGO_WEIGHT_NORMAL, 1, 1, 1, 0.65, x + 10, y - h / 2 + 30, Align::Left);
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
    const double far = reach(out.lane);
    double best = 1e9;
    const auto &bands = m_lanes[size_t(out.lane)].bands;
    for (size_t b = 0; b < bands.size(); ++b) {
        const double position = bands[b].run * far;
        if (onGel(position) <= 0)
            continue;
        const double d = std::abs(y - g.y(bands[b].run, far));
        if (d <= std::max(5.0, bandHeight(bands[b], position, g.scale) / 2 + 3) && d < best) {
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

GBytes *GelView::snapshotPng(const std::vector<std::pair<std::string, std::string>> &legend,
                             const std::string &caption)
{
    std::fill(m_progress.begin(), m_progress.end(), 1);
    const double w = std::max(1, gtk_widget_get_width(m_area)), h = std::max(1, gtk_widget_get_height(m_area));
    const Geometry g = geometry(w, h);
    // Cropped to the gel and its labels.
    const double cx = std::max(0.0, g.slabX - g.labelW - 8), cy = std::max(0.0, g.slabY - 30);
    const double cw = std::min(w - cx, g.slabW + g.labelW + 24), ch = std::min(h - cy, g.slabH + 30 + 16);
    const double lineH = 18;
    const double legendH = 14 + legend.size() * lineH + 6 + lineH + 10;
    const int scale = std::max(1, gtk_widget_get_scale_factor(m_area));
    cairo_surface_t *surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, int(cw * scale), int((ch + legendH) * scale));
    cairo_t *cr = cairo_create(surface);
    cairo_scale(cr, scale, scale);
    cairo_set_source_rgb(cr, 0.05, 0.05, 0.05);
    cairo_paint(cr);
    cairo_save(cr);
    cairo_rectangle(cr, 0, 0, cw, ch);
    cairo_clip(cr);
    cairo_translate(cr, -cx, -cy);
    render(cr, w, h, true);
    cairo_restore(cr);
    // What each lane holds, and how the gel was run, under the picture.
    double y = ch + 14 + lineH / 2;
    for (const auto &[lane, what] : legend) {
        text(cr, m_area, lane, 12, PANGO_WEIGHT_BOLD, 1, 1, 1, 1, 16, y, Align::Left);
        text(cr, m_area, what, 12, PANGO_WEIGHT_NORMAL, 1, 1, 1, 0.8, 40, y, Align::Left);
        y += lineH;
    }
    text(cr, m_area, caption, 11, PANGO_WEIGHT_NORMAL, 1, 1, 1, 0.55, 16, y + 6, Align::Left);
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
