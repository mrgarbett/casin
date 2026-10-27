// SPDX-License-Identifier: GPL-3.0-or-later
#include "search.h"
#include "enzymes.h"   // findPattern
#include "seqio.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace oc {

namespace {

bool isDnaQuery(const std::string &q)
{
    if (q.size() < 4)
        return false;
    for (char c : q)
        if (!isIupacBase(c))
            return false;
    return true;
}

bool contains(const std::string &text, const std::string &lowerQuery)
{
    return toLower(text).find(lowerQuery) != std::string::npos;
}

} // namespace

std::vector<FolderHit> searchFolder(const std::string &dir, const std::string &rawQuery, bool recursive, int maxFiles)
{
    namespace fs = std::filesystem;
    std::vector<FolderHit> hits;
    const std::string query = trim(rawQuery);
    if (query.empty())
        return hits;
    std::string upper = query;
    for (char &c : upper)
        c = char(std::toupper(static_cast<unsigned char>(c)));
    const bool dna = isDnaQuery(upper);
    const std::string lower = toLower(query);

    std::vector<fs::path> files;
    std::error_code ec;
    auto consider = [&](const fs::directory_entry &e) {
        if (int(files.size()) < maxFiles && e.is_regular_file(ec) && formatFromExtension(e.path().string()) != FileFormat::Unknown)
            files.push_back(e.path());
    };
    if (recursive) {
        for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec))
            consider(*it);
    } else {
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            consider(*it);
    }
    std::sort(files.begin(), files.end());

    for (const fs::path &p : files) {
        Sequence seq;
        if (!loadSequenceFile(p.string(), &seq))
            continue;
        std::vector<std::string> why;
        if (dna) {
            const std::vector<PatternMatch> m = findPattern(seq, upper);
            if (!m.empty()) {
                std::string where;
                for (size_t i = 0; i < m.size() && i < 5; ++i)
                    where += (i ? ", " : "") + std::to_string(m[i].start + 1) + (m[i].strand == Strand::Reverse ? " (−)" : "");
                if (m.size() > 5)
                    where += ", …";
                why.push_back("sequence at " + where);
            }
        }
        if (contains(seq.name, lower) || contains(p.filename().string(), lower))
            why.push_back("name");
        if (contains(seq.description, lower))
            why.push_back("description");
        for (const Feature &f : seq.features)
            if (contains(f.name, lower)) {
                why.push_back("feature " + f.name);
                break;
            }
        for (const Primer &pr : seq.primers)
            if (contains(pr.name, lower)) {
                why.push_back("primer " + pr.name);
                break;
            }
        if (why.empty())
            continue;
        FolderHit h;
        h.path = p.string();
        h.name = seq.name;
        h.length = seq.length();
        h.circular = seq.circular;
        for (size_t i = 0; i < why.size(); ++i)
            h.match += (i ? "; " : "") + why[i];
        hits.push_back(h);
    }
    return hits;
}

} // namespace oc
