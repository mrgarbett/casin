// SPDX-License-Identifier: GPL-3.0-or-later
#include "abif.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <utility>

namespace oc {

namespace {

uint32_t u32(const std::string &d, size_t at)
{
    return uint32_t(uint8_t(d[at])) << 24 | uint32_t(uint8_t(d[at + 1])) << 16 | uint32_t(uint8_t(d[at + 2])) << 8
         | uint32_t(uint8_t(d[at + 3]));
}

uint16_t u16(const std::string &d, size_t at)
{
    return uint16_t(uint16_t(uint8_t(d[at])) << 8 | uint8_t(d[at + 1]));
}

struct Entry {
    int type = 0;
    int elementSize = 0;
    int count = 0;
    std::string data;   // the raw bytes
};

enum { kChar = 2, kShort = 4, kLong = 5, kPString = 18, kCString = 19 };

std::vector<int> shorts(const Entry &e)
{
    std::vector<int> out;
    if (e.type != kShort)
        return out;
    for (size_t i = 0; i + 1 < e.data.size(); i += 2)
        out.push_back(int16_t(u16(e.data, i)));
    return out;
}

std::string text(const Entry &e)
{
    if (e.type == kPString && !e.data.empty())
        return e.data.substr(1, std::min<size_t>(uint8_t(e.data[0]), e.data.size() - 1));
    std::string s = e.data;
    if (const size_t z = s.find('\0'); z != std::string::npos)
        s.resize(z);
    return s;
}

} // namespace

bool looksLikeAbif(const std::string &data)
{
    return data.size() >= 34 && data.compare(0, 4, "ABIF") == 0;
}

bool readAbif(const std::string &d, Trace *out, std::string *error)
{
    auto fail = [error](const char *msg) {
        if (error)
            *error = msg;
        return false;
    };
    if (!looksLikeAbif(d))
        return fail("not an ABIF (.ab1) file");
    // The header holds one directory entry that points at the directory.
    const int dirCount = int(u32(d, 18));
    const size_t dirOffset = u32(d, 26);
    if (dirCount <= 0 || dirOffset + size_t(dirCount) * 28 > d.size())
        return fail("the trace file's directory is damaged");
    std::map<std::pair<std::string, int>, Entry> tags;
    for (int i = 0; i < dirCount; ++i) {
        const size_t at = dirOffset + size_t(i) * 28;
        Entry e;
        const std::string name = d.substr(at, 4);
        const int number = int(u32(d, at + 4));
        e.type = u16(d, at + 8);
        e.elementSize = u16(d, at + 10);
        e.count = int(u32(d, at + 12));
        const uint32_t size = u32(d, at + 16);
        // Data of four bytes or less sits in the offset field itself.
        const size_t dataAt = size <= 4 ? at + 20 : u32(d, at + 20);
        if (dataAt + size > d.size())
            continue;
        e.data = d.substr(dataAt, size);
        tags[{name, number}] = std::move(e);
    }
    auto find = [&](const char *name, int number) -> const Entry * {
        auto it = tags.find({name, number});
        return it == tags.end() ? nullptr : &it->second;
    };
    Trace t;
    const Entry *pbas = find("PBAS", 2) ? find("PBAS", 2) : find("PBAS", 1);
    if (!pbas)
        return fail("the trace file has no base calls");
    t.bases = cleanBases(text(*pbas));
    if (const Entry *smpl = find("SMPL", 1))
        t.name = text(*smpl);
    if (const Entry *pcon = find("PCON", 2) ? find("PCON", 2) : find("PCON", 1))
        for (char c : pcon->data)
            t.quality.push_back(int(uint8_t(c)));
    if (const Entry *ploc = find("PLOC", 2) ? find("PLOC", 2) : find("PLOC", 1))
        t.peaks = shorts(*ploc);
    // DATA 9..12 are the analysed channels in the order FWO_ names them.
    std::string order = "GATC";
    if (const Entry *fwo = find("FWO_", 1); fwo && fwo->data.size() >= 4)
        order = fwo->data.substr(0, 4);
    for (int k = 0; k < 4; ++k) {
        const Entry *data = find("DATA", 9 + k);
        const size_t slot = std::string("ACGT").find(char(std::toupper(static_cast<unsigned char>(order[size_t(k)]))));
        if (data && slot != std::string::npos)
            t.channels[slot] = shorts(*data);
    }
    if (t.quality.size() != t.bases.size())
        t.quality.clear();
    if (t.peaks.size() != t.bases.size())
        t.peaks.clear();
    *out = std::move(t);
    return true;
}

Span goodRegion(const Trace &trace, int minQuality, int window)
{
    const int n = int(trace.bases.size());
    if (trace.quality.empty())
        return {0, n};
    window = std::max(1, std::min(window, n));
    auto good = [&](int at) {
        int sum = 0;
        for (int i = at; i < at + window; ++i)
            sum += trace.quality[size_t(i)];
        return sum >= minQuality * window;
    };
    int start = 0;
    while (start + window <= n && !good(start))
        ++start;
    if (start + window > n)
        return {0, 0};
    int end = n - window;
    while (end > start && !good(end))
        --end;
    return {start, end + window - start};
}

} // namespace oc
