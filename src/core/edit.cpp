// SPDX-License-Identifier: GPL-3.0-or-later
#include "edit.h"

#include <algorithm>

namespace oc {

namespace {

bool crossesOrigin(const Feature &f, int n)
{
    bool atEnd = false, atStart = false;
    for (const Range &r : f.ranges) {
        atEnd = atEnd || r.end == n;
        atStart = atStart || r.start == 0;
    }
    return atEnd && atStart;
}

// Joins pieces that touch (end == start), keeping top-strand order.
void mergeTouching(std::vector<Range> &ranges)
{
    for (size_t i = 0; i < ranges.size(); ++i)
        for (size_t j = 0; j < ranges.size(); ++j)
            if (i != j && ranges[i].end == ranges[j].start) {
                ranges[i].end = ranges[j].end;
                ranges.erase(ranges.begin() + long(j));
                i = size_t(-1);
                break;
            }
}

// Deletes [d0, d1) from a sequence that doesn't wrap.
void deleteRange(Sequence &seq, int d0, int d1)
{
    const int len = d1 - d0;
    if (len <= 0)
        return;
    auto map = [&](int p) { return p <= d0 ? p : p >= d1 ? p - len : d0; };
    seq.bases.erase(size_t(d0), size_t(len));
    for (Feature &f : seq.features) {
        std::vector<Range> kept;
        for (const Range &r : f.ranges) {
            Range m{map(r.start), map(r.end)};
            if (m.length() > 0)
                kept.push_back(m);
        }
        mergeTouching(kept);
        f.ranges = kept;
    }
    seq.features.erase(std::remove_if(seq.features.begin(), seq.features.end(),
                                      [](const Feature &f) { return f.ranges.empty(); }),
                       seq.features.end());
    std::vector<Primer> primers;
    for (Primer p : seq.primers) {
        if (p.binding.start < d1 && d0 < p.binding.end)
            continue;
        p.binding = {map(p.binding.start), map(p.binding.end)};
        primers.push_back(p);
    }
    seq.primers = primers;
}

} // namespace

void sortRangesTopStrand(std::vector<Range> &ranges, int n, bool circular)
{
    Feature tmp;
    tmp.ranges = ranges;
    const Span s = tmp.span(n, circular);
    std::sort(ranges.begin(), ranges.end(), [&](const Range &x, const Range &y) {
        auto key = [&](const Range &r) { return n > 0 ? ((r.start - s.start) % n + n) % n : r.start; };
        return key(x) < key(y);
    });
}

void insertBases(Sequence &seq, int pos, const std::string &bases)
{
    const int n = seq.length();
    const int k = int(bases.size());
    pos = std::clamp(pos, 0, n);
    if (k == 0)
        return;
    seq.bases.insert(size_t(pos), bases);
    for (Feature &f : seq.features) {
        const bool wraps = seq.circular && crossesOrigin(f, n);
        for (Range &r : f.ranges) {
            if (wraps && pos == 0 && r.start == 0)
                r.end += k;   // bases at the origin land inside the feature
            else if (pos <= r.start)
                r = {r.start + k, r.end + k};
            else if (pos < r.end)
                r.end += k;
        }
    }
    std::vector<Primer> primers;
    for (Primer p : seq.primers) {
        if (pos > p.binding.start && pos < p.binding.end)
            continue;
        if (pos <= p.binding.start)
            p.binding = {p.binding.start + k, p.binding.end + k};
        primers.push_back(p);
    }
    seq.primers = primers;
}

void deleteBases(Sequence &seq, Span span)
{
    const int n = seq.length();
    if (n == 0 || span.length <= 0)
        return;
    span.length = std::min(span.length, n);
    if (seq.circular) {
        span.start = ((span.start % n) + n) % n;
        if (span.start + span.length > n) {
            const int tail = span.start + span.length - n;
            deleteRange(seq, span.start, n);
            deleteRange(seq, 0, tail);
            return;
        }
    }
    span.start = std::clamp(span.start, 0, n);
    deleteRange(seq, span.start, std::min(n, span.start + span.length));
}

int replaceBases(Sequence &seq, Span span, const std::string &bases)
{
    const int n = seq.length();
    int pos = n > 0 ? std::clamp(span.start, 0, n) : 0;
    if (span.length > 0) {
        if (seq.circular && n > 0 && pos + span.length > n)
            pos -= pos + span.length - n;   // the head before the origin went too
        deleteBases(seq, span);
        pos = std::clamp(pos, 0, seq.length());
    }
    insertBases(seq, pos, bases);
    return pos;
}

void reverseComplementSequence(Sequence &seq)
{
    const int n = seq.length();
    seq.bases = reverseComplement(seq.bases);
    auto flip = [](Strand s) {
        return s == Strand::Forward ? Strand::Reverse : s == Strand::Reverse ? Strand::Forward : s;
    };
    for (Feature &f : seq.features) {
        for (Range &r : f.ranges)
            r = {n - r.end, n - r.start};
        std::reverse(f.ranges.begin(), f.ranges.end());
        f.strand = flip(f.strand);
    }
    for (Primer &p : seq.primers) {
        p.binding = {n - p.binding.end, n - p.binding.start};
        p.strand = flip(p.strand);
    }
}

void rotateOrigin(Sequence &seq, int origin)
{
    const int n = seq.length();
    if (!seq.circular || n == 0)
        return;
    origin = ((origin % n) + n) % n;
    if (origin == 0)
        return;
    seq.bases = seq.bases.substr(size_t(origin)) + seq.bases.substr(0, size_t(origin));
    for (Feature &f : seq.features) {
        std::vector<Range> out;
        for (const Range &r : f.ranges) {
            const int s = ((r.start - origin) % n + n) % n;
            const int e = s + r.length();
            if (e > n) {
                out.push_back({s, n});
                out.push_back({0, e - n});
            } else {
                out.push_back({s, e});
            }
        }
        mergeTouching(out);
        sortRangesTopStrand(out, n, true);
        f.ranges = out;
    }
    for (Primer &p : seq.primers) {
        const int s = ((p.binding.start - origin) % n + n) % n;
        p.binding = {s, std::min(n, s + p.binding.length())};
    }
}

} // namespace oc
