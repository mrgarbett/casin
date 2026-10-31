// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "gelview.h"

#include <gtk/gtk.h>
#include <cstdint>
#include <string>
#include <vector>

namespace oc::gtk {

// How the 3D gel is stained and lit (as on the Mac): the plate's light, and
// the colours the stain gives off, free in the gel and bound to the DNA.
enum class Gel3DScene { PinkOnBlue, GreenOnBlue, BlackAndWhite };
const char *gel3DSceneTitle(Gel3DScene scene);

// The knobs of the 3D picture, for tuning the look by eye (the pill's
// sliders); the defaults are what ships.
struct Gel3DLook {
    float exposure = 0.75f;   // how bright the picture is developed
    float contrast = 1.25f;   // about mid grey; 1 leaves it alone
    float stain = 1.0f;       // the free stain's glow, as a multiple
    float band = 1.0f;        // the DNA's glow, as a multiple
    float scatter = 18.0f;    // how much the gel scatters, per metre
    float lamp = 7.0f;        // the spotlight's brightness
    float lampSize = 0.09f;   // the spotlight's size, m
    float spot = 0.35f;       // the spotlight's cone, radians (half angle)
    float bloom = 0.25f;      // the bright parts' halo
    float vignette = 0.6f;    // how dark the corners go
    float grain = 0.02f;      // the camera's noise
};

// EXPERIMENTAL: the gel photographed on a blue-light box, path traced on the
// GPU (a port of the Mac's Metal tracer to OpenGL 3.3 fragment shaders).
// Nothing is painted to look like a gel: a block of agarose (refracting,
// faintly scattering, stain-filled, with a sagging top, a lip at the edges
// and the wells cut in) lies on a glowing plate under a dark room with one
// lamp, and the picture is what a camera would see. Each frame adds a sample
// per pixel; the picture converges while the gel is still, then the GPU
// rests. While it's being turned it traces at half resolution, so it keeps
// up on a laptop's integrated GPU. Drag to orbit, scroll to come closer,
// double-click to go back. View only: the sidebar does the editing.
class Gel3DView {
public:
    Gel3DView();
    ~Gel3DView();
    GtkWidget *widget() const { return m_root; }

    // Shows `lanes`, run `runLevel` of the way; `dyeRuns` are the loading
    // dyes' fronts after a full run.
    void show(const std::vector<GelLaneModel> &lanes, double runLevel, const std::vector<double> &dyeRuns);
    void setScene(Gel3DScene scene);
    // The comb that cast the wells; the tray is 7 cm wide whatever it is.
    void setComb(const GelComb &comb);
    void setLook(const Gel3DLook &look);
    const Gel3DLook &look() const { return m_look; }
    // Why there's no picture, if there isn't one.
    const std::string &problem() const { return m_problem; }

    struct Vec3 {
        float x, y, z;
    };

private:
    struct Camera {
        Vec3 position, forward, right, up;
        float distance;
    };
    Camera camera() const;
    void pan(double dx, double dy);
    void rebuild();
    void restart();
    void fail(const std::string &message);
    float pitch() const;
    float margin() const;
    void makeBandMap();
    bool setUpGl();
    void render();
    void interacted();

    static void realize(GtkGLArea *, gpointer self);
    static void unrealize(GtkGLArea *, gpointer self);
    static gboolean renderCb(GtkGLArea *, GdkGLContext *, gpointer self);
    static gboolean tick(GtkWidget *, GdkFrameClock *, gpointer self);

    GtkWidget *m_root, *m_area, *m_note;
    std::string m_problem;

    // GL objects, made when the area is realized.
    unsigned m_trace = 0, m_resolve = 0, m_vao = 0, m_bandTex = 0;
    unsigned m_accum[2] = {0, 0}, m_fbo[2] = {0, 0};
    int m_accumW = 0, m_accumH = 0;
    bool m_glReady = false, m_bandDirty = true;

    uint32_t m_samples = 0, m_frame = 0;
    guint m_tick = 0, m_idle = 0, m_buildTimer = 0;
    gint64 m_lastBuild = 0, m_lastRender = 0;
    double m_frameMs = 0;   // how long a traced frame takes, smoothed
    // How many samples to take before resting: fewer on a slow renderer,
    // so software GL and a weak GPU still come to rest.
    uint32_t maxSamples() const;
    bool m_moving = false;   // being turned or zoomed: trace coarsely

    static constexpr float kHomePitch = 0.80f;
    float m_yaw = 0, m_pitch = kHomePitch, m_zoom = 1, m_fit = 0.2f;
    Vec3 m_pan = {0, 0, 0};   // where the camera looks, off the gel's middle
    double m_dragX = 0, m_dragY = 0;
    bool m_panning = false;   // this drag moves the gel rather than turning it
    std::vector<GelLaneModel> m_lanes;
    double m_runLevel = 1;
    std::vector<double> m_dyeRuns;
    Gel3DScene m_scene = Gel3DScene::PinkOnBlue;
    Gel3DLook m_look;
    GelComb m_comb;
    float m_width = 0.1f;
    static constexpr float kDepth = 0.10f, kThick = 0.006f;
    int m_mapW = 0, m_mapH = 0;
    std::vector<uint8_t> m_map;   // rgba8: r = DNA, g = loading dye, b = well (0.5: its walls)
};

} // namespace oc::gtk
