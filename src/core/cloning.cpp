// SPDX-License-Identifier: GPL-3.0-or-later
#include "cloning.h"
#include "edit.h"

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

} // namespace

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
    *out = p;
    return true;
}

ClonePiece flipPiece(const ClonePiece &p)
{
    ClonePiece f;
    f.top = p.bottom;
    f.bottom = p.top;
    f.left = {p.right.enzyme, p.right.overhang, reverseComplement(p.right.bases)};
    f.right = {p.left.enzyme, p.left.overhang, reverseComplement(p.left.bases)};
    return f;
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

} // namespace oc
