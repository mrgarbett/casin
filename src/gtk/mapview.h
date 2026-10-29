// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "view/cairo_render.h"
#include "view/document.h"
#include "view/maplayout.h"

#include <gtk/gtk.h>
#include <functional>
#include <memory>

namespace oc::gtk {

// Circular or linear map, drawn with the shared MapLayout.
class MapView {
public:
    explicit MapView(Document *doc);
    ~MapView();
    GtkWidget *widget() const { return m_area; }
    void invalidate();          // content changed: rebuild the layout
    void setDark(bool dark);
    const view::MapOptions &options() const { return m_options; }
    void setOptions(view::MapOptions options);
    // Room kept clear on the right and at the bottom for capsules floating
    // over the map; the background still fills the whole view.
    void setInsets(int trailing, int bottom);

    // As on SequenceView: the base under the mouse, the mouse while a drag
    // selects bases (-1, -1 at the end), a feature double-clicked.
    std::function<void(int)> onHoverBase;
    std::function<void(double, double)> onSelectDrag;
    std::function<void(int)> onEditFeature;

private:
    static void draw(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer self);
    static void dragBegin(GtkGestureDrag *, double x, double y, gpointer self);
    static void dragUpdate(GtkGestureDrag *, double dx, double dy, gpointer self);
    static void dragEnd(GtkGestureDrag *, double dx, double dy, gpointer self);
    static gboolean queryTooltip(GtkWidget *, int x, int y, gboolean kb, GtkTooltip *tip, gpointer self);
    static void motion(GtkEventControllerMotion *, double x, double y, gpointer self);
    static void leave(GtkEventControllerMotion *, gpointer self);
    static void pressed(GtkGestureClick *, int n, double x, double y, gpointer self);
    void ensureLayout(int w, int h);

    Document *m_doc;
    GtkWidget *m_area;
    view::PangoTextMeasurer m_measure;
    std::unique_ptr<view::MapLayout> m_layout;
    int m_w = 0, m_h = 0;
    int m_trailing = 0, m_bottom = 0;
    bool m_dark = false;
    view::MapOptions m_options;
    // drag state
    bool m_selecting = false;
    double m_x0 = 0, m_y0 = 0;
    int m_anchor = 0;
    double m_lastAngle = 0, m_accum = 0;
};

} // namespace oc::gtk
