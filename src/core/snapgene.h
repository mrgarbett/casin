// SPDX-License-Identifier: GPL-3.0-or-later
// Reader and writer for SnapGene .dna files, written from the publicly documented packet
// layout (also implemented by Biopython and other open-source readers).
#pragma once

#include "sequence.h"

namespace oc {

bool readSnapGene(const std::string &data, Sequence *out, std::string *error = nullptr);
bool looksLikeSnapGene(const std::string &data);
// Writes DNA, features, primers and the description. Other packets the
// format allows (history, alignments, traces) are not written.
std::string writeSnapGene(const Sequence &seq);

} // namespace oc
