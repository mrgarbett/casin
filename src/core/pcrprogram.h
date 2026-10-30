// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "tm.h"

#include <string>
#include <vector>

namespace oc {

// One step of a PCR program; `cycled` steps repeat `cycles` times.
struct PcrStep {
    std::string name;
    int celsius = 0;
    int seconds = 0;     // 0 for an indefinite hold
    bool cycled = false;
};

struct PcrProgram {
    Polymerase polymerase = Polymerase::Q5;
    int tm1 = 0, tm2 = 0;      // the primers' annealing parts, as polymeraseTm
    int annealing = 0;         // Ta
    bool capped = false;       // Ta held at the polymerase's cap
    bool twoStep = false;      // Ta reaches the extension temperature: anneal and extend together
    int cycles = 30;
    std::vector<PcrStep> steps;
};

// The polymerase's name as NEB sells it.
std::string polymeraseName(Polymerase p);
// The longest product the polymerase is sold for, in bp.
int polymeraseMaxLength(Polymerase p);

// The cycling NEB suggests for a product of `length` bp made with primers
// whose annealing parts are `anneal1` and `anneal2`. Ta follows NEB's
// calculator: Q5 the lower Tm + 1 (at most 72), Phusion 0.93 x the lower
// Tm + 7.5 (at most 72), Taq the lower Tm - 5 (at most 68). Extension: Q5
// and Phusion 30 s per kb at 72, Taq 1 min per kb at 68, rounded up to 5 s.
// No steps when either part isn't plain DNA.
PcrProgram pcrProgram(const std::string &anneal1, const std::string &anneal2, int length, Polymerase p,
                      int cycles = 30);

// "1 min 35 s", "30 s", "2 min", "hold".
std::string formatSeconds(int seconds);

// The program as a Markdown table, after `title` (a line of its own when
// not empty), for a lab notebook.
std::string pcrProgramMarkdown(const PcrProgram &program, const std::string &title);

} // namespace oc
