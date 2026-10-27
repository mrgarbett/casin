// SPDX-License-Identifier: GPL-3.0-or-later
#include "seqio.h"
#include "abif.h"
#include "embl.h"
#include "fasta.h"
#include "genbank.h"
#include "snapgene.h"

#include <cctype>
#include <fstream>
#include <sstream>

namespace oc {

FileFormat formatFromExtension(const std::string &path)
{
    const std::string ext = fileExtension(path);
    if (ext == "gb" || ext == "gbk" || ext == "genbank" || ext == "gbff" || ext == "ape" || ext == "gbx")
        return FileFormat::GenBank;
    if (ext == "fa" || ext == "fasta" || ext == "fna" || ext == "fas" || ext == "fsa" || ext == "seq")
        return FileFormat::Fasta;
    if (ext == "dna")
        return FileFormat::SnapGene;
    if (ext == "ab1" || ext == "abi" || ext == "ab")
        return FileFormat::Abif;
    if (ext == "embl" || ext == "emb")
        return FileFormat::Embl;
    return FileFormat::Unknown;
}

FileFormat sniffFormat(const std::string &content)
{
    if (looksLikeSnapGene(content))
        return FileFormat::SnapGene;
    if (looksLikeAbif(content))
        return FileFormat::Abif;
    size_t i = 0;
    while (i < content.size() && std::isspace(static_cast<unsigned char>(content[i])))
        ++i;
    if (content.compare(i, 5, "LOCUS") == 0)
        return FileFormat::GenBank;
    if (looksLikeEmbl(content))
        return FileFormat::Embl;
    if (i < content.size() && content[i] == '>')
        return FileFormat::Fasta;
    // A bare sequence: mostly IUPAC letters.
    size_t letters = 0, total = 0;
    for (size_t j = i; j < content.size() && total < 4096; ++j) {
        unsigned char c = static_cast<unsigned char>(content[j]);
        if (std::isspace(c) || std::isdigit(c))
            continue;
        ++total;
        if (isIupacBase(char(c)) || c == 'U' || c == 'u')
            ++letters;
    }
    if (total > 0 && letters * 10 >= total * 9)
        return FileFormat::Fasta;
    return FileFormat::Unknown;
}

FileFormat detectFormat(const std::string &path, const std::string &content)
{
    FileFormat f = formatFromExtension(path);
    // Trust the extension unless the content clearly says otherwise.
    FileFormat sniffed = sniffFormat(content);
    if (f == FileFormat::Unknown)
        return sniffed;
    if ((f != FileFormat::SnapGene && sniffed == FileFormat::SnapGene) || sniffed == FileFormat::Abif)
        return sniffed;
    if (f == FileFormat::SnapGene && !looksLikeSnapGene(content) && sniffed != FileFormat::Unknown)
        return sniffed;
    return f;
}

bool readFile(const std::string &path, std::string *content, std::string *error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error)
            *error = "cannot open " + path;
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    *content = ss.str();
    return true;
}

bool writeFile(const std::string &path, const std::string &content, std::string *error)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error)
            *error = "cannot write " + path;
        return false;
    }
    out << content;
    out.close();
    if (!out) {
        if (error)
            *error = "error writing " + path;
        return false;
    }
    return true;
}

bool parseSequence(const std::string &content, FileFormat format, Sequence *out, std::string *error)
{
    switch (format) {
    case FileFormat::GenBank: return readGenBank(content, out, error);
    case FileFormat::Fasta: return readFasta(content, out, error);
    case FileFormat::SnapGene: return readSnapGene(content, out, error);
    case FileFormat::Embl: return readEmbl(content, out, error);
    case FileFormat::Abif: {
        Trace t;
        if (!readAbif(content, &t, error))
            return false;
        Sequence s;
        s.name = t.name;
        s.bases = t.bases;
        const Span good = goodRegion(t);
        s.description = "Sanger read, " + std::to_string(t.bases.size()) + " bases; good quality "
                      + std::to_string(good.start + 1) + "-" + std::to_string(good.start + good.length);
        *out = s;
        return true;
    }
    default:
        if (error)
            *error = "unrecognized file format";
        return false;
    }
}

bool loadSequenceFile(const std::string &path, Sequence *out, std::string *error, FileFormat *format)
{
    return loadSequenceFile(path, out, nullptr, error, format);
}

bool loadSequenceFile(const std::string &path, Sequence *out, std::vector<Sequence> *more, std::string *error,
                      FileFormat *format)
{
    std::string content;
    if (!readFile(path, &content, error))
        return false;
    Sequence seq;
    const FileFormat f = detectFormat(path, content);
    if (!parseSequence(content, f, &seq, error))
        return false;
    if (format)
        *format = f;
    if (trim(seq.name).empty())
        seq.name = fileBaseName(path);
    if (more && (f == FileFormat::GenBank || f == FileFormat::Embl)) {
        const std::vector<std::string> records = splitRecords(content);
        for (size_t i = 1; i < records.size(); ++i) {
            Sequence extra;
            if (parseSequence(records[i], f, &extra, nullptr) && !extra.bases.empty()) {
                if (trim(extra.name).empty())
                    extra.name = fileBaseName(path) + " " + std::to_string(i + 1);
                more->push_back(extra);
            }
        }
    }
    *out = seq;
    return true;
}

bool saveSequenceFile(const std::string &path, const Sequence &seq, FileFormat format, std::string *error)
{
    switch (format) {
    case FileFormat::GenBank: return writeFile(path, writeGenBank(seq), error);
    case FileFormat::Fasta: return writeFile(path, writeFasta(seq), error);
    case FileFormat::SnapGene: return writeFile(path, writeSnapGene(seq), error);
    case FileFormat::Embl: return writeFile(path, writeEmbl(seq), error);
    default:
        if (error)
            *error = "unknown file format";
        return false;
    }
}

} // namespace oc
