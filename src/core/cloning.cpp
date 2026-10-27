// SPDX-License-Identifier: GPL-3.0-or-later
#include "cloning.h"
#include "edit.h"

#include <algorithm>

namespace oc {

namespace {

// Overhang of a cut in sequence coordinates; on circular sequences the two
// strand cuts may sit on opposite sides of the origin.
int overhangOf(const CutSite &c, int n)
{
    int o = c.cutBottom - c.cutTop;
    if (o > n / 2)
        o -= n;
    else if (o < -n / 2)
        o += n;
    return o;
}

FragmentEnd endAt(const Sequence &seq, const CutSite &c)
{
    const int n = seq.length();
    FragmentEnd e;
    e.enzyme = c.enzyme ? c.enzyme->name : std::string();
    e.overhang = overhangOf(c, n);
    if (e.overhang != 0)
        e.bases = seq.slice(e.overhang > 0 ? c.cutTop : c.cutTop + e.overhang, std::abs(e.overhang));
    return e;
}

// Whether [s, s + len) lies inside the piece [start, start + pieceLen), on a
// sequence of length n (circular coordinates).
bool spanInside(int s, int len, int start, int pieceLen, int n)
{
    const int off = ((s - start) % n + n) % n;
    return off + len <= pieceLen;
}

} // namespace

int ClonePiece::originOffset() const
{
    if (sourceStart < 0 || sourceLength <= 0)
        return -1;
    if (sourceStart == 0)
        return 0;
    const int o = sourceLength - sourceStart;
    return o < length() ? o : -1;
}

bool cutPiece(const Sequence &seq, const CutSite &left, const CutSite &right, ClonePiece *out, std::string *error)
{
    auto fail = [error](const char *msg) {
        if (error)
            *error = msg;
        return false;
    };
    const int n = seq.length();
    if (n == 0)
        return fail("empty sequence");
    int len = right.cutTop - left.cutTop;
    int lenBottom = right.cutBottom - left.cutBottom;
    if (seq.circular) {
        if (len <= 0)
            len += n;
        if (lenBottom <= 0)
            lenBottom += n;
    } else if (len <= 0 || lenBottom <= 0) {
        return fail("the left cut must come before the right cut");
    }
    ClonePiece p;
    p.top = extractFragment(seq, {left.cutTop, len});
    p.bottom = extractFragment(seq, {left.cutBottom, lenBottom});
    reverseComplementSequence(p.bottom);
    p.left = endAt(seq, left);
    p.right = endAt(seq, right);
    p.sourceStart = ((left.cutTop % n) + n) % n;
    p.sourceLength = n;
    *out = p;
    return true;
}

ClonePiece flipPiece(const ClonePiece &p)
{
    ClonePiece f;
    f.sourceLength = p.sourceLength;
    f.sourceStart = -1;   // reversed: no longer maps onto the source
    f.top = p.bottom;
    f.bottom = p.top;
    f.left = {p.right.enzyme, p.right.overhang, reverseComplement(p.right.bases)};
    f.right = {p.left.enzyme, p.left.overhang, reverseComplement(p.left.bases)};
    return f;
}

bool pieceBetween(const Sequence &seq, const CutSite &a, const CutSite &b, bool keepLarger, ClonePiece *out,
                  std::string *error)
{
    if (a.cutTop == b.cutTop) {
        if (seq.circular)
            return cutPiece(seq, a, a, out, error);
        if (error)
            *error = "a linear sequence needs two different cuts";
        return false;
    }
    const CutSite &first = a.cutTop < b.cutTop ? a : b;
    const CutSite &second = a.cutTop < b.cutTop ? b : a;
    ClonePiece inner;
    if (!cutPiece(seq, first, second, &inner, error))
        return false;
    if (!seq.circular) {
        *out = inner;
        return true;
    }
    ClonePiece outer;
    if (!cutPiece(seq, second, first, &outer, error))
        return false;
    *out = (outer.length() > inner.length()) == keepLarger ? outer : inner;
    return true;
}

bool cutWithEnzymes(const Sequence &seq, const std::vector<CutSite> &all, const std::string &left,
                    const std::string &right, bool keepLarger, ClonePiece *out, std::string *error)
{
    auto fail = [error](const std::string &msg) {
        if (error)
            *error = msg;
        return false;
    };
    std::vector<CutSite> a, b;
    for (const CutSite &c : all) {
        if (c.enzyme && c.enzyme->name == left)
            a.push_back(c);
        if (c.enzyme && c.enzyme->name == right)
            b.push_back(c);
    }
    if (a.empty() || b.empty())
        return fail(a.empty() ? left + " doesn't cut" : right + " doesn't cut");
    if (left == right) {
        if (a.size() <= 2)
            return pieceBetween(seq, a.front(), a.back(), keepLarger, out, error);
        return fail(left + " cuts " + std::to_string(a.size()) + " times");
    }
    if (a.size() != 1 || b.size() != 1)
        return fail((a.size() != 1 ? left : right) + " must cut only once");
    return pieceBetween(seq, a[0], b[0], keepLarger, out, error);
}

bool endsLigate(const FragmentEnd &right, const FragmentEnd &left)
{
    return right.overhang == left.overhang && right.bases == left.bases;
}

bool ligateCircular(const std::vector<ClonePiece> &pieces, Sequence *out, std::string *error)
{
    if (pieces.empty()) {
        if (error)
            *error = "nothing to ligate";
        return false;
    }
    auto describe = [](const FragmentEnd &e) {
        const std::string kind = e.overhang == 0 ? "blunt" : e.overhang > 0 ? "5′ " + e.bases : "3′ " + e.bases;
        return (e.enzyme.empty() ? std::string("end") : e.enzyme) + " (" + kind + ")";
    };
    for (size_t i = 0; i < pieces.size(); ++i) {
        const FragmentEnd &r = pieces[i].right;
        const FragmentEnd &l = pieces[(i + 1) % pieces.size()].left;
        if (!endsLigate(r, l)) {
            if (error)
                *error = describe(r) + " doesn't ligate to " + describe(l);
            return false;
        }
    }
    // Concatenating top strands keeps each overhang once: a 5' overhang
    // belongs to the top strand of the piece on its right, a 3' overhang to
    // the piece on its left.
    Sequence product = pieces.front().top;
    product.circular = false;
    for (size_t i = 1; i < pieces.size(); ++i)
        insertFragment(product, {product.length(), 0}, pieces[i].top);
    product.circular = true;
    *out = product;
    return true;
}

bool goldenGatePiece(const Sequence &seq, const Enzyme &enzyme, ClonePiece *out, std::string *error)
{
    auto fail = [&](const std::string &msg) {
        if (error)
            *error = seq.name + ": " + msg;
        return false;
    };
    const std::vector<Enzyme> one{enzyme};   // the sites point into this vector
    const std::vector<CutSite> sites = findSites(seq, one);
    if (sites.size() != 2)
        return fail(enzyme.name + " cuts " + std::to_string(sites.size()) + (sites.size() == 1 ? " time" : " times")
                    + ", needs 2");
    const int n = seq.length();
    std::vector<ClonePiece> candidates;
    for (int k = 0; k < 2; ++k) {
        if (!seq.circular && k == 1)
            break;   // a linear input keeps the piece between its cuts
        const CutSite &l = sites[size_t(k)], &r = sites[size_t(1 - k)];
        ClonePiece p;
        if (!cutPiece(seq, l, r, &p, error))
            return false;
        bool clean = true;
        for (const CutSite &c : sites)
            clean = clean && !spanInside(c.matchStart, enzyme.siteLength(), p.sourceStart, p.length(), n);
        if (clean)
            candidates.push_back(p);
    }
    if (candidates.size() != 1)
        return fail(candidates.empty() ? "every piece keeps an " + enzyme.name + " site"
                                       : "both pieces are free of " + enzyme.name + " sites");
    *out = candidates.front();
    out->top.name = out->bottom.name = seq.name;
    return true;
}

bool goldenGate(const std::vector<Sequence> &inputs, const Enzyme &enzyme, Sequence *out,
                std::vector<AssemblyStep> *steps, std::string *error)
{
    if (inputs.size() < 2) {
        if (error)
            *error = "pick a vector and at least one part";
        return false;
    }
    std::vector<ClonePiece> pieces(inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i)
        if (!goldenGatePiece(inputs[i], enzyme, &pieces[i], error))
            return false;
    std::vector<AssemblyStep> chain{{0, false, pieces[0]}};
    std::vector<bool> used(inputs.size(), false);
    used[0] = true;
    for (size_t step = 1; step < inputs.size(); ++step) {
        const FragmentEnd &end = chain.back().piece.right;
        std::vector<AssemblyStep> fits;
        for (size_t i = 1; i < inputs.size(); ++i) {
            if (used[i])
                continue;
            if (endsLigate(end, pieces[i].left))
                fits.push_back({int(i), false, pieces[i]});
            const ClonePiece f = flipPiece(pieces[i]);
            if (endsLigate(end, f.left))
                fits.push_back({int(i), true, f});
        }
        if (fits.size() != 1) {
            if (error)
                *error = fits.empty() ? "no part continues after the " + end.bases + " overhang of " + inputs[size_t(chain.back().input)].name
                                      : "more than one part fits after the " + end.bases + " overhang";
            return false;
        }
        used[size_t(fits.front().input)] = true;
        chain.push_back(fits.front());
    }
    std::vector<ClonePiece> ordered;
    for (const AssemblyStep &s : chain)
        ordered.push_back(s.piece);
    if (!ligateCircular(ordered, out, error))
        return false;
    // Keep the vector's numbering when its origin is in the backbone.
    if (pieces[0].originOffset() > 0)
        rotateOrigin(*out, pieces[0].originOffset());
    if (steps)
        *steps = chain;
    return true;
}

namespace {

// Longest k in [minK, maxK] with the last k bases of `a` equal to the first k of `b`.
int endOverlap(const std::string &a, const std::string &b, int minK)
{
    const int maxK = std::min({int(a.size()), int(b.size()) - 1, 300});
    for (int k = maxK; k >= minK; --k)
        if (a.compare(a.size() - size_t(k), size_t(k), b, 0, size_t(k)) == 0)
            return k;
    return 0;
}

} // namespace

bool gibsonAssemble(const std::vector<Sequence> &fragments, int minOverlap, Sequence *out,
                    std::vector<GibsonJoin> *joins, std::string *error)
{
    auto fail = [error](const std::string &msg) {
        if (error)
            *error = msg;
        return false;
    };
    if (fragments.size() < 2)
        return fail("pick a vector and at least one fragment");
    for (const Sequence &f : fragments)
        if (f.circular)
            return fail(f.name + " is circular; linearize it first");
    std::vector<Sequence> fwd = fragments, rev = fragments;
    for (Sequence &r : rev)
        reverseComplementSequence(r);
    std::vector<GibsonJoin> chain{{0, false, 0}};
    std::vector<bool> used(fragments.size(), false);
    used[0] = true;
    Sequence product = fwd[0];
    const Sequence *last = &fwd[0];
    for (size_t step = 1; step < fragments.size(); ++step) {
        std::vector<GibsonJoin> fits;
        for (size_t i = 1; i < fragments.size(); ++i) {
            if (used[i])
                continue;
            if (int k = endOverlap(last->bases, fwd[i].bases, minOverlap))
                fits.push_back({int(i), false, k});
            if (int k = endOverlap(last->bases, rev[i].bases, minOverlap))
                fits.push_back({int(i), true, k});
        }
        if (fits.size() != 1)
            return fail(fits.empty() ? "nothing overlaps the end of " + last->name + " by " + std::to_string(minOverlap)
                                           + " bases or more"
                                     : "more than one fragment overlaps the end of " + last->name);
        const GibsonJoin &j = fits.front();
        used[size_t(j.input)] = true;
        last = j.flipped ? &rev[size_t(j.input)] : &fwd[size_t(j.input)];
        insertFragment(product, {product.length(), 0}, extractFragment(*last, {j.overlap, last->length() - j.overlap}));
        chain.push_back(j);
    }
    const int close = endOverlap(last->bases, fwd[0].bases, minOverlap);
    if (close == 0)
        return fail("the end of " + last->name + " doesn't overlap the start of " + fwd[0].name);
    chain[0].overlap = close;
    // The closing overlap is in the product twice: drop the copy at the end.
    deleteBases(product, {product.length() - close, close});
    product.circular = true;
    *out = product;
    if (joins)
        *joins = chain;
    return true;
}

bool restrictionClone(const Sequence &vector, const std::string &v1, const std::string &v2, const Sequence &insert,
                      const std::string &i1, const std::string &i2, bool reversed, Sequence *out, std::string *error)
{
    if (!vector.circular) {
        if (error)
            *error = "the vector must be circular";
        return false;
    }
    ClonePiece backbone, piece;
    std::string err;
    if (!cutWithEnzymes(vector, findSites(vector, commonEnzymes()), v1, v2, true, &backbone, &err)) {
        if (error)
            *error = "Vector: " + err;
        return false;
    }
    if (!cutWithEnzymes(insert, findSites(insert, commonEnzymes()), i1, i2, false, &piece, &err)) {
        if (error)
            *error = "Insert: " + err;
        return false;
    }
    if (!ligateCircular({backbone, reversed ? flipPiece(piece) : piece}, out, error))
        return false;
    out->name = vector.name + "-" + insert.name;
    if (backbone.originOffset() > 0)
        rotateOrigin(*out, backbone.originOffset());
    return true;
}

bool gibsonIntoVector(const Sequence &vector, const std::string &v1, const std::string &v2,
                      const std::vector<Sequence> &fragments, int minOverlap, Sequence *out,
                      std::vector<GibsonJoin> *joins, std::string *error)
{
    std::vector<Sequence> inputs;
    ClonePiece backbone;
    if (vector.circular) {
        std::string err;
        if (!cutWithEnzymes(vector, findSites(vector, commonEnzymes()), v1, v2, true, &backbone, &err)) {
            if (error)
                *error = "Vector: " + err;
            return false;
        }
        backbone.top.name = vector.name;
        inputs.push_back(backbone.top);
    } else {
        inputs.push_back(vector);
    }
    inputs.insert(inputs.end(), fragments.begin(), fragments.end());
    std::vector<GibsonJoin> j;
    if (!gibsonAssemble(inputs, minOverlap, out, &j, error))
        return false;
    out->name = vector.name;
    for (const GibsonJoin &step : j)
        if (step.input > 0)
            out->name += "-" + inputs[size_t(step.input)].name;
    if (vector.circular && backbone.originOffset() > 0)
        rotateOrigin(*out, backbone.originOffset());
    if (joins)
        *joins = j;
    return true;
}

} // namespace oc
