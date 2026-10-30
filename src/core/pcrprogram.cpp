// SPDX-License-Identifier: GPL-3.0-or-later
#include "pcrprogram.h"

#include <algorithm>
#include <climits>
#include <cmath>

namespace oc {

namespace {

struct Cycling {
    int denature, initialSeconds, denatureSeconds, annealSeconds, extend, secondsPerKb, minExtendSeconds,
        finalSeconds, cap;
};

// NEB's protocols for each polymerase.
Cycling cycling(Polymerase p)
{
    switch (p) {
    case Polymerase::Q5: return {98, 30, 10, 20, 72, 30, 10, 120, 72};
    case Polymerase::Phusion: return {98, 30, 10, 20, 72, 30, 15, 300, 72};
    case Polymerase::Taq: return {95, 30, 30, 30, 68, 60, 30, 300, 68};
    }
    return {};
}

} // namespace

std::string polymeraseName(Polymerase p)
{
    switch (p) {
    case Polymerase::Q5: return "Q5 High-Fidelity";
    case Polymerase::Phusion: return "Phusion High-Fidelity (HF buffer)";
    case Polymerase::Taq: return "Taq";
    }
    return {};
}

int polymeraseMaxLength(Polymerase p)
{
    return p == Polymerase::Taq ? 5000 : 20000;
}

PcrProgram pcrProgram(const std::string &anneal1, const std::string &anneal2, int length, Polymerase p,
                      int cycles)
{
    PcrProgram out;
    out.polymerase = p;
    out.cycles = std::clamp(cycles, 1, 99);
    out.tm1 = polymeraseTm(anneal1, p);
    out.tm2 = polymeraseTm(anneal2, p);
    if (out.tm1 == INT_MIN || out.tm2 == INT_MIN)
        return out;
    const Cycling c = cycling(p);
    // From the lower Tm before rounding, as NEB's calculator does.
    const double low = std::min(polymeraseTmExact(anneal1, p), polymeraseTmExact(anneal2, p));
    const double ta = p == Polymerase::Phusion ? 0.93 * low + 7.5 : p == Polymerase::Q5 ? low + 1.0 : low - 5.0;
    out.annealing = int(std::lround(std::min(ta, double(c.cap))));
    out.capped = ta > c.cap;
    out.twoStep = out.annealing >= c.extend;

    const int perKb = (std::max(0, length) * c.secondsPerKb + 999) / 1000;
    const int extendSeconds = std::max(c.minExtendSeconds, (perKb + 4) / 5 * 5);
    out.steps.push_back({"Initial denaturation", c.denature, c.initialSeconds, false});
    out.steps.push_back({"Denaturation", c.denature, c.denatureSeconds, true});
    if (out.twoStep) {
        out.steps.push_back({"Annealing and extension", c.extend, extendSeconds, true});
    } else {
        out.steps.push_back({"Annealing", out.annealing, c.annealSeconds, true});
        out.steps.push_back({"Extension", c.extend, extendSeconds, true});
    }
    out.steps.push_back({"Final extension", c.extend, c.finalSeconds, false});
    out.steps.push_back({"Hold", 4, 0, false});
    return out;
}

std::string formatSeconds(int s)
{
    if (s <= 0)
        return "hold";
    if (s < 60)
        return std::to_string(s) + " s";
    if (s % 60 == 0)
        return std::to_string(s / 60) + " min";
    return std::to_string(s / 60) + " min " + std::to_string(s % 60) + " s";
}

std::string pcrProgramMarkdown(const PcrProgram &program, const std::string &title)
{
    std::string md;
    if (!title.empty())
        md += title + "\n\n";
    md += "| Step | Temperature | Time | Cycles |\n|---|---|---|---|\n";
    bool cycledShown = false;
    for (const PcrStep &st : program.steps) {
        std::string cycles;
        if (st.cycled && !cycledShown) {
            cycles = std::to_string(program.cycles);
            cycledShown = true;
        } else if (!st.cycled) {
            cycles = "1";
        }
        md += "| " + st.name + " | " + std::to_string(st.celsius) + " °C | " + formatSeconds(st.seconds) + " | "
            + cycles + " |\n";
    }
    return md;
}

} // namespace oc
