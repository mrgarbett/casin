// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sequence.h"

namespace oc {

struct Location {
    std::vector<Range> ranges;   // top-strand order
    Strand strand = Strand::Forward;
    bool ok = false;
    std::string error;
};

// Parses a GenBank/INSDC location: complement, join, order, <, >, ^ and
// single bases. "a..b" with a > b on a circular sequence is split into
// [a-1, n) and [0, b). Remote references (ACC:1..10) are skipped.
Location parseLocation(const std::string &text, int seqLength, bool circular);
std::string formatLocation(const std::vector<Range> &ranges, Strand strand, int seqLength, bool circular);

// Reads the first record. Returns false and sets *error on failure.
bool readGenBank(const std::string &data, Sequence *out, std::string *error = nullptr);
std::string writeGenBank(const Sequence &seq);

} // namespace oc
