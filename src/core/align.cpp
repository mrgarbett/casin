// SPDX-License-Identifier: GPL-3.0-or-later
#include "align.h"

#include <algorithm>
#include <cstdint>

namespace oc {

namespace {

constexpr int kMatch = 2, kMismatch = -3, kGapOpen = 7, kGapExtend = 2;   // a gap of k costs 7 + 2(k-1)

struct Local {
    int score = 0;
    std::string refAligned, readAligned;
    int refStart = 0;    // in the (possibly extended) reference string
    int readStart = 0, readEnd = 0;
};

bool same(char a, char b)
{
    return a == b && a != 'N';
}

// Smith-Waterman with affine gaps (Gotoh). Each cell's traceback byte holds
// where H came from (bits 0-1: 0 stop, 1 diagonal, 2 F, 3 E) and whether E
// (bit 2) and F (bit 3) extended an earlier gap rather than opening one.
Local smithWaterman(const std::string &ref, const std::string &read)
{
    const size_t n = ref.size(), m = read.size();
    constexpr int kNeg = -1000000;
    std::vector<int> hPrev(n + 1, 0), hCur(n + 1, 0), fPrev(n + 1, kNeg), fCur(n + 1, kNeg);
    std::vector<uint8_t> trace((m + 1) * (n + 1), 0);
    Local best;
    size_t bi = 0, bj = 0;
    for (size_t i = 1; i <= m; ++i) {
        hCur[0] = 0;
        int e = kNeg;
        for (size_t j = 1; j <= n; ++j) {
            uint8_t t = 0;
            // E: gap in the read (the reference advances).
            const int eOpen = hCur[j - 1] - kGapOpen, eExt = e - kGapExtend;
            e = std::max(eOpen, eExt);
            if (eExt > eOpen)
                t |= 4;
            // F: gap in the reference (the read advances).
            const int fOpen = hPrev[j] - kGapOpen, fExt = fPrev[j] - kGapExtend;
            fCur[j] = std::max(fOpen, fExt);
            if (fExt > fOpen)
                t |= 8;
            const int diag = hPrev[j - 1] + (same(read[i - 1], ref[j - 1]) ? kMatch : kMismatch);
            int h = 0;
            if (diag > h) { h = diag; t = uint8_t((t & 12) | 1); }
            if (fCur[j] > h) { h = fCur[j]; t = uint8_t((t & 12) | 2); }
            if (e > h) { h = e; t = uint8_t((t & 12) | 3); }
            hCur[j] = h;
            trace[i * (n + 1) + j] = t;
            if (h > best.score) {
                best.score = h;
                bi = i;
                bj = j;
            }
        }
        std::swap(hPrev, hCur);
        std::swap(fPrev, fCur);
    }
    size_t i = bi, j = bj;
    std::string ra, qa;
    int state = 0;   // 0 H, 1 F, 2 E
    while (i > 0 && j > 0) {
        const uint8_t t = trace[i * (n + 1) + j];
        if (state == 0) {
            const int from = t & 3;
            if (from == 0)
                break;
            if (from == 1) {
                ra += ref[j - 1];
                qa += read[i - 1];
                --i, --j;
            } else {
                state = from == 2 ? 1 : 2;
            }
        } else if (state == 1) {   // gap in the reference
            ra += '-';
            qa += read[i - 1];
            state = (t & 8) ? 1 : 0;
            --i;
        } else {                   // gap in the read
            ra += ref[j - 1];
            qa += '-';
            state = (t & 4) ? 2 : 0;
            --j;
        }
    }
    std::reverse(ra.begin(), ra.end());
    std::reverse(qa.begin(), qa.end());
    best.refAligned = ra;
    best.readAligned = qa;
    best.refStart = int(j);
    best.readStart = int(i);
    best.readEnd = int(bi);
    return best;
}

} // namespace

bool alignRead(const Sequence &reference, const std::string &rawRead, ReadAlignment *out, std::string *error,
               int minMatches)
{
    auto fail = [error](const char *msg) {
        if (error)
            *error = msg;
        return false;
    };
    const std::string read = cleanBases(rawRead);
    const int n = reference.length();
    if (read.empty() || n == 0)
        return fail("nothing to align");
    if (double(read.size()) * double(n + read.size()) > 60e6)
        return fail("the read or the reference is too long to align here");
    // On a circle, extend the reference so reads across the origin align in one piece.
    std::string ref = reference.bases;
    if (reference.circular)
        ref += reference.bases.substr(0, std::min(read.size(), reference.bases.size()));
    const Local fwd = smithWaterman(ref, read);
    const std::string rc = reverseComplement(read);
    const Local rev = smithWaterman(ref, rc);
    const bool useRev = rev.score > fwd.score;
    const Local &a = useRev ? rev : fwd;

    ReadAlignment r;
    r.reverse = useRev;
    r.refAligned = a.refAligned;
    r.readAligned = a.readAligned;
    int refLen = 0;
    for (size_t k = 0; k < a.refAligned.size(); ++k) {
        const char x = a.refAligned[k], y = a.readAligned[k];
        refLen += x != '-' ? 1 : 0;
        if (x == '-' || y == '-')
            ++r.gaps;
        else if (same(x, y))
            ++r.matches;
        else
            ++r.mismatches;
    }
    if (r.matches < minMatches)
        return fail("the read doesn't match this sequence");
    r.refSpan = {a.refStart % n, refLen};
    const int len = int(read.size());
    r.readStart = useRev ? len - a.readEnd : a.readStart;
    r.readEnd = useRev ? len - a.readStart : a.readEnd;
    // Differences, merging runs of gaps.
    int pos = a.refStart;
    for (size_t k = 0; k < a.refAligned.size();) {
        const char x = a.refAligned[k], y = a.readAligned[k];
        if (x != '-' && y != '-') {
            if (!same(x, y) && !(x == 'N' && y == 'N'))
                r.diffs.push_back({pos % n, std::string(1, x), std::string(1, y)});
            ++pos;
            ++k;
            continue;
        }
        AlignmentDiff d;
        d.refPos = pos % n;
        const bool insertion = x == '-';
        while (k < a.refAligned.size() && (insertion ? a.refAligned[k] == '-' : a.readAligned[k] == '-')) {
            if (insertion)
                d.read += a.readAligned[k];
            else {
                d.ref += a.refAligned[k];
                ++pos;
            }
            ++k;
        }
        r.diffs.push_back(d);
    }
    *out = std::move(r);
    return true;
}

ReadTrack readTrack(const ReadAlignment &a, int refLength, const std::string &name)
{
    ReadTrack t;
    t.name = name;
    t.reverse = a.reverse;
    t.refSpan = a.refSpan;
    for (size_t k = 0; k < a.refAligned.size() && k < a.readAligned.size(); ++k) {
        if (a.refAligned[k] == '-') {
            const int pos = a.refSpan.start + int(t.bases.size());
            t.insertions.push_back(refLength > 0 ? pos % refLength : pos);
        } else {
            t.bases += a.readAligned[k];
        }
    }
    return t;
}

} // namespace oc
