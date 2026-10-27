// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "sequence.h"

namespace oc {

enum class FileFormat { Unknown, GenBank, Fasta, SnapGene, Abif };   // Abif: read only

// From the extension first, then by sniffing the content.
FileFormat detectFormat(const std::string &path, const std::string &content);
FileFormat formatFromExtension(const std::string &path);
FileFormat sniffFormat(const std::string &content);

bool readFile(const std::string &path, std::string *content, std::string *error = nullptr);
bool writeFile(const std::string &path, const std::string &content, std::string *error = nullptr);

bool parseSequence(const std::string &content, FileFormat format, Sequence *out, std::string *error = nullptr);
// Uses the file's base name when the sequence has no name. *format (if
// given) is the format that was read; a read-only one (Abif) means saving
// must go to a new file.
bool loadSequenceFile(const std::string &path, Sequence *out, std::string *error = nullptr,
                      FileFormat *format = nullptr);
bool saveSequenceFile(const std::string &path, const Sequence &seq, FileFormat format,
                      std::string *error = nullptr);

} // namespace oc
