// SPDX-License-Identifier: GPL-3.0-or-later
// Searching a folder of sequence files by text (names, descriptions, feature
// and primer names) or by a DNA sequence (both strands, IUPAC).
#pragma once

#include "sequence.h"

namespace oc {

struct FolderHit {
    std::string path;
    std::string name;
    int length = 0;
    bool circular = false;
    std::string match;   // what matched: "sequence at 120, 980", "feature lacZ", ...
};

// Looks at every file the readers recognise under `dir` (and its subfolders
// when `recursive`), stopping after `maxFiles` files.
std::vector<FolderHit> searchFolder(const std::string &dir, const std::string &query, bool recursive = true,
                                    int maxFiles = 5000);

} // namespace oc
