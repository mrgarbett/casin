// SPDX-License-Identifier: GPL-3.0-or-later
#include "gateway.h"

#include "edit.h"

#include <algorithm>

namespace oc {
namespace {

// The core of each att number, read 5'->3' the way that site's own attB
// primer tail reads it. Two sites flanking an insert therefore point at
// each other: the one at the left reads on the top strand, the one at the
// right on the bottom. Taken from the att sites in Invitrogen's Gateway and
// MultiSite Gateway manuals (see docs/KNOWN-RESULTS.md).
const char *const kCores[] = {
    "TTTGTACAAAAAAGC",   // att1
    "GCTTTCTTGTACAAA",   // att2
    "TTTGTATAATAAAGT",   // att3
    "TTTGTATAGAAAAGT",   // att4
};
constexpr int kCoreLength = 15;
constexpr int kCrossover = kCoreLength / 2;   // where the strands cross inside the core

// Every start of `needle` on the top strand; a circular sequence is searched
// across its origin too.
std::vector<int> findAll(const Sequence &seq, const std::string &needle)
{
    std::vector<int> out;
    const int n = seq.length();
    if (n == 0 || needle.empty() || int(needle.size()) > n)
        return out;
    std::string hay = seq.bases;
    if (seq.circular)
        hay += seq.bases.substr(0, needle.size() - 1);
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1))
        if (int(p) < n)
            out.push_back(int(p));
    return out;
}

// The two sites of one molecule, the one reading on the top strand first.
struct SitePair {
    AttSite forward, reverse;
};

// Adds `name` over `span` (which may wrap) as a recombination site.
void addAttFeature(Sequence &seq, const Span &span, const std::string &name)
{
    const int n = seq.length();
    Feature f;
    f.type = "misc_recomb";
    f.name = name;
    if (span.end() > n) {
        f.ranges = {{span.start, n}, {0, span.end() - n}};
    } else {
        f.ranges = {{span.start, span.end()}};
    }
    seq.features.push_back(f);
}

// Features of a product that mark att sites the reaction has just rebuilt:
// each old one was cut in half, so they go and the new ones are added.
void dropAttFeatures(Sequence &seq)
{
    seq.features.erase(std::remove_if(seq.features.begin(), seq.features.end(),
                                      [](const Feature &f) {
                                          return f.name.rfind("att", 0) == 0 || f.name.rfind("ATT", 0) == 0;
                                      }),
                       seq.features.end());
}

std::string siteName(GatewayReaction reaction, bool clone, int number)
{
    const char family = reaction == GatewayReaction::BP ? (clone ? 'L' : 'R') : (clone ? 'B' : 'P');
    return std::string("att") + family + std::to_string(number);
}

} // namespace

std::string attCore(int number)
{
    const int count = int(sizeof kCores / sizeof kCores[0]);
    return number >= 1 && number <= count ? kCores[number - 1] : std::string();
}

std::vector<AttSite> findAttSites(const Sequence &seq)
{
    std::vector<AttSite> out;
    for (int number = 1; !attCore(number).empty(); ++number) {
        const std::string core = attCore(number);
        for (int start : findAll(seq, core))
            out.push_back({number, {start, kCoreLength}, false});
        for (int start : findAll(seq, reverseComplement(core)))
            out.push_back({number, {start, kCoreLength}, true});
    }
    std::sort(out.begin(), out.end(), [](const AttSite &a, const AttSite &b) { return a.span.start < b.span.start; });
    return out;
}

bool gatewayRecombine(const Sequence &insert, const Sequence &vector, GatewayReaction reaction, GatewayResult *out,
                      std::string *error)
{
    auto fail = [error](const std::string &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!vector.circular)
        return fail((reaction == GatewayReaction::BP ? std::string("the donor vector")
                                                     : std::string("the destination vector"))
                    + " must be circular");

    // One molecule's pair of sites: one reading on each strand (they point at
    // each other across the insert). A linear molecule whose sites lie the
    // other way round is read from its other end.
    auto pairOf = [&fail](const Sequence &seq, const std::string &what, Sequence *oriented, SitePair *pair,
                          bool *turnedOver) -> bool {
        Sequence s = seq;
        for (int attempt = 0; attempt < 2; ++attempt) {
            const std::vector<AttSite> sites = findAttSites(s);
            if (sites.size() != 2)
                return fail(what + " carries " + std::to_string(sites.size()) + " att sites, not two");
            if (sites[0].reversed == sites[1].reversed)
                return fail("the two att sites of " + what + " point the same way");
            if (sites[0].number == sites[1].number)
                return fail(what + " carries att" + std::to_string(sites[0].number) + " twice");
            const bool forwardFirst = !sites[0].reversed;
            if (forwardFirst || s.circular) {
                *oriented = s;
                pair->forward = forwardFirst ? sites[0] : sites[1];
                pair->reverse = forwardFirst ? sites[1] : sites[0];
                return true;
            }
            reverseComplementSequence(s);   // linear, the other way round
            *turnedOver = !*turnedOver;
        }
        return fail(what + "'s att sites don't line up");
    };

    Sequence a, b;
    SitePair as{}, bs{};
    bool insertFlipped = false, vectorFlipped = false;
    if (!pairOf(insert, reaction == GatewayReaction::BP ? "the attB fragment" : "the entry clone", &a, &as,
                &insertFlipped))
        return false;
    if (!pairOf(vector, reaction == GatewayReaction::BP ? "the donor vector" : "the destination vector", &b, &bs,
                &vectorFlipped))
        return false;
    // The same molecule read from its other end has its two sites the other
    // way round: turn it over rather than refuse it.
    if (as.forward.number == bs.reverse.number && as.reverse.number == bs.forward.number) {
        Sequence turned = a;
        reverseComplementSequence(turned);
        insertFlipped = !insertFlipped;
        if (!pairOf(turned, "the insert", &a, &as, &insertFlipped))
            return false;
    }
    if (as.forward.number != bs.forward.number || as.reverse.number != bs.reverse.number)
        return fail("the att sites don't match: att" + std::to_string(as.forward.number) + " and att"
                    + std::to_string(as.reverse.number) + " against att" + std::to_string(bs.forward.number)
                    + " and att" + std::to_string(bs.reverse.number));

    // The strands cross inside each core, so each product carries part of
    // the core from either parent; the core is the same in both, so it comes
    // out whole. The piece between the cores changes places.
    auto arc = [](const Sequence &seq, const AttSite &from, const AttSite &to) {
        const int n = seq.length();
        const int start = from.span.start + kCrossover;
        int length = to.span.start + kCrossover - start;
        if (length <= 0) {
            if (!seq.circular)
                return Span{0, 0};
            length += n;
        }
        return Span{start % std::max(1, n), length};
    };
    const Span insertArc = arc(a, as.forward, as.reverse);     // the insert itself
    const Span vectorArc = arc(b, bs.forward, bs.reverse);     // what it replaces (ccdB and friends)
    const Span vectorRest = arc(b, bs.reverse, bs.forward);    // the backbone that keeps it
    if (insertArc.length <= 0 || vectorRest.length <= 0)
        return fail("the att sites are in the wrong order");

    Sequence clone = extractFragment(b, vectorRest);
    insertFragment(clone, {clone.length(), 0}, extractFragment(a, insertArc));
    clone.circular = true;
    clone.name = vector.name + "-" + insert.name;
    dropAttFeatures(clone);
    // The site of the reverse-reading number spans the join at the origin.
    addAttFeature(clone, {vectorRest.length - kCrossover, kCoreLength}, siteName(reaction, true, as.forward.number));
    addAttFeature(clone, {clone.length() - kCrossover, kCoreLength}, siteName(reaction, true, as.reverse.number));
    clone.history = vector.history;
    addHistory(clone, std::string(reaction == GatewayReaction::BP ? "Gateway BP: " : "Gateway LR: ") + insert.name
                          + " x " + vector.name);

    GatewayResult result;
    result.clone = clone;
    result.firstSite = as.forward.number;
    result.secondSite = as.reverse.number;
    result.insertFlipped = insertFlipped;
    // The other circle (the ccdB piece joined to what was left of the
    // insert's molecule) only closes when the insert was a circle too: an
    // attB PCR product leaves loose ends instead.
    if (a.circular && vectorArc.length > 0) {
        const Span insertRest = arc(a, as.reverse, as.forward);
        if (insertRest.length > 0) {
            Sequence by = extractFragment(b, vectorArc);
            insertFragment(by, {by.length(), 0}, extractFragment(a, insertRest));
            by.circular = true;
            by.name = clone.name + "-byproduct";
            dropAttFeatures(by);
            addAttFeature(by, {vectorArc.length - kCrossover, kCoreLength}, siteName(reaction, false, as.reverse.number));
            addAttFeature(by, {by.length() - kCrossover, kCoreLength}, siteName(reaction, false, as.forward.number));
            result.byproduct = by;
        }
    }
    *out = result;
    return true;
}

} // namespace oc
