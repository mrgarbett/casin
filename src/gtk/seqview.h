// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "view/cairo_render.h"
#include "view/document.h"
#include "view/seqlayout.h"

#include <gtk/gtk.h>
#include <memory>

namespace oc::gtk {

// Double-strand sequence view: a drawing area plus its own scrollbar (so the
// drawing surface stays viewport-sized however long the sequence is).
class SequenceView {
public:
    explicit SequenceView(Document *doc);
    ~SequenceView();
    GtkWidget *widget() const { return m_box; }
    void invalidate();
    void setDark(bool dark);
    void setShowComplement(bool show);
    bool showComplement() const { return m_complement; }
    // 0, 3 (forward) or 6 reading frames translated around the strands.
    void setFrames(int frames);
    // A small gap after every 10 bases.
    void setGroupTens(bool group);
    void scrollToPosition(int pos);
    // Typing bases, Backspace, Delete and the arrow keys edit the document.
    void setEditable(bool editable) { m_editable = editable; }

private:
    static void draw(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer self);
    static void resized(GtkDrawingArea *, int w, int h, gpointer self);
    static gboolean scrolled(GtkEventControllerScroll *, double dx, double dy, gpointer self);
    static void valueChanged(GtkAdjustment *, gpointer self);
    static void dragBegin(GtkGestureDrag *, double x, double y, gpointer self);
    static void dragUpdate(GtkGestureDrag *, double dx, double dy, gpointer self);
    static void dragEnd(GtkGestureDrag *, double dx, double dy, gpointer self);
    static gboolean autoScroll(gpointer self);
    static gboolean keyPressed(GtkEventControllerKey *, guint keyval, guint keycode, GdkModifierType state,
                               gpointer self);
    static gboolean queryTooltip(GtkWidget *, int x, int y, gboolean kb, GtkTooltip *tip, gpointer self);
    void ensureLayout();
    void updateAdjustment();
    void extendSelectionTo(double viewX, double viewY);
    double scrollY() const;

    Document *m_doc;
    GtkWidget *m_box, *m_area, *m_scrollbar;
    GtkAdjustment *m_adj;
    view::PangoTextMeasurer m_measure;
    std::unique_ptr<view::SequenceLayout> m_layout;
    int m_w = 0, m_h = 0;
    bool m_dark = false;
    bool m_complement = true;
    int m_frames = 0;
    bool m_groupTens = false;
    bool m_editable = false;
    // drag state
    bool m_dragging = false;
    int m_anchor = 0;
    double m_x0 = 0, m_y0 = 0, m_lastX = 0, m_lastY = 0;
    guint m_timer = 0;
};

} // namespace oc::gtk
