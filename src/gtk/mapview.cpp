// SPDX-License-Identifier: GPL-3.0-or-later
#include "mapview.h"

#include <cmath>

namespace oc::gtk {

namespace {
const double kPi = 3.14159265358979323846;
}

MapView::MapView(Document *doc) : m_doc(doc)
{
    m_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(m_area, TRUE);
    gtk_widget_set_vexpand(m_area, TRUE);
    gtk_widget_set_size_request(m_area, 320, 280);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(m_area), &MapView::draw, this, nullptr);

    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(&MapView::dragBegin), this);
    g_signal_connect(drag, "drag-update", G_CALLBACK(&MapView::dragUpdate), this);
    g_signal_connect(drag, "drag-end", G_CALLBACK(&MapView::dragEnd), this);
    gtk_widget_add_controller(m_area, GTK_EVENT_CONTROLLER(drag));

    gtk_widget_set_has_tooltip(m_area, TRUE);
    g_signal_connect(m_area, "query-tooltip", G_CALLBACK(&MapView::queryTooltip), this);

    m_doc->onSequenceChanged([this] { invalidate(); });
    m_doc->onEnzymesChanged([this] { invalidate(); });
    m_doc->onSelectionChanged([this] { gtk_widget_queue_draw(m_area); });
}

MapView::~MapView() = default;

void MapView::invalidate()
{
    m_layout.reset();
    gtk_widget_queue_draw(m_area);
}

void MapView::setDark(bool dark)
{
    if (dark != m_dark) {
        m_dark = dark;
        invalidate();
    }
}

void MapView::setOptions(view::MapOptions options)
{
    m_options = std::move(options);
    invalidate();
}

void MapView::ensureLayout(int w, int h)
{
    if (m_layout && w == m_w && h == m_h)
        return;
    m_w = w;
    m_h = h;
    m_layout = std::make_unique<view::MapLayout>(m_doc->sequence(), m_doc->visibleSites(),
                                                 m_dark ? view::Theme::darkTheme() : view::Theme::light(),
                                                 m_measure, w, h, m_options);
}

void MapView::draw(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer p)
{
    auto *self = static_cast<MapView *>(p);
    self->ensureLayout(w, h);
    view::MapState st;
    st.hasSelection = self->m_doc->hasSelection();
    st.selection = self->m_doc->selection();
    st.activeFeature = self->m_doc->activeFeature();
    st.activeSite = self->m_doc->activeSite();
    view::drawScene(cr, self->m_layout->render(st));
}

void MapView::dragBegin(GtkGestureDrag *, double x, double y, gpointer p)
{
    auto *self = static_cast<MapView *>(p);
    if (!self->m_layout || self->m_doc->isEmpty())
        return;
    self->m_x0 = x;
    self->m_y0 = y;
    self->m_selecting = false;
    const view::MapHit hit = self->m_layout->hitTest({x, y});
    switch (hit.kind) {
    case view::MapHit::Feature:
        self->m_doc->activateFeature(hit.index);
        break;
    case view::MapHit::Site:
        self->m_doc->activateSite(hit.index);
        break;
    case view::MapHit::Sequence:
        self->m_selecting = true;
        self->m_anchor = hit.position;
        self->m_lastAngle = self->m_layout->angleAt({x, y});
        self->m_accum = 0;
        self->m_doc->setSelection({hit.position, 0});
        break;
    default:
        break;
    }
}

void MapView::dragUpdate(GtkGestureDrag *, double dx, double dy, gpointer p)
{
    auto *self = static_cast<MapView *>(p);
    if (!self->m_selecting || !self->m_layout)
        return;
    const view::Point pt{self->m_x0 + dx, self->m_y0 + dy};
    const int n = self->m_doc->sequence().length();
    if (self->m_layout->isCircular()) {
        // Accumulate the swept angle so a drag can pass the origin either way.
        const double a = self->m_layout->angleAt(pt);
        double d = a - self->m_lastAngle;
        if (d > kPi)
            d -= 2 * kPi;
        else if (d < -kPi)
            d += 2 * kPi;
        self->m_accum = std::max(-2 * kPi, std::min(2 * kPi, self->m_accum + d));
        self->m_lastAngle = a;
        const int len = int(std::lround(self->m_accum / (2 * kPi) * n));
        if (len >= 0)
            self->m_doc->setSelection({self->m_anchor, len});
        else
            self->m_doc->setSelection({self->m_anchor + len, -len});
    } else {
        const int pos = self->m_layout->positionAt(pt);
        self->m_doc->setSelection({std::min(pos, self->m_anchor), std::abs(pos - self->m_anchor)});
    }
}

void MapView::dragEnd(GtkGestureDrag *, double, double, gpointer p)
{
    static_cast<MapView *>(p)->m_selecting = false;
}

gboolean MapView::queryTooltip(GtkWidget *, int x, int y, gboolean, GtkTooltip *tip, gpointer p)
{
    auto *self = static_cast<MapView *>(p);
    if (!self->m_layout)
        return FALSE;
    const std::string text = self->m_layout->tooltipAt({double(x), double(y)});
    if (text.empty())
        return FALSE;
    gtk_tooltip_set_text(tip, text.c_str());
    return TRUE;
}

} // namespace oc::gtk
