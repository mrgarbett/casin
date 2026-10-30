// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <gtk/gtk.h>
#include <functional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace oc::gtk {

struct GelBandModel {
    int bp = 0;
    double run = 0;         // 0 at the wells, 1 at the bottom of a full run (more: ran off the gel)
    double intensity = 0;   // 0..1
    bool reference = false;
    std::string caption;    // what the band is: "EcoRI – BamHI", "uncut, supercoiled"…
};

struct GelLaneModel {
    std::string name;
    std::vector<GelBandModel> bands;
    bool ladder = false;
};

// How the gel was stained and photographed.
enum class GelStain { EthidiumUV, SybrSafe, BlackAndWhite };
const char *gelStainTitle(GelStain stain);

// The comb that cast the wells: how many, how wide each is, and how thick a
// plug of DNA it loads (a thin comb gives sharper, brighter bands).
struct GelComb {
    std::string name;
    int teeth = 10;
    double width = 5, thickness = 1;   // mm
    // How much thinner (or thicker) than a 1 mm comb's a fresh band is.
    double plug() const { return 0.6 + 0.4 * thickness; }
};
// Combs as sold for a 7 cm mini gel tray; the second is the usual one.
const std::vector<GelComb> &gelCombs();

// The gel as it looks on the imager (as on the Mac, #34 and #35): an agarose
// slab on the light box, its wells cut by the comb, bands glowing in the
// stain's colour (brighter where there's more DNA, wider and softer the
// further they ran, smiling slightly as real bands do), and the loading
// dyes' shadows marking how far the run went. When a lane changes, its bands
// run down from the well; the run level stops the whole gel part way. The
// core says where each band runs and how bright it is (view::gelRunFraction,
// gelBandIntensity); this view only draws, in cairo.
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
    // Every well the comb cast is on the gel, loaded or not.
    void setComb(const GelComb &comb);
    // How long the gel ran, as a fraction of a full run.
    void setRunLevel(double level);
    // Where the loading dyes' fronts are after a full run, as band runs.
    void setDyeRuns(std::vector<double> runs);
    // The lane being edited, lit a little.
    void setSelectedLane(int lane);
    // Room left at the bottom for the floating controls.
    void setBottomInset(int inset);
    // The gel as a PNG for a notebook: cropped to the gel and its labels,
    // fully run, without the pointer's bubble or the selected lane's light,
    // with what each lane holds (lane name, contents) and `caption` below.
    GBytes *snapshotPng(const std::vector<std::pair<std::string, std::string>> &legend, const std::string &caption);

    std::function<void(int)> onSelectLane;
    std::function<void(int, int)> onPickBand;   // lane, band

private:
    struct Geometry {
        double slabX, slabY, slabW, slabH, laneW, gap, scale, labelW, runTop, runBottom;
        double laneX(int i) const { return slabX + gap + i * (laneW + gap); }
        // Where a band of `run` is when its lane has reached this far.
        double y(double run, double reach) const { return runTop + run * reach * (runBottom - runTop); }
    };
    struct Hit {
        int lane = -1, band = -1;
    };

    Geometry geometry(double w, double h) const;
    double reach(int lane) const;
    double bandHeight(const GelBandModel &b, double position, double scale) const;
    void render(cairo_t *cr, double w, double h, bool forPicture) const;
    void drawSlab(cairo_t *cr, const Geometry &g) const;
    void drawBand(cairo_t *cr, double x, double y, double w, double h, double intensity, bool hovered) const;
    void drawCallout(cairo_t *cr, const Geometry &g, double width) const;
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
    GelStain m_stain = GelStain::EthidiumUV;
    GelComb m_comb;
    double m_runLevel = 1;
    std::vector<double> m_dyeRuns;
    int m_selected = 1;
    int m_bottomInset = 64;
    Hit m_hover;
    guint m_tick = 0;
    gint64 m_lastFrame = 0;
};

} // namespace oc::gtk
