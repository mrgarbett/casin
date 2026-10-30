// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <gtk/gtk.h>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace oc::gtk {

struct GelBandModel {
    int bp = 0;
    double run = 0;         // 0 at the wells, 1 at the bottom of the run
    double intensity = 0;   // 0..1
    bool reference = false;
    std::string caption;    // what the band is: "EcoRI – BamHI", "uncut, supercoiled"…
};

struct GelLaneModel {
    std::string name;
    std::vector<GelBandModel> bands;
    bool ladder = false;
};

enum class GelStain { UV, BlueLight };

// The gel as an imager shows it (as on the Mac, #34): a dark agarose slab
// with its wells, and bands that glow in the stain's colour, brighter where
// there's more DNA. When a lane changes, its bands run down from the well to
// where they stop. The core says where each band runs and how bright it is
// (view::gelRunFraction, gelBandIntensity); this view only draws, in cairo.
class GelView {
public:
    GelView();
    ~GelView();
    GtkWidget *widget() const { return m_area; }

    // Shows `lanes`; the lanes in `running` run from their wells again.
    void setLanes(std::vector<GelLaneModel> lanes, const std::set<int> &running);
    const std::vector<GelLaneModel> &lanes() const { return m_lanes; }
    void setStain(GelStain stain);
    GelStain stain() const { return m_stain; }
    // The lane being edited, lit a little.
    void setSelectedLane(int lane);
    // Room left at the bottom for the floating controls.
    void setBottomInset(int inset);
    // The gel as a PNG, fully run and without the pointer's bubble.
    GBytes *snapshotPng();

    std::function<void(int)> onSelectLane;
    std::function<void(int, int)> onPickBand;   // lane, band

private:
    struct Geometry {
        double slabX, slabY, slabW, slabH, laneW, gap, runTop, runBottom;
        double laneX(int i) const { return slabX + gap + i * (laneW + gap); }
        double y(double run, double eased) const { return runTop + run * eased * (runBottom - runTop); }
    };
    struct Hit {
        int lane = -1, band = -1;
    };

    Geometry geometry(double w, double h) const;
    double eased(int lane) const;
    void render(cairo_t *cr, double w, double h, bool withHover) const;
    void drawBand(cairo_t *cr, double x, double y, double w, double h, double intensity, bool hovered) const;
    void drawCallout(cairo_t *cr, const Geometry &g, double w) const;
    Hit hit(double x, double y) const;
    void startRunning();

    static void draw(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer self);
    static gboolean tick(GtkWidget *, GdkFrameClock *clock, gpointer self);
    static void motion(GtkEventControllerMotion *, double x, double y, gpointer self);
    static void leave(GtkEventControllerMotion *, gpointer self);
    static void pressed(GtkGestureClick *, int n, double x, double y, gpointer self);

    GtkWidget *m_area;
    std::vector<GelLaneModel> m_lanes;
    std::vector<double> m_progress;   // how far each lane has run, 0..1, linear in time
    GelStain m_stain = GelStain::UV;
    int m_selected = 1;
    int m_bottomInset = 64;
    Hit m_hover;
    guint m_tick = 0;
    gint64 m_lastFrame = 0;
};

} // namespace oc::gtk
