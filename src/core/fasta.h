// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sequence.h"

namespace oc {

// Reads the first record, or a bare sequence with no header line.
bool readFasta(const std::string &data, Sequence *out, std::string *error = nullptr);
// 70 bases per line.
std::string writeFasta(const Sequence &seq);

} // namespace oc
