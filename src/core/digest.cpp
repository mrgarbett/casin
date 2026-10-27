// SPDX-License-Identifier: GPL-3.0-or-later
#include "digest.h"

#include <algorithm>

namespace oc {

DigestResult digest(const Sequence &seq, const std::vector<Enzyme> &enzymes)
{
    DigestResult out;
    const int n = seq.length();
    if (n == 0)
        return out;
    std::vector<CutSite> sites = findSites(seq, enzymes);
    // Two enzymes (or two matches) cutting at the same place make one cut.
    std::sort(sites.begin(), sites.end(), [](const CutSite &a, const CutSite &b) { return a.cutTop < b.cutTop; });
    sites.erase(std::unique(sites.begin(), sites.end(),
                            [](const CutSite &a, const CutSite &b) { return a.cutTop == b.cutTop; }),
                sites.end());

    // Both cuts are in sequence coordinates, so this holds on either strand;
    // on circular sequences they may sit on opposite sides of the origin.
    auto overhang = [n](const CutSite &s) {
        int o = s.cutBottom - s.cutTop;
        if (o > n / 2)
            o -= n;
        else if (o < -n / 2)
            o += n;
        return o;
    };
    if (sites.empty()) {
        out.uncut = true;
        out.fragments.push_back({0, n, {}, {}, 0, 0});
        return out;
    }
    if (seq.circular) {
        for (size_t i = 0; i < sites.size(); ++i) {
            const CutSite &a = sites[i];
            const CutSite &b = sites[(i + 1) % sites.size()];
            int len = b.cutTop - a.cutTop;
            if (len <= 0)
                len += n;
            out.fragments.push_back({a.cutTop, len, a.enzyme->name, b.enzyme->name, overhang(a), overhang(b)});
        }
        return out;
    }
    int prev = 0;
    const CutSite *left = nullptr;
    for (const CutSite &s : sites) {
        out.fragments.push_back({prev, s.cutTop - prev, left ? left->enzyme->name : std::string(), s.enzyme->name,
                                 left ? overhang(*left) : 0, overhang(s)});
        prev = s.cutTop;
        left = &s;
    }
    out.fragments.push_back({prev, n - prev, left->enzyme->name, {}, overhang(*left), 0});
    return out;
}

const std::vector<Ladder> &commonLadders()
{
    static const std::vector<Ladder> list = {
        {"1 kb ladder", {10000, 8000, 6000, 5000, 4000, 3000, 2500, 2000, 1500, 1000, 750, 500, 250}, {3000, 1000}},
        {"100 bp ladder", {1500, 1200, 1000, 900, 800, 700, 600, 500, 400, 300, 200, 100}, {1000, 500}},
        {"1 kb plus ladder",
         {20000, 10000, 7000, 5000, 4000, 3000, 2000, 1500, 1000, 700, 500, 400, 300, 200, 75},
         {5000, 1500, 500}},
    };
    return list;
}

} // namespace oc
