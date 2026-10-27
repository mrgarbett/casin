// SPDX-License-Identifier: GPL-3.0-or-later
#include "printlayout.h"

#include <variant>

namespace oc::view {

namespace {

const double kHeaderH = 28;

// Moves a scene's items down by dy and adds them to `page`.
void appendShifted(Scene &page, const Scene &part, double dy)
{
    for (Item it : part.items) {
        if (auto *p = std::get_if<PathItem>(&it))
            for (Point &pt : p->points)
                pt.y += dy;
        else if (auto *t = std::get_if<TextItem>(&it))
            t->pos.y += dy;
        page.items.push_back(std::move(it));
    }
}

Scene newPage(const PrintSettings &ps, const Theme &theme)
{
    Scene s;
    s.width = ps.width;
    s.height = ps.height;
    s.background = theme.background;
    return s;
}

} // namespace

std::vector<Scene> printPages(const oc::Sequence &seq, const std::vector<CutSite> &sites,
                              const std::vector<ReadTrack> &reads, TextMeasurer &tm, const PrintSettings &ps)
{
    const Theme theme = Theme::light();
    std::vector<Scene> pages;
    const double bodyH = ps.height - kHeaderH;
    if (ps.map) {
        Scene page = newPage(ps, theme);
        MapLayout map(seq, sites, theme, tm, ps.width, std::min(bodyH, ps.width * 1.1), ps.mapOptions);
        appendShifted(page, map.render({}), kHeaderH);
        pages.push_back(std::move(page));
    }
    if (ps.sequence && !seq.empty()) {
        SequenceLayout layout(seq, sites, theme, tm, ps.width, ps.complement, ps.frames, reads);
        const int bpl = layout.basesPerLine();
        double y0 = 0;
        int pos = 0;
        while (pos < seq.length()) {
            // Whole lines that fit; at least one per page.
            double y1 = layout.lineTopFor(pos) + layout.lineHeightFor(pos);
            int next = pos + bpl;
            while (next < seq.length() && layout.lineTopFor(next) + layout.lineHeightFor(next) - y0 <= bodyH) {
                y1 = layout.lineTopFor(next) + layout.lineHeightFor(next);
                next += bpl;
            }
            Scene page = newPage(ps, theme);
            Scene part = layout.render(y0, y1 - y0 - 0.01, {});
            appendShifted(page, part, kHeaderH);
            pages.push_back(std::move(page));
            y0 = y1;
            pos = next;
        }
    }
    const Font headerFont{10, false, false};
    for (size_t i = 0; i < pages.size(); ++i) {
        pages[i].label(seq.name + "  ·  " + formatBp(seq.length()) + (seq.circular ? " circular" : " linear"),
                       {0, kHeaderH / 2}, headerFont, theme.mutedText, HAlign::Left, VAlign::Middle);
        pages[i].label(std::to_string(i + 1) + " / " + std::to_string(pages.size()), {ps.width, kHeaderH / 2},
                       headerFont, theme.mutedText, HAlign::Right, VAlign::Middle);
        pages[i].line({0, kHeaderH - 6}, {ps.width, kHeaderH - 6}, theme.tick.withAlpha(0.5), 0.6);
    }
    return pages;
}

} // namespace oc::view
