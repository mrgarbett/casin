// SPDX-License-Identifier: GPL-3.0-or-later
// EMBL flat files. The feature table uses the same INSDC locations and
// qualifiers as GenBank, so reading goes through the GenBank reader.
#pragma once

#include "sequence.h"

namespace oc {

bool looksLikeEmbl(const std::string &data);
// Reads the first record. Returns false and sets *error on failure.
bool readEmbl(const std::string &data, Sequence *out, std::string *error = nullptr);
std::string writeEmbl(const Sequence &seq);

// Splits a GenBank or EMBL file holding several records ("//" ends each).
std::vector<std::string> splitRecords(const std::string &data);

} // namespace oc
