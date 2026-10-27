// SPDX-License-Identifier: GPL-3.0-or-later
#include "primercheck.h"
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

} // namespace oc
