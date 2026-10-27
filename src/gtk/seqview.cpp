// SPDX-License-Identifier: GPL-3.0-or-later
#include "seqview.h"

#include <algorithm>
#include <cmath>

namespace oc::gtk {

SequenceView::SequenceView(Document *doc) : m_doc(doc)
{
    m_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    m_area = gtk_drawing_area_new();
    gtk_widget_set_hexpand(m_area, TRUE);
    gtk_widget_set_vexpand(m_area, TRUE);
    gtk_widget_set_size_request(m_area, 360, 200);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(m_area), &SequenceView::draw, this, nullptr);
    g_signal_connect(m_area, "resize", G_CALLBACK(&SequenceView::resized), this);

    m_adj = gtk_adjustment_new(0, 0, 1, 40, 400, 1);
    g_object_ref_sink(m_adj);
    g_signal_connect(m_adj, "value-changed", G_CALLBACK(&SequenceView::valueChanged), this);
    m_scrollbar = gtk_scrollbar_new(GTK_ORIENTATION_VERTICAL, m_adj);
    gtk_box_append(GTK_BOX(m_box), m_area);
    gtk_box_append(GTK_BOX(m_box), m_scrollbar);

    GtkEventController *scroll = gtk_event_controller_scroll_new(
        GtkEventControllerScrollFlags(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL | GTK_EVENT_CONTROLLER_SCROLL_KINETIC));
    g_signal_connect(scroll, "scroll", G_CALLBACK(&SequenceView::scrolled), this);
    gtk_widget_add_controller(m_area, scroll);

    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(&SequenceView::dragBegin), this);
    g_signal_connect(drag, "drag-update", G_CALLBACK(&SequenceView::dragUpdate), this);
    g_signal_connect(drag, "drag-end", G_CALLBACK(&SequenceView::dragEnd), this);
    gtk_widget_add_controller(m_area, GTK_EVENT_CONTROLLER(drag));

    gtk_widget_set_focusable(m_area, TRUE);
    gtk_widget_set_focus_on_click(m_area, TRUE);
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(&SequenceView::keyPressed), this);
    gtk_widget_add_controller(m_area, keys);

    gtk_widget_set_has_tooltip(m_area, TRUE);
    g_signal_connect(m_area, "query-tooltip", G_CALLBACK(&SequenceView::queryTooltip), this);

    m_doc->onSequenceChanged([this] {
        if (!m_doc->isEditing())
            gtk_adjustment_set_value(m_adj, 0);   // a newly loaded sequence starts at the top
        invalidate();
    });
    m_doc->onEnzymesChanged([this] { invalidate(); });
    m_doc->onReadsChanged([this] { invalidate(); });
    m_doc->onSelectionChanged([this] {
        // Keep in sync with selections made elsewhere (map, lists, find).
        if (!m_dragging && m_doc->hasSelection())
            scrollToPosition(m_doc->selection().start);
        gtk_widget_queue_draw(m_area);
    });
}

SequenceView::~SequenceView()
{
    if (m_timer)
        g_source_remove(m_timer);
    g_object_unref(m_adj);
}

void SequenceView::invalidate()
{
    m_layout.reset();
    ensureLayout();
    gtk_widget_queue_draw(m_area);
}

void SequenceView::setDark(bool dark)
{
    if (dark != m_dark) {
        m_dark = dark;
        invalidate();
    }
}

void SequenceView::setShowComplement(bool show)
{
    if (show != m_complement) {
        m_complement = show;
        invalidate();
    }
}

void SequenceView::setFrames(int frames)
{
    if (frames != m_frames) {
        m_frames = frames;
        invalidate();
    }
}

double SequenceView::scrollY() const
{
    return gtk_adjustment_get_value(m_adj);
}

void SequenceView::ensureLayout()
{
    if (m_layout || m_w <= 0)
        return;
    m_layout = std::make_unique<view::SequenceLayout>(m_doc->sequence(), m_doc->visibleSites(),
                                                      m_dark ? view::Theme::darkTheme() : view::Theme::light(),
                                                      m_measure, m_w, m_complement, m_frames, m_doc->reads());
    updateAdjustment();
}

void SequenceView::updateAdjustment()
{
    const double total = m_layout ? m_layout->totalHeight() + 20 : 0;
    const double page = std::max(1, m_h);
    const double value = std::min(gtk_adjustment_get_value(m_adj), std::max(0.0, total - page));
    gtk_adjustment_configure(m_adj, value, 0, std::max(total, page), 40, page * 0.9, page);
}

void SequenceView::scrollToPosition(int pos)
{
    ensureLayout();
    if (!m_layout)
        return;
    const double top = m_layout->lineTopFor(pos), h = m_layout->lineHeightFor(pos);
    const double y = scrollY();
    if (top < y || top + h > y + m_h)
        gtk_adjustment_set_value(m_adj, std::max(0.0, top - 20));
}

void SequenceView::resized(GtkDrawingArea *, int w, int h, gpointer p)
{
    auto *self = static_cast<SequenceView *>(p);
    if (w != self->m_w) {
        self->m_w = w;
        self->m_layout.reset();
    }
    self->m_h = h;
    self->ensureLayout();
    self->updateAdjustment();
}

void SequenceView::draw(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer p)
{
    auto *self = static_cast<SequenceView *>(p);
    if (w != self->m_w || h != self->m_h) {
        self->m_w = w;
        self->m_h = h;
        self->m_layout.reset();
    }
    self->ensureLayout();
    if (!self->m_layout)
        return;
    view::SeqViewState st;
    st.hasSelection = self->m_doc->hasSelection();
    st.selection = self->m_doc->selection();
    st.activeFeature = self->m_doc->activeFeature();
    st.activeSite = self->m_doc->activeSite();
    view::drawScene(cr, self->m_layout->render(self->scrollY(), h, st));
}

gboolean SequenceView::scrolled(GtkEventControllerScroll *c, double, double dy, gpointer p)
{
    auto *self = static_cast<SequenceView *>(p);
    const bool pixels = gtk_event_controller_scroll_get_unit(c) == GDK_SCROLL_UNIT_SURFACE;
    gtk_adjustment_set_value(self->m_adj, self->scrollY() + dy * (pixels ? 1.0 : 50.0));
    return TRUE;
}

void SequenceView::valueChanged(GtkAdjustment *, gpointer p)
{
    gtk_widget_queue_draw(static_cast<SequenceView *>(p)->m_area);
}

void SequenceView::extendSelectionTo(double x, double y)
{
    if (!m_layout)
        return;
    const int pos = m_layout->positionAt({x, y + scrollY()});
    m_doc->setSelection({std::min(pos, m_anchor), std::abs(pos - m_anchor)});
}

gboolean SequenceView::keyPressed(GtkEventControllerKey *, guint keyval, guint, GdkModifierType state, gpointer p)
{
    auto *self = static_cast<SequenceView *>(p);
    Document *doc = self->m_doc;
    if (!self->m_editable || doc->isEmpty() || (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)))
        return FALSE;
    const int n = doc->sequence().length();
    const bool shift = state & GDK_SHIFT_MASK;
    switch (keyval) {
    case GDK_KEY_BackSpace: doc->deleteBackward(); return TRUE;
    case GDK_KEY_Delete:
    case GDK_KEY_KP_Delete: doc->deleteForward(); return TRUE;
    case GDK_KEY_Left:
    case GDK_KEY_Right: {
        const int step = keyval == GDK_KEY_Left ? -1 : 1;
        if (!doc->hasSelection()) {
            doc->setSelection({0, 0});
            return TRUE;
        }
        const Span sel = doc->selection();
        if (shift) {
            // Extend from the anchor set when the selection began.
            const int anchor = self->m_anchor;
            int head = sel.start == anchor ? sel.end() : sel.start;
            head = std::clamp(head + step, 0, n);
            doc->setSelection({std::min(anchor, head), std::abs(head - anchor)});
        } else {
            int pos = sel.length > 0 ? (step < 0 ? sel.start : sel.end()) : sel.start + step;
            pos = std::clamp(pos, 0, n);
            self->m_anchor = pos;
            doc->setSelection({pos, 0});
        }
        self->scrollToPosition(doc->selection().start);
        return TRUE;
    }
    default: break;
    }
    const gunichar c = gdk_keyval_to_unicode(keyval);
    if (c > 0 && c < 128 && isIupacBase(char(c)) && c != '-') {
        const char text[2] = {char(c), 0};
        if (!doc->hasSelection())
            doc->setSelection({n, 0});
        doc->typeBases(text);
        self->m_anchor = doc->selection().start;
        self->scrollToPosition(doc->selection().start);
        return TRUE;
    }
    return FALSE;
}

void SequenceView::dragBegin(GtkGestureDrag *g, double x, double y, gpointer p)
{
    auto *self = static_cast<SequenceView *>(p);
    gtk_widget_grab_focus(self->m_area);
    if (!self->m_layout || self->m_doc->isEmpty())
        return;
    self->m_x0 = self->m_lastX = x;
    self->m_y0 = self->m_lastY = y;
    const view::Point content{x, y + self->scrollY()};
    const GdkModifierType mods = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
    if (mods & GDK_SHIFT_MASK) {
        // Shift-click extends from the existing anchor.
        if (!self->m_doc->hasSelection())
            self->m_anchor = 0;
        self->m_dragging = true;
        self->extendSelectionTo(x, y);
        return;
    }
    const int site = self->m_layout->siteAt(content);
    if (site >= 0) {
        self->m_doc->activateSite(site);
        return;
    }
    const int primer = self->m_layout->primerAt(content);
    if (primer >= 0) {
        const Range b = self->m_doc->sequence().primers[size_t(primer)].binding;
        self->m_anchor = b.start;
        self->m_doc->setSelection({b.start, b.length()});
        return;
    }
    const int feature = self->m_layout->featureAt(content);
    if (feature >= 0) {
        self->m_doc->activateFeature(feature);
        return;
    }
    self->m_dragging = true;
    self->m_anchor = self->m_layout->positionAt(content);
    self->m_doc->setSelection({self->m_anchor, 0});
}

gboolean SequenceView::autoScroll(gpointer p)
{
    auto *self = static_cast<SequenceView *>(p);
    if (!self->m_dragging) {
        self->m_timer = 0;
        return G_SOURCE_REMOVE;
    }
    double step = 0;
    if (self->m_lastY < 0)
        step = std::max(-60.0, self->m_lastY);
    else if (self->m_lastY > self->m_h)
        step = std::min(60.0, self->m_lastY - self->m_h);
    if (step == 0) {
        self->m_timer = 0;
        return G_SOURCE_REMOVE;
    }
    gtk_adjustment_set_value(self->m_adj, self->scrollY() + step);
    self->extendSelectionTo(self->m_lastX, std::clamp(self->m_lastY, 0.0, double(self->m_h)));
    return G_SOURCE_CONTINUE;
}

void SequenceView::dragUpdate(GtkGestureDrag *, double dx, double dy, gpointer p)
{
    auto *self = static_cast<SequenceView *>(p);
    if (!self->m_dragging)
        return;
    self->m_lastX = self->m_x0 + dx;
    self->m_lastY = self->m_y0 + dy;
    self->extendSelectionTo(self->m_lastX, std::clamp(self->m_lastY, 0.0, double(self->m_h)));
    if ((self->m_lastY < 0 || self->m_lastY > self->m_h) && !self->m_timer)
        self->m_timer = g_timeout_add(30, &SequenceView::autoScroll, self);
}

void SequenceView::dragEnd(GtkGestureDrag *, double, double, gpointer p)
{
    static_cast<SequenceView *>(p)->m_dragging = false;
}

gboolean SequenceView::queryTooltip(GtkWidget *, int x, int y, gboolean, GtkTooltip *tip, gpointer p)
{
    auto *self = static_cast<SequenceView *>(p);
    if (!self->m_layout)
        return FALSE;
    const std::string text = self->m_layout->tooltipAt({double(x), y + self->scrollY()});
    if (text.empty())
        return FALSE;
    gtk_tooltip_set_text(tip, text.c_str());
    return TRUE;
}

} // namespace oc::gtk
