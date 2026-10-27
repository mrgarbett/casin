// SPDX-License-Identifier: GPL-3.0-or-later
// A deliberately small XML reader: elements, attributes, text, CDATA and the
// predefined/numeric entities. Enough for the XML blocks inside .dna files.
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace oc::xml {

struct Element {
    std::string name;
    std::vector<std::pair<std::string, std::string>> attributes;
    std::vector<std::unique_ptr<Element>> children;
    std::string text;   // concatenated character data directly inside this element

    const std::string *attribute(const std::string &key) const;
    std::string attr(const std::string &key, const std::string &fallback = {}) const;
    const Element *child(const std::string &name) const;
    std::vector<const Element *> childrenNamed(const std::string &name) const;
};

// Returns the root element, or nullptr (with *error set) on malformed input.
std::unique_ptr<Element> parse(const std::string &text, std::string *error = nullptr);

// Decodes &amp; &lt; &gt; &quot; &apos; &#NN; &#xNN; (UTF-8 output).
std::string decodeEntities(const std::string &s);
// Removes HTML tags, decodes entities, collapses whitespace.
std::string stripHtml(const std::string &s);

} // namespace oc::xml
