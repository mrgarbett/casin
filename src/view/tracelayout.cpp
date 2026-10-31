// SPDX-License-Identifier: GPL-3.0-or-later
#include "tracelayout.h"

#include <algorithm>
#include <string>

namespace oc::view {

namespace {
constexpr double kLetters = 16;    // baseline of the base letters
constexpr double kNumbers = 30;    // baseline of the position numbers
constexpr double kQualTop = 36;    // quality bars hang from here
constexpr double kQualH = 22;
constexpr double kTraceTop = 64;
} // namespace

Color TraceLayout::channelColor(int channel, bool dark)
{
    switch (channel) {
    case 0: return Color::rgb(0x26, 0xa2, 0x69);                                  // A green
    case 1: return dark ? Color::rgb(0x62, 0xa0, 0xea) : Color::rgb(0x1c, 0x71, 0xd8);   // C blue
    case 2: return dark ? Color::rgb(0xde, 0xdd, 0xda) : Color::rgb(0x24, 0x1f, 0x31);   // G black
    default: return Color::rgb(0xe0, 0x1b, 0x24);                                 // T red
    }
}

TraceLayout::TraceLayout(const Trace &trace, const Theme &theme, double height, double pixelsPerScan)
    : m_trace(trace), m_theme(theme), m_h(height), m_px(std::max(pixelsPerScan, 0.01))
{
    // Scale to a high percentile of the peak heights so one huge peak
    // doesn't flatten the rest.
    std::vector<int> tops;
    for (size_t i = 0; i < trace.peaks.size(); ++i)
        for (const auto &ch : trace.channels)
            if (size_t(trace.peaks[i]) < ch.size())
                tops.push_back(ch[size_t(trace.peaks[i])]);
    int ref = 1000;
    if (!tops.empty()) {
        std::sort(tops.begin(), tops.end());
        ref = std::max(1, tops[tops.size() * 98 / 100]);
    }
    m_scale = std::max(10.0, m_h - kTraceTop - 6) / ref;
    m_good = goodRegion(trace);
}

double TraceLayout::totalWidth() const
{
    return std::max(1, m_trace.scans()) * m_px;
}

double TraceLayout::xOfBase(int base) const
{
    if (base < 0 || base >= int(m_trace.peaks.size()))
        return 0;
    return m_trace.peaks[size_t(base)] * m_px;
}

int TraceLayout::baseAt(double x) const
{
    const std::vector<int> &p = m_trace.peaks;
    if (p.empty())
        return -1;
    const double scan = x / m_px;
    auto it = std::lower_bound(p.begin(), p.end(), scan);
    int i = int(it - p.begin());
    if (i > 0 && (i == int(p.size()) || scan - p[size_t(i - 1)] < p[size_t(i)] - scan))
        --i;
    return i;
}

Scene TraceLayout::render(double scrollX, double viewWidth, Span selection) const
{
    Scene s;
    s.width = viewWidth;
    s.height = m_h;
    s.background = m_theme.background;
    const int scans = m_trace.scans();
    const int first = std::max(0, int(scrollX / m_px) - 2);
    const int last = std::min(scans - 1, int((scrollX + viewWidth) / m_px) + 2);
    auto X = [&](double scan) { return scan * m_px - scrollX; };
    const double base = m_h - 4;

    // Selection and the low-quality ends.
    const std::vector<int> &peaks = m_trace.peaks;
    auto spanX = [&](int a, int b) {   // bases [a, b)
        const double x0 = a <= 0 ? 0 : (peaks[size_t(a - 1)] + peaks[size_t(a)]) / 2.0;
        const double x1 = b >= int(peaks.size()) ? scans : (peaks[size_t(b - 1)] + peaks[size_t(b)]) / 2.0;
        return std::make_pair(X(x0), X(x1));
    };
    if (!peaks.empty()) {
        Color dim = m_theme.mutedText;
        dim.a = 0.10;
        if (m_good.start > 0) {
            auto [a, b] = spanX(0, m_good.start);
            s.rect({a, 0, b - a, m_h}, dim);
        }
        if (m_good.start + m_good.length < int(peaks.size())) {
            auto [a, b] = spanX(m_good.start + m_good.length, int(peaks.size()));
            s.rect({a, 0, b - a, m_h}, dim);
        }
        const int selA = std::max(0, selection.start);
        const int selB = std::min<int>(int(peaks.size()), selection.start + std::max(0, selection.length));
        if (selB > selA) {
            auto [a, b] = spanX(selA, selB);
            s.rect({a, 0, b - a, m_h}, m_theme.selection);
        }
    }

    // Channels.
    for (int k = 0; k < 4; ++k) {
        const std::vector<int> &ch = m_trace.channels[size_t(k)];
        if (ch.empty() || first > last)
            continue;
        PathItem p;
        p.stroke = channelColor(k, m_theme.dark);
        p.lineWidth = 1.2;
        for (int i = first; i <= last && i < int(ch.size()); ++i)
            p.points.push_back({X(i), base - std::min(m_h - kTraceTop, ch[size_t(i)] * m_scale)});
        s.path(std::move(p));
    }

    // Base calls, quality and numbers.
    Font letter{12, true, true};
    Font small{9, false, false};
    for (size_t i = 0; i < m_trace.bases.size() && i < peaks.size(); ++i) {
        const double x = X(peaks[i]);
        if (x < -10 || x > viewWidth + 10)
            continue;
        const char b = m_trace.bases[i];
        const size_t slot = std::string("ACGT").find(b);
        const Color c = slot == std::string::npos ? m_theme.mutedText : channelColor(int(slot), m_theme.dark);
        s.label(std::string(1, b), {x, kLetters}, letter, c, HAlign::Center);
        if (!m_trace.quality.empty()) {
            const int q = m_trace.quality[i];
            Color qc = q >= 20 ? m_theme.accent : m_theme.mutedText;
            qc.a = 0.45;
            const double h = std::min(1.0, q / 60.0) * kQualH;
            s.rect({x - 2.5 * m_px, kQualTop, 5 * m_px, h}, qc);
        }
        if ((i + 1) % 10 == 0)
            s.label(std::to_string(i + 1), {x, kNumbers}, small, m_theme.mutedText, HAlign::Center);
    }
    s.line({0, m_h - 0.5}, {viewWidth, m_h - 0.5}, m_theme.tick);
    return s;
}

} // namespace oc::view
