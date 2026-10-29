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
    g_signal_connect(scroll, "scroll-end", G_CALLBACK(&SequenceView::scrollEnd), this);
    g_signal_connect(scroll, "decelerate", G_CALLBACK(&SequenceView::decelerate), this);
    gtk_widget_add_controller(m_area, scroll);

    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), GDK_BUTTON_PRIMARY);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(&SequenceView::dragBegin), this);
    g_signal_connect(drag, "drag-update", G_CALLBACK(&SequenceView::dragUpdate), this);
    g_signal_connect(drag, "drag-end", G_CALLBACK(&SequenceView::dragEnd), this);
    gtk_widget_add_controller(m_area, GTK_EVENT_CONTROLLER(drag));

    GtkEventController *move = gtk_event_controller_motion_new();
    g_signal_connect(move, "motion", G_CALLBACK(&SequenceView::motion), this);
    g_signal_connect(move, "leave", G_CALLBACK(&SequenceView::leave), this);
    gtk_widget_add_controller(m_area, move);
    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(&SequenceView::pressed), this);
    gtk_widget_add_controller(m_area, GTK_EVENT_CONTROLLER(click));
    // Right-click: Add Primer (from the selection, if any), as on the Mac.
    GtkGesture *menu = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(menu), GDK_BUTTON_SECONDARY);
    g_signal_connect(menu, "pressed", G_CALLBACK((+[](GtkGestureClick *, int, double x, double y, gpointer p) {
                         auto *self = static_cast<SequenceView *>(p);
                         if (self->m_doc->isEmpty())
                             return;
                         const bool sel = self->m_doc->hasSelection() && self->m_doc->selection().length > 0;
                         GMenu *model = g_menu_new();
                         g_menu_append(model, sel ? "Add Primer from Selection…" : "Add Primer…", "win.add-primer");
                         if (sel)
                             g_menu_append(model, "Copy", "win.copy");
                         GtkWidget *pop = gtk_popover_menu_new_from_model(G_MENU_MODEL(model));
                         g_object_unref(model);
                         gtk_widget_set_parent(pop, self->m_area);
                         gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
                         const GdkRectangle at = {int(x), int(y), 1, 1};
                         gtk_popover_set_pointing_to(GTK_POPOVER(pop), &at);
                         g_signal_connect(pop, "closed", G_CALLBACK(+[](GtkPopover *pop) {
                                              // Unparent once it has finished closing.
                                              g_idle_add_once([](gpointer w) { gtk_widget_unparent(GTK_WIDGET(w)); },
                                                              pop);
                                          }),
                                          nullptr);
                         gtk_popover_popup(GTK_POPOVER(pop));
                     })),
                     this);
    gtk_widget_add_controller(m_area, GTK_EVENT_CONTROLLER(menu));

    gtk_widget_set_focusable(m_area, TRUE);
    gtk_widget_set_focus_on_click(m_area, TRUE);
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(&SequenceView::keyPressed), this);
    gtk_widget_add_controller(m_area, keys);

    gtk_widget_set_has_tooltip(m_area, TRUE);
    g_signal_connect(m_area, "query-tooltip", G_CALLBACK(&SequenceView::queryTooltip), this);

    m_doc->onSequenceChanged([this] {
        if (!m_doc->isEditing())
            setOffset(0);   // a newly loaded sequence starts at the top
        invalidate();
    });
    m_doc->onEnzymesChanged([this] { invalidate(); });
    // A primer being added draws as if it were on the sequence.
    m_doc->onPendingChanged([this] { invalidate(); });
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
    if (m_releaseTimer)
        g_source_remove(m_releaseTimer);
    if (m_relayoutTimer)
        g_source_remove(m_relayoutTimer);
    if (m_tickId)
        gtk_widget_remove_tick_callback(m_area, m_tickId);
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

void SequenceView::setGroupTens(bool group)
{
    if (group != m_groupTens) {
        m_groupTens = group;
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

void SequenceView::setShowEnzymes(bool show)
{
    if (show != m_enzymes) {
        m_enzymes = show;
        invalidate();
    }
}

void SequenceView::setHiddenTypes(std::set<std::string> types)
{
    if (types != m_hiddenTypes) {
        m_hiddenTypes = std::move(types);
        invalidate();
    }
}

void SequenceView::ensureLayout()
{
    if (m_layout || m_w <= 0)
        return;
    view::SeqOptions options;
    options.frames = m_frames;
    options.groupTens = m_groupTens;
    options.enzymes = m_enzymes;
    options.hiddenTypes = m_hiddenTypes;
    m_layout = std::make_unique<view::SequenceLayout>(m_doc->displaySequence(), m_doc->visibleSites(),
                                                      m_dark ? view::Theme::darkTheme() : view::Theme::light(),
                                                      m_measure, std::max(200, int((m_w - m_trailing) / m_zoom)), m_complement,
                                                      options, m_doc->reads());
    updateAdjustment();
}

void SequenceView::updateAdjustment()
{
    const double total = m_layout ? (m_layout->totalHeight() + 20) * m_zoom + m_bottomInset : 0;
    const double page = std::max(1, m_h);
    m_syncing = true;
    gtk_adjustment_configure(m_adj, std::clamp(m_offset, 0.0, std::max(0.0, total - page)), 0,
                             std::max(total, page), 40, page * 0.9, page);
    m_syncing = false;
    // Past an end only while the rubber band or a spring is moving it.
    if (m_motion == Motion::None && overshoot() != 0)
        setOffset(std::clamp(m_offset, 0.0, maxOffset()));
}

void SequenceView::scrollToPosition(int pos)
{
    ensureLayout();
    if (!m_layout)
        return;
    const double top = m_layout->lineTopFor(pos) * m_zoom, h = m_layout->lineHeightFor(pos) * m_zoom;
    const double y = m_motion == Motion::Ease ? m_target : m_offset;
    if (top < y || top + h > y + m_h)
        scrollSmoothlyTo(top - 20);
}

// ---- scrolling ---------------------------------------------------------------------

namespace {
// Momentum after a flick: the velocity keeps 0.998 of itself each millisecond.
const double kDecayPerMs = 0.998;
// The spring that pulls the view back from past an end: critically damped,
// settling in about a quarter of a second.
const double kSpring = 220, kDamping = 2 * std::sqrt(kSpring);
// Eased wheel and jump scrolls close the gap with this time constant (s).
const double kEase = 0.07;

bool animationsEnabled(GtkWidget *w)
{
    gboolean on = TRUE;
    g_object_get(gtk_widget_get_settings(w), "gtk-enable-animations", &on, nullptr);
    return on;
}
}

double SequenceView::maxOffset() const
{
    return std::max(0.0, gtk_adjustment_get_upper(m_adj) - gtk_adjustment_get_page_size(m_adj));
}

double SequenceView::overshoot() const
{
    if (m_offset < 0)
        return m_offset;
    const double max = maxOffset();
    return m_offset > max ? m_offset - max : 0;
}

void SequenceView::syncOffset()
{
    m_syncing = true;
    gtk_adjustment_set_value(m_adj, std::clamp(m_offset, 0.0, maxOffset()));
    m_syncing = false;
    gtk_widget_queue_draw(m_area);
}

void SequenceView::setOffset(double y)
{
    stopMotion();
    m_offset = y;
    syncOffset();
}

void SequenceView::scrollSmoothlyTo(double y)
{
    m_target = std::clamp(y, 0.0, maxOffset());
    if (!animationsEnabled(m_area) || !gtk_widget_get_mapped(m_area)) {
        setOffset(m_target);
        return;
    }
    startMotion(Motion::Ease);
}

void SequenceView::startMotion(Motion motion)
{
    m_motion = motion;
    if (!animationsEnabled(m_area) || !gtk_widget_get_mapped(m_area)) {
        // No animation: straight to where the motion would end.
        setOffset(motion == Motion::Ease ? m_target : std::clamp(m_offset, 0.0, maxOffset()));
        return;
    }
    if (!m_tickId) {
        m_lastFrame = gdk_frame_clock_get_frame_time(gtk_widget_get_frame_clock(m_area));
        m_tickId = gtk_widget_add_tick_callback(m_area, &SequenceView::tick, this, nullptr);
    }
}

void SequenceView::stopMotion()
{
    m_motion = Motion::None;
    m_velocity = 0;
    if (m_tickId) {
        gtk_widget_remove_tick_callback(m_area, m_tickId);
        m_tickId = 0;
    }
}

gboolean SequenceView::tick(GtkWidget *, GdkFrameClock *clock, gpointer p)
{
    auto *self = static_cast<SequenceView *>(p);
    const gint64 now = gdk_frame_clock_get_frame_time(clock);
    const double dt = std::clamp((now - self->m_lastFrame) / 1e6, 0.0, 0.05);
    self->m_lastFrame = now;
    switch (self->m_motion) {
    case Motion::Momentum:
        self->m_velocity *= std::pow(kDecayPerMs, dt * 1000);
        self->m_offset += self->m_velocity * dt;
        if (self->overshoot() != 0)
            self->m_motion = Motion::Spring;   // ran into an end: bounce back
        else if (std::abs(self->m_velocity) < 8)
            self->m_motion = Motion::None;
        break;
    case Motion::Spring: {
        const double bound = std::clamp(self->m_offset, 0.0, self->maxOffset());
        // Small steps keep the stiff spring stable at low frame rates.
        for (double left = dt; left > 0; left -= 0.004) {
            const double h = std::min(left, 0.004);
            const double x = self->m_offset - bound;
            self->m_velocity += (-kSpring * x - kDamping * self->m_velocity) * h;
            self->m_offset += self->m_velocity * h;
        }
        if (std::abs(self->m_offset - bound) < 0.5 && std::abs(self->m_velocity) < 10) {
            self->m_offset = bound;
            self->m_motion = Motion::None;
        }
        break;
    }
    case Motion::Ease:
        self->m_offset += (self->m_target - self->m_offset) * (1 - std::exp(-dt / kEase));
        if (std::abs(self->m_target - self->m_offset) < 0.5) {
            self->m_offset = self->m_target;
            self->m_motion = Motion::None;
        }
        break;
    case Motion::None:
        break;
    }
    self->syncOffset();
    if (self->m_motion != Motion::None)
        return G_SOURCE_CONTINUE;
    self->m_velocity = 0;
    self->m_tickId = 0;
    return G_SOURCE_REMOVE;
}

gboolean SequenceView::scrolled(GtkEventControllerScroll *c, double, double dy, gpointer p)
{
    auto *self = static_cast<SequenceView *>(p);
    if (gtk_event_controller_scroll_get_unit(c) != GDK_SCROLL_UNIT_SURFACE) {
        // A mouse wheel: eased steps, which add up while one is still moving.
        const double from = self->m_motion == Motion::Ease ? self->m_target : self->m_offset;
        self->scrollSmoothlyTo(from + dy * 50);
        return TRUE;
    }
    // A touchpad follows the fingers, with growing resistance past an end.
    self->stopMotion();
    double delta = dy;
    const double over = self->overshoot();
    if ((over < 0 && delta < 0) || (over > 0 && delta > 0))
        delta *= 0.45 / (1 + std::abs(over) / 40);
    self->m_offset += delta;
    self->syncOffset();
    // X11 never says when the fingers lift: spring back after a pause.
    if (self->m_releaseTimer)
        g_source_remove(self->m_releaseTimer);
    self->m_releaseTimer = g_timeout_add(150, [](gpointer p) -> gboolean {
        auto *self = static_cast<SequenceView *>(p);
        self->m_releaseTimer = 0;
        if (self->m_motion == Motion::None && self->overshoot() != 0)
            self->startMotion(Motion::Spring);
        return G_SOURCE_REMOVE;
    }, self);
    return TRUE;
}

void SequenceView::scrollEnd(GtkEventControllerScroll *, gpointer p)
{
    auto *self = static_cast<SequenceView *>(p);
    if (self->overshoot() != 0)
        self->startMotion(Motion::Spring);
}

void SequenceView::decelerate(GtkEventControllerScroll *c, double, double vy, gpointer p)
{
    auto *self = static_cast<SequenceView *>(p);
    if (gtk_event_controller_scroll_get_unit(c) != GDK_SCROLL_UNIT_SURFACE)
        return;
    // The fingers left with this speed: carry on and slow down, or bounce
    // back if already past an end.
    if (self->overshoot() != 0 || std::abs(vy) > 30) {
        self->m_velocity = self->overshoot() != 0 ? vy * 0.3 : vy;
        self->startMotion(self->overshoot() != 0 ? Motion::Spring : Motion::Momentum);
    }
}

// ---- layout ------------------------------------------------------------------------

void SequenceView::relayoutSoon()
{
    if (m_relayoutTimer)
        g_source_remove(m_relayoutTimer);
    m_relayoutTimer = g_timeout_add(120, [](gpointer p) -> gboolean {
        auto *self = static_cast<SequenceView *>(p);
        self->m_relayoutTimer = 0;
        self->relayoutKeepingPlace();
        return G_SOURCE_REMOVE;
    }, this);
}

void SequenceView::relayoutKeepingPlace()
{
    // The lines rewrap around a base that stays put: the caret's, when it is
    // in view, else the first one shown.
    int keep = -1;
    double below = 0;
    if (m_layout) {
        keep = m_layout->positionAt(content(0, 1));
        if (m_doc->hasSelection()) {
            const int caret = m_doc->selection().start;
            const double top = m_layout->lineTopFor(caret) * m_zoom;
            if (top >= m_offset && top + m_layout->lineHeightFor(caret) * m_zoom <= m_offset + m_h)
                keep = caret;
        }
        if (keep >= 0)
            below = m_layout->lineTopFor(keep) * m_zoom - m_offset;
    }
    invalidate();
    if (keep >= 0 && m_layout)
        setOffset(std::clamp(m_layout->lineTopFor(keep) * m_zoom - below, 0.0, maxOffset()));
}

void SequenceView::resized(GtkDrawingArea *, int w, int h, gpointer p)
{
    auto *self = static_cast<SequenceView *>(p);
    self->m_h = h;
    if (w != self->m_w) {
        self->m_w = w;
        // While the width keeps changing (a panel sliding, a window being
        // resized), the old layout is drawn and the new one waits for it to
        // settle.
        if (self->m_layout) {
            self->relayoutSoon();
            self->updateAdjustment();
            return;
        }
    }
    self->ensureLayout();
    self->updateAdjustment();
}

void SequenceView::draw(GtkDrawingArea *, cairo_t *cr, int, int h, gpointer p)
{
    auto *self = static_cast<SequenceView *>(p);
    self->ensureLayout();
    if (!self->m_layout)
        return;
    view::SeqViewState st;
    st.hasSelection = self->m_doc->hasSelection();
    st.selection = self->m_doc->selection();
    st.activeFeature = self->m_doc->activeFeature();
    st.activeSite = self->m_doc->activeSite();
    st.hoverCodon = self->m_hoverCodon;
    const double z = self->m_zoom;
    const view::Scene scene = self->m_layout->render(self->m_offset / z, h / z, st);
    if (!scene.background.isNone()) {
        // The whole width, so the pill floats over the sequence.
        const view::Color &c = scene.background;
        cairo_set_source_rgba(cr, c.r, c.g, c.b, c.a);
        cairo_paint(cr);
    }
    cairo_scale(cr, z, z);
    view::drawScene(cr, scene);
}

void SequenceView::valueChanged(GtkAdjustment *adj, gpointer p)
{
    auto *self = static_cast<SequenceView *>(p);
    if (!self->m_syncing) {
        // The scrollbar dragged: it leads.
        self->stopMotion();
        self->m_offset = gtk_adjustment_get_value(adj);
    }
    gtk_widget_queue_draw(self->m_area);
}

void SequenceView::setHoverCodon(int codon)
{
    if (codon != m_hoverCodon) {
        m_hoverCodon = codon;
        gtk_widget_queue_draw(m_area);
    }
}

void SequenceView::extendSelectionTo(double x, double y)
{
    if (!m_layout)
        return;
    const view::Point at = content(x, y);
    if (m_codonAnchor >= 0) {
        // From the first codon to the one under the mouse, both whole; off
        // the amino acids, to the nearest base.
        const int first = m_codonAnchor;
        const int codon = m_layout->codonAt(at);
        setHoverCodon(codon);
        const int end = codon >= 0 ? (codon >= first ? codon + 3 : codon) : m_layout->positionAt(at);
        const int start = std::min(first, end);
        const int stop = std::min(std::max(first + 3, end), m_doc->sequence().length());
        m_doc->setSelection({start, stop - start});
        return;
    }
    const int pos = m_layout->positionAt(at);
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
    const view::Point at = self->content(x, y);
    const GdkModifierType mods = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
    if (mods & GDK_SHIFT_MASK) {
        // Shift-click extends from the existing anchor.
        if (!self->m_doc->hasSelection())
            self->m_anchor = 0;
        self->m_dragging = true;
        self->extendSelectionTo(x, y);
        return;
    }
    // An amino acid in a translation row selects its codon; dragging on
    // across amino acids selects their codons too.
    const int codon = self->m_layout->codonAt(at);
    if (codon >= 0) {
        self->m_dragging = true;
        self->m_codonAnchor = self->m_anchor = codon;
        self->m_doc->setSelection({codon, std::min(3, self->m_doc->sequence().length() - codon)});
        return;
    }
    const int site = self->m_layout->siteAt(at);
    if (site >= 0) {
        self->m_doc->activateSite(site);
        return;
    }
    // The layout draws displaySequence(): the pending primer, if any, comes last.
    const int primer = self->m_layout->primerAt(at);
    if (primer >= 0 && primer < int(self->m_doc->displaySequence().primers.size())) {
        const Range b = self->m_doc->displaySequence().primers[size_t(primer)].binding;
        self->m_anchor = b.start;
        self->m_doc->setSelection({b.start, b.length()});
        return;
    }
    const int feature = self->m_layout->featureAt(at);
    if (feature >= 0) {
        self->m_doc->activateFeature(feature);
        return;
    }
    self->m_dragging = true;
    self->m_anchor = self->m_layout->positionAt(at);
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
    self->setOffset(std::clamp(self->m_offset + step, 0.0, self->maxOffset()));
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
    if (self->onSelectDrag)
        self->onSelectDrag(self->m_lastX, self->m_lastY);
    if ((self->m_lastY < 0 || self->m_lastY > self->m_h) && !self->m_timer)
        self->m_timer = g_timeout_add(30, &SequenceView::autoScroll, self);
}

void SequenceView::dragEnd(GtkGestureDrag *, double, double, gpointer p)
{
    auto *self = static_cast<SequenceView *>(p);
    if (self->m_dragging && self->onSelectDrag)
        self->onSelectDrag(-1, -1);
    self->m_dragging = false;
    self->m_codonAnchor = -1;
}

void SequenceView::motion(GtkEventControllerMotion *, double x, double y, gpointer p)
{
    auto *self = static_cast<SequenceView *>(p);
    if (!self->m_layout)
        return;
    const view::Point at = self->content(x, y);
    // An amino acid under the mouse outlines its codon.
    self->setHoverCodon(self->m_layout->codonAt(at));
    if (self->onHoverBase)
        self->onHoverBase(self->m_layout->baseAt(at));
}

void SequenceView::leave(GtkEventControllerMotion *, gpointer p)
{
    auto *self = static_cast<SequenceView *>(p);
    self->setHoverCodon(-1);
    if (self->onHoverBase)
        self->onHoverBase(-1);
}

void SequenceView::pressed(GtkGestureClick *, int n, double x, double y, gpointer p)
{
    auto *self = static_cast<SequenceView *>(p);
    if (n != 2 || !self->m_layout || !self->onEditFeature)
        return;
    const int feature = self->m_layout->featureAt(self->content(x, y));
    if (feature >= 0)
        self->onEditFeature(feature);
}

void SequenceView::setBottomInset(int px)
{
    m_bottomInset = px;
    updateAdjustment();
}

void SequenceView::setZoom(double zoom)
{
    if (zoom == m_zoom)
        return;
    // Keep the same base at the top of the view.
    const int keep = m_layout ? m_layout->positionAt(content(0, 1)) : -1;
    const double below = keep >= 0 ? m_layout->lineTopFor(keep) * m_zoom - m_offset : 0;
    m_zoom = zoom;
    invalidate();
    if (keep >= 0 && m_layout)
        setOffset(std::clamp(m_layout->lineTopFor(keep) * m_zoom - below, 0.0, maxOffset()));
}

void SequenceView::setTrailingInset(int px)
{
    if (px != m_trailing) {
        m_trailing = px;
        invalidate();
    }
}

int SequenceView::scrollbarWidth() const
{
    int natural = 0;
    gtk_widget_measure(m_scrollbar, GTK_ORIENTATION_HORIZONTAL, -1, nullptr, &natural, nullptr, nullptr);
    return natural;
}

gboolean SequenceView::queryTooltip(GtkWidget *, int x, int y, gboolean, GtkTooltip *tip, gpointer p)
{
    auto *self = static_cast<SequenceView *>(p);
    if (!self->m_layout)
        return FALSE;
    const std::string text = self->m_layout->tooltipAt(self->content(x, y));
    if (text.empty())
        return FALSE;
    gtk_tooltip_set_text(tip, text.c_str());
    return TRUE;
}

} // namespace oc::gtk
