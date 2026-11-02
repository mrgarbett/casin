// SPDX-License-Identifier: GPL-3.0-or-later
#include "xml.h"

#include <cctype>
#include <cstdlib>

namespace oc::xml {

const std::string *Element::attribute(const std::string &key) const
{
    for (const auto &kv : attributes)
        if (kv.first == key)
            return &kv.second;
    return nullptr;
}

std::string Element::attr(const std::string &key, const std::string &fallback) const
{
    const std::string *v = attribute(key);
    return v ? *v : fallback;
}

const Element *Element::child(const std::string &n) const
{
    for (const auto &c : children)
        if (c->name == n)
            return c.get();
    return nullptr;
}

std::vector<const Element *> Element::childrenNamed(const std::string &n) const
{
    std::vector<const Element *> out;
    for (const auto &c : children)
        if (c->name == n)
            out.push_back(c.get());
    return out;
}

static void appendUtf8(std::string &out, unsigned long cp)
{
    if (cp < 0x80) {
        out.push_back(char(cp));
    } else if (cp < 0x800) {
        out.push_back(char(0xC0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(char(0xE0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    } else if (cp < 0x110000) {
        out.push_back(char(0xF0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(char(0x80 | (cp & 0x3F)));
    }
}

std::string decodeEntities(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out.push_back(s[i]);
            continue;
        }
        size_t semi = s.find(';', i);
        if (semi == std::string::npos || semi - i > 10) {
            out.push_back('&');
            continue;
        }
        std::string ent = s.substr(i + 1, semi - i - 1);
        if (ent == "amp") out.push_back('&');
        else if (ent == "lt") out.push_back('<');
        else if (ent == "gt") out.push_back('>');
        else if (ent == "quot") out.push_back('"');
        else if (ent == "apos") out.push_back('\'');
        else if (ent == "nbsp") out.push_back(' ');
        else if (!ent.empty() && ent[0] == '#') {
            unsigned long cp = (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X'))
                                   ? std::strtoul(ent.c_str() + 2, nullptr, 16)
                                   : std::strtoul(ent.c_str() + 1, nullptr, 10);
            if (cp > 0)   // "&#;" would be a NUL byte in a name
                appendUtf8(out, cp);
        } else {
            out.append(s, i, semi - i + 1);   // unknown entity: keep literally
        }
        i = semi;
    }
    return out;
}

std::string stripHtml(const std::string &s)
{
    std::string noTags;
    bool inTag = false;
    for (char c : s) {
        if (c == '<') {
            inTag = true;
            noTags.push_back(' ');
        } else if (c == '>' && inTag) {
            inTag = false;
        } else if (!inTag) {
            noTags.push_back(c);
        }
    }
    std::string decoded = decodeEntities(noTags);
    std::string out;
    bool space = false;
    for (char c : decoded) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            space = !out.empty();
        } else {
            if (space)
                out.push_back(' ');
            space = false;
            out.push_back(c);
        }
    }
    return out;
}

namespace {

class Parser {
public:
    explicit Parser(const std::string &s) : m_s(s) {}

    std::unique_ptr<Element> run(std::string *error)
    {
        std::unique_ptr<Element> root;
        skipProlog();
        if (m_pos < m_s.size() && m_s[m_pos] == '<')
            root = parseElement(0);
        if (!root && error)
            *error = m_error.empty() ? "no root element" : m_error;
        return root;
    }

private:
    bool startsWithAt(const char *t) const { return m_s.compare(m_pos, std::char_traits<char>::length(t), t) == 0; }

    void skipSpace()
    {
        while (m_pos < m_s.size() && std::isspace(static_cast<unsigned char>(m_s[m_pos])))
            ++m_pos;
    }

    bool skipPast(const char *t)
    {
        size_t p = m_s.find(t, m_pos);
        if (p == std::string::npos) {
            m_pos = m_s.size();
            return false;
        }
        m_pos = p + std::char_traits<char>::length(t);
        return true;
    }

    void skipProlog()
    {
        for (;;) {
            skipSpace();
            if (startsWithAt("<?"))
                skipPast("?>");
            else if (startsWithAt("<!--"))
                skipPast("-->");
            else if (startsWithAt("<!"))
                skipPast(">");
            else
                return;
        }
    }

    std::string parseName()
    {
        size_t start = m_pos;
        while (m_pos < m_s.size()) {
            char c = m_s[m_pos];
            if (std::isspace(static_cast<unsigned char>(c)) || c == '/' || c == '>' || c == '=')
                break;
            ++m_pos;
        }
        return m_s.substr(start, m_pos - start);
    }

    std::unique_ptr<Element> parseElement(int depth)
    {
        if (depth > 200) {
            m_error = "XML nested too deeply";
            return nullptr;
        }
        ++m_pos;   // '<'
        auto el = std::make_unique<Element>();
        el->name = parseName();
        if (el->name.empty()) {
            m_error = "bad element name";
            return nullptr;
        }
        for (;;) {
            skipSpace();
            if (m_pos >= m_s.size()) {
                m_error = "unexpected end in tag";
                return nullptr;
            }
            if (startsWithAt("/>")) {
                m_pos += 2;
                return el;
            }
            if (m_s[m_pos] == '>') {
                ++m_pos;
                break;
            }
            std::string key = parseName();
            skipSpace();
            std::string value;
            if (m_pos < m_s.size() && m_s[m_pos] == '=') {
                ++m_pos;
                skipSpace();
                if (m_pos >= m_s.size())
                    return nullptr;
                char q = m_s[m_pos];
                if (q == '"' || q == '\'') {
                    size_t end = m_s.find(q, m_pos + 1);
                    if (end == std::string::npos) {
                        m_error = "unterminated attribute";
                        return nullptr;
                    }
                    value = decodeEntities(m_s.substr(m_pos + 1, end - m_pos - 1));
                    m_pos = end + 1;
                } else {
                    value = parseName();
                }
            }
            if (key.empty()) {
                m_error = "bad attribute";
                return nullptr;
            }
            el->attributes.emplace_back(key, value);
        }
        // Content
        for (;;) {
            if (m_pos >= m_s.size()) {
                m_error = "unexpected end of document in <" + el->name + ">";
                return nullptr;
            }
            if (startsWithAt("</")) {
                m_pos += 2;
                std::string closing = parseName();
                skipPast(">");
                if (closing != el->name) {
                    m_error = "mismatched </" + closing + ">";
                    return nullptr;
                }
                return el;
            }
            if (startsWithAt("<!--")) {
                skipPast("-->");
            } else if (startsWithAt("<![CDATA[")) {
                m_pos += 9;
                size_t end = m_s.find("]]>", m_pos);
                if (end == std::string::npos)
                    return nullptr;
                el->text += m_s.substr(m_pos, end - m_pos);
                m_pos = end + 3;
            } else if (startsWithAt("<?")) {
                skipPast("?>");
            } else if (m_s[m_pos] == '<') {
                auto c = parseElement(depth + 1);
                if (!c)
                    return nullptr;
                el->children.push_back(std::move(c));
            } else {
                size_t end = m_s.find('<', m_pos);
                if (end == std::string::npos)
                    end = m_s.size();
                el->text += decodeEntities(m_s.substr(m_pos, end - m_pos));
                m_pos = end;
            }
        }
    }

    const std::string &m_s;
    size_t m_pos = 0;
    std::string m_error;
};

} // namespace

std::unique_ptr<Element> parse(const std::string &text, std::string *error)
{
    return Parser(text).run(error);
}

} // namespace oc::xml
