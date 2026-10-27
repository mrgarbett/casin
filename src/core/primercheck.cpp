// SPDX-License-Identifier: GPL-3.0-or-later
#include "primercheck.h"
#include "edit.h"
#include "sequence.h"
#include "tm.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace oc {

namespace {

bool pairs(char x, char y)
{
    return (x == 'A' && y == 'T') || (x == 'T' && y == 'A') || (x == 'G' && y == 'C') || (x == 'C' && y == 'G');
}

// Longest run of pairs when `a` (5'->3') lies antiparallel to `b` (5'->3'),
// over every offset. With `threePrime`, only runs that include the last base
// of `a` or of `b` count: those can be extended by the polymerase.
int longestPairing(const std::string &a, const std::string &b, bool threePrime)
{
    const int la = int(a.size()), lb = int(b.size());
    int best = 0;
    // a[i] pairs with b[j] where i + j = s (antiparallel diagonal).
    for (int s = 0; s <= la + lb - 2; ++s) {
        int run = 0;
        bool touches3 = false;
        for (int i = std::max(0, s - lb + 1); i <= std::min(la - 1, s); ++i) {
            const int j = s - i;
            if (pairs(a[size_t(i)], b[size_t(j)])) {
                ++run;
                touches3 = touches3 || i == la - 1 || j == lb - 1;
                if (!threePrime || touches3)
                    best = std::max(best, run);
            } else {
                run = 0;
                touches3 = false;
            }
        }
    }
    return best;
}

int longestHairpin(const std::string &p)
{
    const int n = int(p.size());
    int best = 0;
    // Stem: p[i..i+k) pairs with p[j-k+1..j] read backwards, loop of 3 or more between.
    for (int i = 0; i < n; ++i)
        for (int j = n - 1; j > i; --j) {
            int k = 0;
            while (i + k < j - k && pairs(p[size_t(i + k)], p[size_t(j - k)]))
                ++k;
            // Leave at least 3 unpaired bases for the loop.
            k = std::min(k, (j - i - 2) / 2);
            best = std::max(best, k);
        }
    return best;
}

std::string fmt(const char *f, double v)
{
    char buf[96];
    std::snprintf(buf, sizeof buf, f, v);
    return buf;
}

} // namespace

PrimerCheck checkPrimer(const std::string &raw)
{
    PrimerCheck c;
    const std::string p = cleanBases(raw);
    c.length = int(p.size());
    if (p.empty())
        return c;
    c.gc = gcFraction(p);
    c.tm = meltingTemperature(p);
    for (int i = std::max(0, c.length - 5); i < c.length; ++i)
        c.clampGC += (p[size_t(i)] == 'G' || p[size_t(i)] == 'C') ? 1 : 0;
    for (int i = 0, run = 0; i < c.length; ++i) {
        run = (i > 0 && p[size_t(i)] == p[size_t(i - 1)]) ? run + 1 : 1;
        c.longestRun = std::max(c.longestRun, run);
    }
    c.selfDimer = longestPairing(p, p, false);
    c.selfDimer3 = longestPairing(p, p, true);
    c.hairpinStem = longestHairpin(p);

    if (c.length < 18)
        c.warnings.push_back("Short (" + std::to_string(c.length) + " nt); 18–30 is typical");
    else if (c.length > 30)
        c.warnings.push_back("Long (" + std::to_string(c.length) + " nt); check for a 5′ tail");
    if (c.gc < 0.4 || c.gc > 0.6)
        c.warnings.push_back(fmt("GC %.0f%%; 40–60%% is typical", c.gc * 100));
    if (std::isnan(c.tm))
        c.warnings.push_back("Tm can't be computed for ambiguous bases");
    else if (c.tm < 50 || c.tm > 68)
        c.warnings.push_back(fmt("Tm %.1f °C; 52–65 °C is typical", c.tm));
    const char last = p.back();
    if (last != 'G' && last != 'C')
        c.warnings.push_back("No G or C at the 3′ end");
    if (c.clampGC > 3)
        c.warnings.push_back(std::to_string(c.clampGC) + " G/C in the last 5 bases can prime off-target");
    if (c.longestRun >= 5)
        c.warnings.push_back("Run of " + std::to_string(c.longestRun) + " identical bases");
    if (c.selfDimer3 >= 4)
        c.warnings.push_back("Self-dimer with " + std::to_string(c.selfDimer3) + " pairs at the 3′ end");
    else if (c.selfDimer >= 8)
        c.warnings.push_back("Self-dimer with " + std::to_string(c.selfDimer) + " pairs");
    if (c.hairpinStem >= 4)
        c.warnings.push_back("Hairpin with a " + std::to_string(c.hairpinStem) + " bp stem");
    return c;
}

PairCheck checkPrimerPair(const std::string &rawA, const std::string &rawB)
{
    PairCheck c;
    const std::string a = cleanBases(rawA), b = cleanBases(rawB);
    if (a.empty() || b.empty())
        return c;
    c.dimer = longestPairing(a, b, false);
    c.dimer3 = longestPairing(a, b, true);
    const double ta = meltingTemperature(a), tb = meltingTemperature(b);
    c.tmDifference = std::fabs(ta - tb);
    if (c.dimer3 >= 4)
        c.warnings.push_back("Primer-dimer with " + std::to_string(c.dimer3) + " pairs at a 3′ end");
    else if (c.dimer >= 8)
        c.warnings.push_back("Primer-dimer with " + std::to_string(c.dimer) + " pairs");
    if (!std::isnan(c.tmDifference) && c.tmDifference > 5)
        c.warnings.push_back(fmt("Tm differs by %.1f °C; keep it within 5 °C", c.tmDifference));
    return c;
}

namespace {

struct Candidate {
    std::string bases;
    int length;
    PrimerCheck check;
    double score;
};

std::vector<Candidate> candidates(const std::string &region, double targetTm)
{
    std::vector<Candidate> out;
    for (int len = 18; len <= std::min(32, int(region.size())); ++len) {
        Candidate c{region.substr(0, size_t(len)), len, checkPrimer(region.substr(0, size_t(len))), 0};
        if (std::isnan(c.check.tm))
            continue;
        const char last = c.bases.back();
        c.score = std::fabs(c.check.tm - targetTm) + (last == 'G' || last == 'C' ? 0 : 2)
                + 1.5 * double(c.check.warnings.size());
        out.push_back(c);
    }
    return out;
}

} // namespace

bool designPrimers(const Sequence &seq, Span target, double targetTm, PrimerDesign *out, std::string *error)
{
    auto fail = [error](const char *msg) {
        if (error)
            *error = msg;
        return false;
    };
    const int n = seq.length();
    if (target.length < 18)
        return fail("select at least 18 bases to amplify");
    if (target.start < 0 || target.start + target.length > n)
        return fail("the selection can't cross the origin");
    const std::string region = seq.bases.substr(size_t(target.start), size_t(target.length));
    const std::vector<Candidate> fwd = candidates(region, targetTm);
    const std::vector<Candidate> rev = candidates(reverseComplement(region), targetTm);
    if (fwd.empty() || rev.empty())
        return fail("no primer can be computed for ambiguous bases");
    const Candidate *bestF = nullptr, *bestR = nullptr;
    double best = 0;
    for (const Candidate &f : fwd)
        for (const Candidate &r : rev) {
            if (f.length + r.length > target.length + 32)
                continue;
            const PairCheck pc = checkPrimerPair(f.bases, r.bases);
            const double score = f.score + r.score + std::fabs(f.check.tm - r.check.tm) + 2.0 * double(pc.warnings.size());
            if (!bestF || score < best) {
                best = score;
                bestF = &f;
                bestR = &r;
            }
        }
    if (!bestF)
        return fail("the selection is too short for two primers");
    PrimerDesign d;
    d.forward = {seq.name.empty() ? "fwd" : seq.name + "-fwd", bestF->bases,
                 {target.start, target.start + bestF->length}, Strand::Forward};
    d.reverse = {seq.name.empty() ? "rev" : seq.name + "-rev", bestR->bases,
                 {target.start + target.length - bestR->length, target.start + target.length}, Strand::Reverse};
    d.forwardCheck = bestF->check;
    d.reverseCheck = bestR->check;
    d.pair = checkPrimerPair(bestF->bases, bestR->bases);
    *out = d;
    return true;
}

bool designMutagenesis(const Sequence &seq, Span target, const std::string &rawReplacement, double targetTm,
                       MutagenesisDesign *out, std::string *error)
{
    auto fail = [error](const char *msg) {
        if (error)
            *error = msg;
        return false;
    };
    const int n = seq.length();
    const std::string replacement = cleanBases(rawReplacement);
    const int s = target.start, e = target.start + target.length;
    if (target.length == 0 && replacement.empty())
        return fail("type the new bases, or select bases to delete");
    if (s < 32 || e + 32 > n)
        return fail("the change is too close to the end or the origin; move the origin first");
    if (cleanBases(seq.bases.substr(size_t(s), size_t(target.length))) == replacement)
        return fail("the new bases are the same as the old ones");
    const std::vector<Candidate> fwd = candidates(seq.bases.substr(size_t(e), 32), targetTm);
    const std::vector<Candidate> rev = candidates(reverseComplement(seq.bases.substr(size_t(s - 32), 32)), targetTm);
    if (fwd.empty() || rev.empty())
        return fail("no primer can be computed for ambiguous bases");
    const Candidate *bf = nullptr, *br = nullptr;
    double best = 0;
    for (const Candidate &f : fwd)
        for (const Candidate &r : rev) {
            const double score = f.score + r.score + std::fabs(f.check.tm - r.check.tm);
            if (!bf || score < best) {
                best = score;
                bf = &f;
                br = &r;
            }
        }
    MutagenesisDesign d;
    const std::string base = seq.name.empty() ? "mut" : seq.name + "-mut";
    d.forward = {base + "-fwd", replacement + bf->bases, {e, e + bf->length}, Strand::Forward};
    d.reverse = {base + "-rev", br->bases, {s - br->length, s}, Strand::Reverse};
    d.forwardTm = bf->check.tm;
    d.reverseTm = br->check.tm;
    for (const std::string &w : checkPrimer(d.forward.bases).warnings)
        if (w.rfind("Long", 0) != 0 || replacement.size() < 10)   // a long tail is expected here
            d.warnings.push_back("Forward: " + w);
    for (const std::string &w : br->check.warnings)
        d.warnings.push_back("Reverse: " + w);
    if (replacement.size() > 40)
        d.warnings.push_back("Inserts longer than about 40 bases are easier split over both primers or made by assembly");
    d.mutant = seq;
    d.mutant.name = base;
    replaceBases(d.mutant, target, replacement);
    const std::string old = seq.bases.substr(size_t(s), size_t(target.length));
    addHistory(d.mutant, "Mutagenesis at " + std::to_string(s + 1) + ": " + (old.empty() ? "-" : old) + " to "
                             + (replacement.empty() ? "-" : replacement));
    const int r = int(replacement.size());
    Primer mf = d.forward, mr = d.reverse;
    mf.binding = {s, s + r + bf->length};
    d.mutant.primers.push_back(mf);
    d.mutant.primers.push_back(mr);
    *out = d;
    return true;
}

std::vector<PrimerSite> findPrimerSites(const Sequence &seq, const std::string &raw, int maxMismatches, int minBinding)
{
    std::vector<PrimerSite> out;
    const std::string p = cleanBases(raw);
    const int n = seq.length(), lp = int(p.size());
    if (lp < minBinding || n < minBinding)
        return out;
    const std::string &top = seq.bases;
    const std::string bottom = reverseComplement(top);   // read 5'->3' on the bottom strand
    for (int pass = 0; pass < 2; ++pass) {
        const std::string &t = pass == 0 ? top : bottom;
        // The primer's 3' end sits on base e of t; walk back towards its 5' end.
        for (int e = minBinding - 1; e < n; ++e) {
            if (t[size_t(e)] != p.back() || t[size_t(e - 1)] != p[size_t(lp - 2)] || t[size_t(e - 2)] != p[size_t(lp - 3)])
                continue;
            // Extend towards the 5' end; a mismatch costs three matches, so a
            // tail that happens to match a little doesn't pull the site out.
            int mism = 0, bestLen = 0, bestMism = 0, bestScore = 0;
            for (int k = 0; k < lp && e - k >= 0; ++k) {
                if (t[size_t(e - k)] != p[size_t(lp - 1 - k)] && ++mism > maxMismatches)
                    break;
                const int score = (k + 1 - mism) - 3 * mism;
                if (t[size_t(e - k)] == p[size_t(lp - 1 - k)] && score >= bestScore) {
                    bestLen = k + 1;
                    bestMism = mism;
                    bestScore = score;
                }
            }
            if (bestLen < minBinding)
                continue;
            PrimerSite s;
            s.strand = pass == 0 ? Strand::Forward : Strand::Reverse;
            s.mismatches = bestMism;
            const int start = e - bestLen + 1;   // in t
            s.binding = pass == 0 ? Range{start, e + 1} : Range{n - 1 - e, n - start};
            out.push_back(s);
        }
    }
    std::sort(out.begin(), out.end(), [](const PrimerSite &a, const PrimerSite &b) {
        if (a.mismatches != b.mismatches)
            return a.mismatches < b.mismatches;
        if (a.binding.length() != b.binding.length())
            return a.binding.length() > b.binding.length();
        return a.binding.start < b.binding.start;
    });
    return out;
}

} // namespace oc
