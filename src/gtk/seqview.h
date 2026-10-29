// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "view/cairo_render.h"
#include "view/document.h"
#include "view/seqlayout.h"

#include <gtk/gtk.h>
#include <functional>
#include <memory>
#include <set>
#include <string>

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
    int frames() const { return m_frames; }
    // A small gap after every 10 bases.
    void setGroupTens(bool group);
    bool groupTens() const { return m_groupTens; }
    // As on the map: enzyme labels, and the feature types left out.
    void setShowEnzymes(bool show);
    void setHiddenTypes(std::set<std::string> types);
    void scrollToPosition(int pos);
    // Typing bases, Backspace, Delete and the arrow keys edit the document.
    void setEditable(bool editable) { m_editable = editable; }
    // Room on the right for something floating over the view (the lines wrap
    // short of it; the background still fills the view).
    void setTrailingInset(int px);
    // Room to scroll the last line clear of something floating at the bottom.
    void setBottomInset(int px);
    // The width of the view's own scrollbar, for floating things to stay clear of.
    int scrollbarWidth() const;

    // The base under the mouse (-1 when none); the mouse, in widget
    // coordinates, while a drag selects bases, and -1, -1 when it ends; a
    // feature double-clicked.
    std::function<void(int)> onHoverBase;
    std::function<void(double, double)> onSelectDrag;
    std::function<void(int)> onEditFeature;

private:
    static void draw(GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer self);
    static void resized(GtkDrawingArea *, int w, int h, gpointer self);
    static gboolean scrolled(GtkEventControllerScroll *, double dx, double dy, gpointer self);
    static void scrollEnd(GtkEventControllerScroll *, gpointer self);
    static void decelerate(GtkEventControllerScroll *, double vx, double vy, gpointer self);
    static gboolean tick(GtkWidget *, GdkFrameClock *clock, gpointer self);
    static void valueChanged(GtkAdjustment *, gpointer self);
    static void dragBegin(GtkGestureDrag *, double x, double y, gpointer self);
    static void dragUpdate(GtkGestureDrag *, double dx, double dy, gpointer self);
    static void dragEnd(GtkGestureDrag *, double dx, double dy, gpointer self);
    static gboolean autoScroll(gpointer self);
    static gboolean keyPressed(GtkEventControllerKey *, guint keyval, guint keycode, GdkModifierType state,
                               gpointer self);
    static gboolean queryTooltip(GtkWidget *, int x, int y, gboolean kb, GtkTooltip *tip, gpointer self);
    static void motion(GtkEventControllerMotion *, double x, double y, gpointer self);
    static void leave(GtkEventControllerMotion *, gpointer self);
    static void pressed(GtkGestureClick *, int n, double x, double y, gpointer self);
    void ensureLayout();
    void updateAdjustment();
    void extendSelectionTo(double viewX, double viewY);
    void setHoverCodon(int codon);
    double scrollY() const { return m_offset; }
    // Scrolling. The offset can run past either end while a touchpad pulls
    // it (the rubber band); a tick callback runs the momentum after a flick,
    // the spring back, and the eased wheel and jump scrolls.
    enum class Motion { None, Momentum, Spring, Ease };
    double maxOffset() const;
    double overshoot() const;   // < 0 past the top, > 0 past the bottom
    void setOffset(double y);   // now, stopping any motion
    void syncOffset();          // the scrollbar and the drawing follow m_offset
    void scrollSmoothlyTo(double y);
    void startMotion(Motion motion);
    void stopMotion();
    void relayoutSoon();
    void relayoutKeepingPlace();

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
    bool m_enzymes = true;
    int m_bottomInset = 0;
    int m_trailing = 0;
    std::set<std::string> m_hiddenTypes;
    bool m_editable = false;
    // drag state
    bool m_dragging = false;
    int m_anchor = 0;
    // A drag that started on an amino acid: its codon, so the selection grows
    // codon by codon while it stays on amino acids (-1 otherwise).
    int m_codonAnchor = -1;
    // The codon whose amino acid is under the mouse, outlined on the strands.
    int m_hoverCodon = -1;
    double m_x0 = 0, m_y0 = 0, m_lastX = 0, m_lastY = 0;
    guint m_timer = 0;
    // scroll state
    double m_offset = 0, m_velocity = 0, m_target = 0;
    Motion m_motion = Motion::None;
    guint m_tickId = 0;
    gint64 m_lastFrame = 0;
    guint m_releaseTimer = 0;   // springs back when a touchpad stops without telling
    guint m_relayoutTimer = 0;  // while the width keeps changing, the old layout is drawn
    bool m_syncing = false;
};

} // namespace oc::gtk
