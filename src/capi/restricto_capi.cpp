// SPDX-License-Identifier: GPL-3.0-or-later
#include "include/restricto.h"

#include "core/version.h"
#include "core/align.h"
#include "core/cloning.h"
#include "core/digest.h"
#include "core/edit.h"
#include "core/orf.h"
#include "core/pcrprogram.h"
#include "core/protein.h"
#include "core/primercheck.h"
#include "core/search.h"
#include "core/seqio.h"
#include "view/document.h"
#include "view/gellayout.h"
#include "view/maplayout.h"
#include "view/printlayout.h"
#include "view/seqlayout.h"
#include "view/style.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <set>

using namespace oc;

namespace {

char *dupString(const std::string &s)
{
    char *out = static_cast<char *>(std::malloc(s.size() + 1));
    if (out)
        std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

std::vector<Enzyme> enzymesNamed(const char *const *names, int count)
{
    std::vector<Enzyme> out;
    for (int i = 0; i < count && names; ++i)
        if (const Enzyme *e = names[i] ? findEnzyme(names[i]) : nullptr)
            out.push_back(*e);
    return out;
}

std::string joinLines(const std::vector<std::string> &lines)
{
    std::string out;
    for (const std::string &l : lines)
        out += (out.empty() ? "" : "\n") + l;
    return out;
}

bool failWith(char **error, const std::string &err)
{
    if (error)
        *error = dupString(err);
    return false;
}

OCColor toC(const view::Color &c)
{
    return {c.r, c.g, c.b, c.a};
}

// Calls back into the frontend for text widths.
class CallbackMeasurer : public view::TextMeasurer {
public:
    CallbackMeasurer(OCMeasureText fn, void *ctx) : m_fn(fn), m_ctx(ctx) {}
    double width(const std::string &text, const view::Font &font) override
    {
        if (!m_fn)
            return m_approx.width(text, font);
        return m_fn(m_ctx, text.c_str(), font.size, font.bold, font.mono);
    }

private:
    OCMeasureText m_fn;
    void *m_ctx;
    view::ApproxTextMeasurer m_approx;
};

Strand strandFromC(int strand)
{
    return strand == OC_STRAND_FORWARD ? Strand::Forward : strand == OC_STRAND_REVERSE ? Strand::Reverse : Strand::None;
}

// A footprint as feature ranges, split at the origin when it wraps.
std::vector<Range> rangesFor(const Sequence &s, int start, int length)
{
    const int n = s.length();
    if (n == 0 || length <= 0)
        return {};
    length = std::min(length, n);
    start = s.circular ? ((start % n) + n) % n : std::clamp(start, 0, n);
    if (start + length <= n)
        return {{start, start + length}};
    if (!s.circular)
        return {{start, n}};
    return {{start, n}, {0, start + length - n}};
}



} // namespace

struct OCDocument {
    Document doc;
    OCChangeCallback cb = nullptr;
    void *ctx = nullptr;
    std::string scratch1, scratch2, scratch3, scratch4;   // backing for const char * results
};

struct OCMapLayout {
    std::unique_ptr<view::MapLayout> layout;
};

struct OCSeqLayout {
    std::unique_ptr<view::SequenceLayout> layout;
    int hoverCodon = -1;
};

struct OCScene {
    view::Scene scene;
};

namespace {
bool deliver(OCDocument *out, const Sequence &product)
{
    if (!out)
        return false;
    out->doc.setSequence(product);
    out->doc.markUnsaved();
    return true;
}
} // namespace

namespace {
std::vector<view::Scene> pagesFor(const OCDocument *doc, double w, double h, OCMeasureText measure, void *context)
{
    CallbackMeasurer tm(measure, context);
    view::PrintSettings ps;
    ps.width = w;
    ps.height = h;
    return view::printPages(doc->doc.sequence(), doc->doc.visibleSites(), doc->doc.reads(), tm, ps);
}

// "CDS, promoter" -> {"CDS", "promoter"}
std::set<std::string> typeSet(const char *list)
{
    std::set<std::string> out;
    std::string types = list ? list : "";
    size_t from = 0;
    while (from <= types.size()) {
        const size_t comma = types.find(',', from);
        const std::string t = trim(types.substr(from, comma == std::string::npos ? std::string::npos : comma - from));
        if (!t.empty())
            out.insert(t);
        if (comma == std::string::npos)
            break;
        from = comma + 1;
    }
    return out;
}

} // namespace

namespace {
// A site from oc_primer_sites in the core's terms.
PrimerSite coreSite(const OCPrimerSite &s)
{
    PrimerSite p;
    p.binding = {s.start, s.start + s.length};
    p.strand = s.strand == OC_STRAND_REVERSE ? Strand::Reverse : Strand::Forward;
    p.mismatches = s.mismatches;
    return p;
}
}

extern "C" {

const char *oc_version(void) { return RESTRICTO_VERSION; }

void oc_string_free(char *s)
{
    std::free(s);
}

// ---- document --------------------------------------------------------------

OCDocument *oc_document_new(void)
{
    auto *d = new OCDocument;
    d->doc.onSequenceChanged([d] { if (d->cb) d->cb(d->ctx, OC_CHANGED_SEQUENCE); });
    d->doc.onSelectionChanged([d] { if (d->cb) d->cb(d->ctx, OC_CHANGED_SELECTION); });
    d->doc.onEnzymesChanged([d] { if (d->cb) d->cb(d->ctx, OC_CHANGED_ENZYMES); });
    d->doc.onHistoryChanged([d] { if (d->cb) d->cb(d->ctx, OC_CHANGED_HISTORY); });
    d->doc.onPendingChanged([d] { if (d->cb) d->cb(d->ctx, OC_CHANGED_PENDING); });
    return d;
}

void oc_document_free(OCDocument *doc)
{
    delete doc;
}

void oc_document_set_callback(OCDocument *doc, OCChangeCallback cb, void *context)
{
    doc->cb = cb;
    doc->ctx = context;
}

bool oc_document_load(OCDocument *doc, const char *path, char **error)
{
    Sequence seq;
    std::string err;
    FileFormat format = FileFormat::Unknown;
    if (!loadSequenceFile(path, &seq, &err, &format)) {
        if (error)
            *error = dupString(err);
        return false;
    }
    // A trace can't be written back: saving goes to a new file.
    doc->doc.setSequence(std::move(seq), format == FileFormat::Abif ? std::string() : path);
    return true;
}

bool oc_document_save(OCDocument *doc, const char *path, int format, char **error)
{
    std::string err;
    const FileFormat f = format == OC_FORMAT_FASTA    ? FileFormat::Fasta
                       : format == OC_FORMAT_SNAPGENE ? FileFormat::SnapGene
                       : format == OC_FORMAT_EMBL     ? FileFormat::Embl
                                                      : FileFormat::GenBank;
    if (!saveSequenceFile(path, doc->doc.sequence(), f, &err)) {
        if (error)
            *error = dupString(err);
        return false;
    }
    if (f != FileFormat::Fasta) {
        doc->doc.setPath(path);
        doc->doc.markSaved();
    }
    return true;
}

bool oc_document_copy(const OCDocument *src, OCDocument *out)
{
    if (!src || !out)
        return false;
    out->doc.setSequence(src->doc.sequence());
    out->doc.markUnsaved();
    out->doc.setSelection(src->doc.selection());
    return true;
}

bool oc_document_new_sequence(OCDocument *doc, const char *name, const char *bases, bool circular)
{
    Sequence seq;
    seq.bases = cleanBases(bases ? bases : "");
    if (seq.bases.empty())
        return false;
    seq.name = trim(name ? name : "");
    if (seq.name.empty())
        seq.name = "Untitled";
    seq.circular = circular;
    doc->doc.setSequence(std::move(seq));
    doc->doc.markUnsaved();
    return true;
}

bool oc_document_is_empty(const OCDocument *doc) { return doc->doc.isEmpty(); }

const char *oc_document_name(OCDocument *doc)
{
    doc->scratch1 = doc->doc.sequence().name;
    return doc->scratch1.c_str();
}

const char *oc_document_description(OCDocument *doc)
{
    doc->scratch1 = doc->doc.sequence().description;
    return doc->scratch1.c_str();
}

int oc_document_length(const OCDocument *doc) { return doc->doc.sequence().length(); }
bool oc_document_is_circular(const OCDocument *doc) { return doc->doc.sequence().circular; }
char *oc_document_status_text(const OCDocument *doc) { return dupString(doc->doc.statusText()); }
char *oc_document_selection_text(const OCDocument *doc)
{
    std::string s = doc->doc.statusText(false);
    if (!s.empty())
        s[0] = char(std::toupper(static_cast<unsigned char>(s[0])));   // "Selected …", "Position …"
    return dupString(s);
}

int oc_document_feature_count(const OCDocument *doc) { return int(doc->doc.sequence().features.size()); }

bool oc_document_feature(OCDocument *doc, int index, OCFeatureInfo *out)
{
    const Sequence &s = doc->doc.sequence();
    if (index < 0 || index >= int(s.features.size()) || !out)
        return false;
    const Feature &f = s.features[size_t(index)];
    doc->scratch1 = f.name;
    doc->scratch2 = f.type;
    const Span sp = f.span(s.length(), s.circular);
    out->name = doc->scratch1.c_str();
    out->type = doc->scratch2.c_str();
    out->start = sp.start;
    out->length = sp.length;
    out->part_count = int(f.ranges.size());
    out->strand = f.strand == Strand::Forward ? OC_STRAND_FORWARD
                : f.strand == Strand::Reverse ? OC_STRAND_REVERSE
                                              : OC_STRAND_NONE;
    out->color = toC(view::FeatureStyle::fill(f.type, f.color));
    out->drawn = view::FeatureStyle::isDrawn(f);
    return true;
}

// ---- editing -------------------------------------------------------------------


bool oc_document_can_undo(const OCDocument *doc) { return doc->doc.canUndo(); }
bool oc_document_can_redo(const OCDocument *doc) { return doc->doc.canRedo(); }
char *oc_document_undo_label(const OCDocument *doc) { return dupString(doc->doc.undoLabel()); }
char *oc_document_redo_label(const OCDocument *doc) { return dupString(doc->doc.redoLabel()); }
void oc_document_undo(OCDocument *doc) { doc->doc.undo(); }
void oc_document_redo(OCDocument *doc) { doc->doc.redo(); }
bool oc_document_is_modified(const OCDocument *doc) { return doc->doc.isModified(); }

const char *oc_document_path(OCDocument *doc)
{
    doc->scratch1 = doc->doc.path();
    return doc->scratch1.c_str();
}

bool oc_document_type_bases(OCDocument *doc, const char *text) { return text && doc->doc.typeBases(text); }
void oc_document_delete_selection(OCDocument *doc) { doc->doc.deleteSelection(); }
void oc_document_delete_backward(OCDocument *doc) { doc->doc.deleteBackward(); }
void oc_document_delete_forward(OCDocument *doc) { doc->doc.deleteForward(); }
void oc_document_reverse_complement(OCDocument *doc) { doc->doc.reverseComplementAll(); }
void oc_document_set_origin_at_selection(OCDocument *doc) { doc->doc.setOriginAtSelection(); }
void oc_document_set_circular(OCDocument *doc, bool circular) { doc->doc.setCircular(circular); }
void oc_document_rename(OCDocument *doc, const char *name) { doc->doc.rename(name ? name : ""); }

int oc_document_add_feature(OCDocument *doc, const char *name, const char *type, int strand, int start, int length)
{
    Feature f;
    f.type = type && *type ? type : "misc_feature";
    f.strand = strandFromC(strand);
    f.ranges = rangesFor(doc->doc.sequence(), start, length);
    f.name = trim(name ? name : "");
    if (f.name.empty())
        f.name = defaultFeatureName(f.type, f.qualifiers);
    else
        f.qualifiers.push_back({"label", f.name});
    return doc->doc.addFeature(f);
}

void oc_document_update_feature(OCDocument *doc, int index, const char *name, const char *type, int strand,
                                int start, int length)
{
    const Sequence &s = doc->doc.sequence();
    if (index < 0 || index >= int(s.features.size()))
        return;
    Feature f = s.features[size_t(index)];
    if (type && *type)
        f.type = type;
    f.strand = strandFromC(strand);
    const Span old = f.span(s.length(), s.circular);
    if (old.start != start || old.length != length)
        f.ranges = rangesFor(s, start, length);
    const std::string clean = trim(name ? name : "");
    if (!clean.empty() && clean != f.name) {
        f.name = clean;
        auto label = std::find_if(f.qualifiers.begin(), f.qualifiers.end(),
                                  [](const Qualifier &q) { return q.key == "label"; });
        if (label != f.qualifiers.end())
            label->value = clean;
        else
            f.qualifiers.insert(f.qualifiers.begin(), {"label", clean});
    }
    doc->doc.replaceFeature(index, f);
}

void oc_document_set_feature_color(OCDocument *doc, int index, const char *color)
{
    const Sequence &s = doc->doc.sequence();
    if (index < 0 || index >= int(s.features.size()))
        return;
    Feature f = s.features[size_t(index)];
    const std::string c = color ? color : "";
    if (c == f.color)
        return;
    f.color = c;
    doc->doc.replaceFeature(index, f);
}

void oc_document_remove_feature(OCDocument *doc, int index) { doc->doc.removeFeature(index); }

int oc_document_annotate_common(OCDocument *doc) { return doc->doc.annotateCommonFeatures(); }

int oc_document_primer_count(const OCDocument *doc) { return int(doc->doc.sequence().primers.size()); }

int oc_document_history_count(const OCDocument *doc) { return int(doc->doc.sequence().history.size()); }

const char *oc_document_history(OCDocument *doc, int index)
{
    const std::vector<std::string> &h = doc->doc.sequence().history;
    doc->scratch1 = index >= 0 && index < int(h.size()) ? h[size_t(index)] : std::string();
    return doc->scratch1.c_str();
}

bool oc_document_primer(OCDocument *doc, int index, OCPrimerInfo *out)
{
    const Sequence &s = doc->doc.sequence();
    if (index < 0 || index >= int(s.primers.size()) || !out)
        return false;
    const Primer &p = s.primers[size_t(index)];
    doc->scratch1 = p.name;
    doc->scratch2 = p.bases;
    out->name = doc->scratch1.c_str();
    out->bases = doc->scratch2.c_str();
    out->start = p.binding.start;
    out->length = p.binding.length();
    out->strand = p.strand == Strand::Reverse ? OC_STRAND_REVERSE : OC_STRAND_FORWARD;
    doc->scratch3 = p.color;
    out->color = doc->scratch3.c_str();
    doc->scratch4 = p.description;
    out->description = doc->scratch4.c_str();
    return true;
}

int oc_document_add_primer_from_selection(OCDocument *doc, const char *name, int strand)
{
    return doc->doc.addPrimerFromSelection(name ? name : "", strand == OC_STRAND_REVERSE ? Strand::Reverse
                                                                                          : Strand::Forward);
}

void oc_document_remove_primer(OCDocument *doc, int index) { doc->doc.removePrimer(index); }

int oc_document_add_primer_sequence(OCDocument *doc, const char *name, const char *bases, char **error)
{
    // A primer typed in mixed case keeps it (lowercase can mark a tail);
    // the binding search is on uppercase.
    const std::string typed = cleanPrimerBases(bases ? bases : "");
    const std::string b = cleanBases(typed);
    const std::vector<PrimerSite> sites = findPrimerSites(doc->doc.sequence(), b);
    if (b.size() < 15 || sites.empty()) {
        failWith(error, b.size() < 15 ? "a primer needs at least 15 bases" : "the primer doesn't bind this sequence");
        return -1;
    }
    Primer p;
    p.name = name && *name ? name : "primer";
    p.bases = typed;
    p.binding = sites.front().binding;
    p.strand = sites.front().strand;
    doc->doc.addPrimers({p}, "Add Primer");
    return int(doc->doc.sequence().primers.size()) - 1;
}

int oc_primer_sites(const OCDocument *doc, const char *bases, OCPrimerSite *out, int max)
{
    const std::string b = cleanBases(cleanPrimerBases(bases ? bases : ""));
    if (b.empty())
        return 0;
    const std::vector<PrimerSite> sites = findPrimerSites(doc->doc.sequence(), b);
    for (int i = 0; out && i < max && i < int(sites.size()); ++i)
        out[i] = {sites[size_t(i)].binding.start, sites[size_t(i)].binding.length(),
                  sites[size_t(i)].strand == Strand::Reverse ? OC_STRAND_REVERSE : OC_STRAND_FORWARD,
                  sites[size_t(i)].mismatches};
    return int(sites.size());
}

namespace {
// A primer at a site from oc_primer_sites, or false for an empty primer or bad site.
bool primerAt(const OCDocument *doc, const char *name, const char *bases, const char *color,
              const char *description, const OCPrimerSite *site, Primer *out)
{
    const std::string typed = cleanPrimerBases(bases ? bases : "");
    const int n = doc->doc.sequence().length();
    if (typed.empty() || !site || site->length <= 0 || site->start < 0 || site->start + site->length > n
        || site->length > int(typed.size()))
        return false;
    out->name = name && *name ? name : "primer";
    out->bases = typed;
    out->binding = {site->start, site->start + site->length};
    out->strand = site->strand == OC_STRAND_REVERSE ? Strand::Reverse : Strand::Forward;
    out->color = color ? color : "";
    out->description = description ? description : "";
    return true;
}
}

int oc_document_add_primer_at(OCDocument *doc, const char *name, const char *bases, const char *color,
                              const char *description, const OCPrimerSite *site)
{
    Primer p;
    if (!primerAt(doc, name, bases, color, description, site, &p))
        return -1;
    doc->doc.addPrimers({p}, "Add Primer");
    return int(doc->doc.sequence().primers.size()) - 1;
}

bool oc_document_set_pending_primer(OCDocument *doc, const char *name, const char *bases, const char *color,
                                    const OCPrimerSite *site)
{
    Primer p;
    if (!primerAt(doc, name, bases, color, nullptr, site, &p)) {
        doc->doc.clearPendingPrimer();
        return false;
    }
    doc->doc.setPendingPrimer(p);
    return true;
}

void oc_document_clear_pending_primer(OCDocument *doc) { doc->doc.clearPendingPrimer(); }

char *oc_primer_fit_tm(const OCDocument *doc, const char *bases, const OCPrimerSite *site, double target_tm,
                       OCPrimerSite *fitted)
{
    const std::string primer = cleanPrimerBases(bases ? bases : "");
    if (!site) {
        if (fitted)
            *fitted = {};
        return dupString(primer);
    }
    PrimerSite out;
    const std::string result = fitAnnealingToTm(doc->doc.sequence(), primer, coreSite(*site), target_tm, &out);
    if (fitted)
        *fitted = {out.binding.start, out.binding.length(),
                   out.strand == Strand::Reverse ? OC_STRAND_REVERSE : OC_STRAND_FORWARD, site->mismatches};
    return dupString(result);
}

int oc_pcr_product_length(const OCDocument *doc, const OCPrimerSite *a, int tail_a, const OCPrimerSite *b,
                          int tail_b)
{
    if (!a || !b)
        return -1;
    return pcrProductLength(doc->doc.sequence(), coreSite(*a), tail_a, coreSite(*b), tail_b);
}

void oc_primer_part_frame(const OCDocument *doc, const OCPrimerSite *site, int tail_length, int part_start,
                          int part_length, bool part_forward, OCFrameCheck *out)
{
    if (!out)
        return;
    *out = {};
    if (!site)
        return;
    const FrameCheck f =
        checkPartFrame(doc->doc.sequence(), coreSite(*site), tail_length, part_start, part_length, part_forward);
    out->applies = f.applies;
    out->against_cds = f.againstCds;
    out->in_frame = f.inFrame;
    out->fix = f.fix;
    std::snprintf(out->cds, sizeof out->cds, "%s", f.cds.c_str());
}
bool oc_document_has_pending_primer(const OCDocument *doc) { return doc->doc.hasPendingPrimer(); }

int oc_document_enzyme_count(const OCDocument *doc) { return int(doc->doc.enzymes().size()); }

bool oc_document_enzyme(OCDocument *doc, int index, OCEnzymeInfo *out)
{
    const auto &list = doc->doc.enzymes();
    if (index < 0 || index >= int(list.size()) || !out)
        return false;
    const Enzyme &e = list[size_t(index)];
    out->name = e.name.c_str();   // stable: owned by the document's enzyme list
    out->site = e.site.c_str();
    out->cuts = doc->doc.cutCount(e.name);
    out->hidden = doc->doc.isEnzymeHidden(e.name);
    out->passes_filter = doc->doc.enzymePassesFilter(e.name);
    int blocked = 0, kinds = MethylNone;
    for (const CutSite &s : doc->doc.allSites())
        if (s.enzyme && s.enzyme->name == e.name) {
            const int m = methylationBlock(doc->doc.sequence(), s);
            blocked += m != MethylNone;
            kinds |= m;
        }
    out->methylation_blocked = blocked;
    static const char *const names[] = {"", "Dam", "Dcm", "Dam and Dcm"};
    out->methylation = names[kinds & 3];
    return true;
}

void oc_document_set_enzyme_hidden(OCDocument *doc, const char *name, bool hidden)
{
    doc->doc.setEnzymeHidden(name, hidden);
}

int oc_document_max_cuts(const OCDocument *doc) { return doc->doc.maxCuts(); }
void oc_document_set_max_cuts(OCDocument *doc, int max_cuts) { doc->doc.setMaxCuts(max_cuts); }
int oc_document_visible_site_count(const OCDocument *doc) { return int(doc->doc.visibleSites().size()); }

const char *oc_document_visible_site_enzyme(const OCDocument *doc, int index)
{
    const auto &v = doc->doc.visibleSites();
    if (index < 0 || index >= int(v.size()))
        return nullptr;
    return v[size_t(index)].enzyme->name.c_str();
}

bool oc_document_has_selection(const OCDocument *doc) { return doc->doc.hasSelection(); }
int oc_document_selection_start(const OCDocument *doc) { return doc->doc.selection().start; }
int oc_document_selection_length(const OCDocument *doc) { return doc->doc.selection().length; }
void oc_document_set_selection(OCDocument *doc, int start, int length) { doc->doc.setSelection({start, length}); }
void oc_document_clear_selection(OCDocument *doc) { doc->doc.clearSelection(); }
char *oc_document_selected_bases(const OCDocument *doc) { return dupString(doc->doc.selectedBases()); }

char *oc_document_selected_reverse_complement(const OCDocument *doc)
{
    return dupString(reverseComplement(doc->doc.selectedBases()));
}

int oc_document_active_feature(const OCDocument *doc) { return doc->doc.activeFeature(); }
int oc_document_active_site(const OCDocument *doc) { return doc->doc.activeSite(); }
void oc_document_activate_feature(OCDocument *doc, int index) { doc->doc.activateFeature(index); }
void oc_document_activate_site(OCDocument *doc, int visible_index) { doc->doc.activateSite(visible_index); }

int oc_document_find_next(OCDocument *doc, const char *query, int *count)
{
    return doc->doc.findNext(query ? query : "", count);
}

// ---- layouts --------------------------------------------------------------------

/* ---- digest and gel ---- */

int oc_digest(const OCDocument *doc, const char *const *enzymes, int enzyme_count, OCFragmentInfo *out, int max)
{
    const DigestResult r = digest(doc->doc.sequence(), enzymesNamed(enzymes, enzyme_count));
    for (int i = 0; i < int(r.fragments.size()) && i < max && out; ++i) {
        const DigestFragment &f = r.fragments[size_t(i)];
        OCFragmentInfo &o = out[i];
        o.start = f.start;
        o.length = f.length;
        std::snprintf(o.left_enzyme, sizeof o.left_enzyme, "%s", f.leftEnzyme.c_str());
        std::snprintf(o.right_enzyme, sizeof o.right_enzyme, "%s", f.rightEnzyme.c_str());
        o.left_overhang = f.leftOverhang;
        o.right_overhang = f.rightOverhang;
    }
    return int(r.fragments.size());
}

int oc_ladder_count(void) { return int(commonLadders().size()); }

const char *oc_ladder_name(int index)
{
    return index >= 0 && index < oc_ladder_count() ? commonLadders()[size_t(index)].name.c_str() : "";
}

OCScene *oc_gel_render(const OCDocument *doc, const char *const *enzymes, int enzyme_count, int ladder, double width,
                       double height, bool dark, OCMeasureText measure, void *context)
{
    const Sequence &seq = doc->doc.sequence();
    const Ladder &l = commonLadders()[size_t(std::clamp(ladder, 0, oc_ladder_count() - 1))];
    const std::vector<Enzyme> use = enzymesNamed(enzymes, enzyme_count);
    std::vector<view::GelLane> lanes;
    lanes.push_back({"Ladder", l.bands, l.bright, true, false, {}});
    lanes.push_back({"Uncut", {seq.length()}, {}, false, seq.circular, {}});
    view::GelLane cut{"Digest", {}, {}, false, false, {}};
    if (!use.empty()) {
        const DigestResult r = digest(seq, use);
        cut.uncutCircular = r.uncut && seq.circular;
        for (const DigestFragment &f : r.fragments)
            cut.bands.push_back(f.length);
    }
    lanes.push_back(cut);
    CallbackMeasurer tm(measure, context);
    view::GelLayout layout(lanes, dark ? view::Theme::darkTheme() : view::Theme::light(), tm, width, height);
    return new OCScene{layout.render()};
}

/* ---- primers ---- */

char *oc_primer_check(const char *bases, OCPrimerCheck *out)
{
    const PrimerCheck c = checkPrimer(bases ? bases : "");
    if (out)
        *out = {c.length, c.gc, c.tm, c.clampGC, c.longestRun, c.selfDimer, c.selfDimer3, c.hairpinStem};
    return dupString(joinLines(c.warnings));
}

namespace {
Polymerase polymeraseOf(int p)
{
    return p == OC_POLYMERASE_PHUSION ? Polymerase::Phusion : p == OC_POLYMERASE_TAQ ? Polymerase::Taq
                                                                                      : Polymerase::Q5;
}
} // namespace

void oc_pcr_program(const char *anneal1, const char *anneal2, int length, int polymerase, int cycles,
                    OCPcrProgram *out)
{
    if (!out)
        return;
    const PcrProgram p = pcrProgram(anneal1 ? anneal1 : "", anneal2 ? anneal2 : "", length,
                                    polymeraseOf(polymerase), cycles);
    *out = {};
    out->tm1 = p.tm1;
    out->tm2 = p.tm2;
    out->annealing = p.annealing;
    out->capped = p.capped;
    out->two_step = p.twoStep;
    out->cycles = p.cycles;
    for (const PcrStep &st : p.steps) {
        if (out->step_count >= int(std::size(out->steps)))
            break;
        OCPcrStep &o = out->steps[out->step_count++];
        std::snprintf(o.name, sizeof o.name, "%s", st.name.c_str());
        o.celsius = st.celsius;
        o.seconds = st.seconds;
        o.cycled = st.cycled;
    }
}

char *oc_pcr_program_markdown(const char *anneal1, const char *anneal2, int length, int polymerase, int cycles,
                              const char *title)
{
    const PcrProgram p = pcrProgram(anneal1 ? anneal1 : "", anneal2 ? anneal2 : "", length,
                                    polymeraseOf(polymerase), cycles);
    return dupString(pcrProgramMarkdown(p, title ? title : ""));
}

char *oc_polymerase_name(int polymerase) { return dupString(polymeraseName(polymeraseOf(polymerase))); }
int oc_polymerase_max_length(int polymerase) { return polymeraseMaxLength(polymeraseOf(polymerase)); }
char *oc_format_seconds(int seconds) { return dupString(formatSeconds(seconds)); }

char *oc_primer_pair_check(const char *a, const char *b)
{
    return dupString(joinLines(checkPrimerPair(a ? a : "", b ? b : "").warnings));
}

bool oc_document_design_primers(OCDocument *doc, double target_tm, char **error)
{
    if (!doc->doc.hasSelection() || doc->doc.selection().length <= 0)
        return failWith(error, "select the region to amplify");
    PrimerDesign d;
    std::string err;
    if (!designPrimers(doc->doc.sequence(), doc->doc.selection(), target_tm, &d, &err))
        return failWith(error, err);
    doc->doc.addPrimers({d.forward, d.reverse}, "Add Primers");
    return true;
}

bool oc_document_design_primers_flank(OCDocument *doc, double target_tm, int flank, char **error)
{
    if (!doc->doc.hasSelection() || doc->doc.selection().length <= 0)
        return failWith(error, "select the region to amplify");
    PrimerDesign d;
    std::string err;
    if (!designPrimers(doc->doc.sequence(), doc->doc.selection(), target_tm, &d, &err, std::max(0, flank)))
        return failWith(error, err);
    doc->doc.addPrimers({d.forward, d.reverse}, "Add Primers");
    return true;
}

/* ---- ORFs ---- */

char *oc_document_selection_protein(const OCDocument *doc, OCProteinInfo *out)
{
    const Span sel = doc->doc.selection();
    if (sel.length < 3)
        return nullptr;
    const Sequence &seq = doc->doc.sequence();
    std::string bases = doc->doc.selectedBases();
    const int fi = doc->doc.activeFeature();
    if (fi >= 0 && size_t(fi) < seq.features.size() && seq.features[size_t(fi)].span(seq.length(), seq.circular) == sel)
        bases = seq.featureBases(seq.features[size_t(fi)]);
    std::string protein = translate(bases);
    const ProteinStats ps = proteinStats(protein);
    if (out)
        *out = {ps.length, ps.unknown, ps.truncated, ps.mass, ps.pI, ps.extinctionReduced, ps.extinctionCystines,
                ps.positive, ps.negative};
    if (!protein.empty() && protein.back() == '*')
        protein.pop_back();
    return dupString(protein);
}

char *oc_find_orfs(const OCDocument *doc, int min_codons)
{
    std::vector<std::string> lines;
    for (const Orf &o : findOrfs(doc->doc.sequence(), min_codons))
        lines.push_back(std::to_string(o.start) + "\t" + std::to_string(o.length) + "\t"
                        + (o.strand == Strand::Reverse ? "-1" : "1") + "\t" + o.protein);
    return dupString(joinLines(lines));
}

int oc_document_add_orf(OCDocument *doc, int min_codons, int index, const char *name)
{
    const std::vector<Orf> orfs = findOrfs(doc->doc.sequence(), min_codons);
    if (index < 0 || index >= int(orfs.size()))
        return -1;
    return doc->doc.addFeature(orfFeature(doc->doc.sequence(), orfs[size_t(index)], name ? name : "ORF"));
}

/* ---- enzyme sets ---- */

char *oc_builtin_enzyme_sets(void)
{
    std::vector<std::string> lines;
    for (const EnzymeSet &set : builtinEnzymeSets()) {
        std::string l = set.name + "\t";
        for (size_t i = 0; i < set.enzymes.size(); ++i)
            l += (i ? "," : "") + set.enzymes[i];
        lines.push_back(l);
    }
    return dupString(joinLines(lines));
}

char *oc_enzyme_suppliers(const char *enzyme) { return dupString(enzymeSuppliers(enzyme ? enzyme : "")); }

void oc_document_show_enzyme_set(OCDocument *doc, const char *const *enzymes, int count)
{
    std::set<std::string> names;
    for (int i = 0; i < count; ++i)
        if (enzymes[i])
            names.insert(enzymes[i]);
    doc->doc.showOnlyEnzymes(names, doc->doc.maxCuts());
}

/* ---- printing ---- */


int oc_print_page_count(const OCDocument *doc, double page_width, double page_height, OCMeasureText measure,
                        void *context)
{
    return int(pagesFor(doc, page_width, page_height, measure, context).size());
}

OCScene *oc_print_page(const OCDocument *doc, double page_width, double page_height, int page, OCMeasureText measure,
                       void *context)
{
    std::vector<view::Scene> pages = pagesFor(doc, page_width, page_height, measure, context);
    if (page < 0 || page >= int(pages.size()))
        return nullptr;
    return new OCScene{std::move(pages[size_t(page)])};
}

/* ---- choosing enzymes ---- */

char *oc_document_choose_enzymes(const OCDocument *doc, int max_cuts, bool inside_selection, int spare_feature,
                                 int ends)
{
    const Sequence &seq = doc->doc.sequence();
    EnzymeCriteria c;
    c.maxCuts = max_cuts;
    c.ends = EnzymeCriteria::Ends(std::clamp(ends, 0, 3));
    if (inside_selection && doc->doc.hasSelection() && doc->doc.selection().length > 0) {
        c.inside = true;
        c.region = doc->doc.selection();
    }
    if (spare_feature >= 0 && spare_feature < int(seq.features.size()))
        for (const Range &r : seq.features[size_t(spare_feature)].ranges)
            c.spare.push_back({r.start, r.length()});
    return dupString(joinLines(chooseEnzymes(seq, doc->doc.enzymes(), doc->doc.allSites(), c)));
}

void oc_document_show_only_enzymes(OCDocument *doc, const char *const *names, int count, int max_cuts)
{
    std::set<std::string> set;
    for (int i = 0; i < count && names; ++i)
        if (names[i])
            set.insert(names[i]);
    doc->doc.showOnlyEnzymes(set, max_cuts);
}

/* ---- cloning ---- */


bool oc_pcr_product(const OCDocument *templ, int forward_primer, int reverse_primer, OCDocument *out, char **error)
{
    const Sequence &t = templ->doc.sequence();
    const int n = int(t.primers.size());
    if (forward_primer < 0 || forward_primer >= n || reverse_primer < 0 || reverse_primer >= n)
        return failWith(error, "pick two primers of this sequence");
    Sequence p;
    std::string err;
    if (!pcrProduct(t, t.primers[size_t(forward_primer)], t.primers[size_t(reverse_primer)], &p, &err))
        return failWith(error, err);
    return deliver(out, p);
}

bool oc_overhang_clone(const OCDocument *vector, const OCDocument *insert, char overhang, bool reversed,
                       OCDocument *out, char **error)
{
    Sequence p;
    std::string err;
    if (!overhangClone(vector->doc.sequence(), insert->doc.sequence(), overhang, reversed, &p, &err))
        return failWith(error, err);
    return deliver(out, p);
}

bool oc_restriction_clone(const OCDocument *vector, const char *vector_enzyme1, const char *vector_enzyme2,
                          const OCDocument *insert, const char *insert_enzyme1, const char *insert_enzyme2,
                          bool reversed, OCDocument *out, char **error)
{
    Sequence p;
    std::string err;
    auto str = [](const char *s) { return std::string(s ? s : ""); };
    if (!restrictionClone(vector->doc.sequence(), str(vector_enzyme1), str(vector_enzyme2), insert->doc.sequence(),
                          str(insert_enzyme1), str(insert_enzyme2), reversed, &p, &err))
        return failWith(error, err);
    return deliver(out, p);
}

bool oc_golden_gate(const OCDocument *vector, const OCDocument *const *parts, int part_count, const char *enzyme,
                    OCDocument *out, char **error)
{
    const Enzyme *e = enzyme ? findEnzyme(enzyme) : nullptr;
    if (!e)
        return failWith(error, "unknown enzyme");
    std::vector<Sequence> inputs{vector->doc.sequence()};
    for (int i = 0; i < part_count && parts; ++i)
        inputs.push_back(parts[i]->doc.sequence());
    Sequence p;
    std::vector<AssemblyStep> steps;
    std::string err;
    if (!goldenGate(inputs, *e, &p, &steps, &err))
        return failWith(error, err);
    p.name = inputs[0].name;
    for (const AssemblyStep &s : steps)
        if (s.input > 0)
            p.name += "-" + inputs[size_t(s.input)].name;
    return deliver(out, p);
}

bool oc_mutagenesis(const OCDocument *doc, const char *bases, double target_tm, OCDocument *out, char **error)
{
    if (!doc->doc.hasSelection())
        return failWith(error, "select the bases to change, or place the caret");
    MutagenesisDesign d;
    std::string err;
    if (!designMutagenesis(doc->doc.sequence(), doc->doc.selection(), bases ? bases : "", target_tm, &d, &err))
        return failWith(error, err);
    return deliver(out, d.mutant);
}

bool oc_gibson(const OCDocument *vector, const char *vector_enzyme1, const char *vector_enzyme2,
               const OCDocument *const *fragments, int fragment_count, int min_overlap, OCDocument *out,
               char **error)
{
    std::vector<Sequence> frags;
    for (int i = 0; i < fragment_count && fragments; ++i)
        frags.push_back(fragments[i]->doc.sequence());
    Sequence p;
    std::string err;
    if (!gibsonIntoVector(vector->doc.sequence(), vector_enzyme1 ? vector_enzyme1 : "",
                          vector_enzyme2 ? vector_enzyme2 : "", frags, min_overlap, &p, nullptr, &err))
        return failWith(error, err);
    return deliver(out, p);
}

bool oc_overlap_extension(const OCDocument *const *fragments, int fragment_count, int min_overlap, OCDocument *out,
                          char **error)
{
    std::vector<Sequence> frags;
    for (int i = 0; i < fragment_count && fragments; ++i)
        frags.push_back(fragments[i]->doc.sequence());
    Sequence p;
    std::string err;
    if (!overlapExtension(frags, min_overlap, &p, nullptr, &err))
        return failWith(error, err);
    return deliver(out, p);
}

bool oc_insertion_by_pcr(const OCDocument *vector, const OCDocument *insert, double target_tm, int overlap,
                         OCDocument *out, char **primers, char **error)
{
    const Sequence &ins = insert->doc.sequence();
    Span target{0, ins.length()};
    const Span sel = insert->doc.selection();
    if (insert->doc.hasSelection() && sel.length >= 18 && !sel.wraps(ins.length()))
        target = sel;
    InsertionDesign d;
    std::string err;
    if (!designInsertion(vector->doc.sequence(), vector->doc.selection(), ins, target, target_tm, overlap, &d, &err))
        return failWith(error, err);
    if (primers)
        *primers = dupString(d.forward.name + "\t" + d.forward.bases + "\n" + d.reverse.name + "\t" + d.reverse.bases);
    return deliver(out, d.construct);
}

char *oc_search_folder(const char *folder, const char *query)
{
    std::vector<std::string> lines;
    for (const FolderHit &h : searchFolder(folder ? folder : "", query ? query : ""))
        lines.push_back(h.path + "\t" + h.name + "\t" + std::to_string(h.length) + "\t" + (h.circular ? "1" : "0") + "\t"
                        + h.match);
    return dupString(joinLines(lines));
}

char *oc_align_read(const OCDocument *reference, const OCDocument *read, OCAlignmentInfo *out, char **error)
{
    ReadAlignment a;
    std::string err;
    if (!alignRead(reference->doc.sequence(), read->doc.sequence().bases, &a, &err)) {
        failWith(error, err);
        return nullptr;
    }
    if (out)
        *out = {a.reverse, a.refSpan.start, a.refSpan.length, a.matches, a.mismatches, a.gaps, a.identity(),
                int(a.diffs.size())};
    std::vector<std::string> lines;
    for (const AlignmentDiff &d : a.diffs)
        lines.push_back(std::to_string(d.refPos + 1) + "\t" + d.ref + "\t" + d.read);
    return dupString(joinLines(lines));
}

bool oc_document_show_read(OCDocument *reference, const OCDocument *read, char **error)
{
    ReadAlignment a;
    std::string err;
    const Sequence &ref = reference->doc.sequence();
    if (!alignRead(ref, read->doc.sequence().bases, &a, &err)) {
        failWith(error, err);
        return false;
    }
    reference->doc.addRead(readTrack(a, ref.length(), read->doc.sequence().name));
    return true;
}

int oc_document_read_count(const OCDocument *doc) { return int(doc->doc.reads().size()); }
void oc_document_clear_reads(OCDocument *doc) { doc->doc.clearReads(); }

OCMapLayout *oc_map_layout_new(const OCDocument *doc, double width, double height, bool dark,
                               OCMeasureText measure, void *context)
{
    CallbackMeasurer tm(measure, context);
    auto *l = new OCMapLayout;
    l->layout = std::make_unique<view::MapLayout>(doc->doc.displaySequence(), doc->doc.visibleSites(),
                                                  dark ? view::Theme::darkTheme() : view::Theme::light(), tm,
                                                  width, height);
    return l;
}

OCMapLayout *oc_map_layout_new_options(const OCDocument *doc, double width, double height, bool dark,
                                       bool show_enzymes, bool show_primers, const char *hidden_types,
                                       OCMeasureText measure, void *context)
{
    return oc_map_layout_new_frames(doc, width, height, dark, show_enzymes, show_primers, hidden_types, 0, measure,
                                    context);
}

OCMapLayout *oc_map_layout_new_frames(const OCDocument *doc, double width, double height, bool dark,
                                      bool show_enzymes, bool show_primers, const char *hidden_types, int frames,
                                      OCMeasureText measure, void *context)
{
    return oc_map_layout_new_scaled(doc, width, height, dark, show_enzymes, show_primers, hidden_types, frames, 1,
                                    measure, context);
}

OCMapLayout *oc_map_layout_new_scaled(const OCDocument *doc, double width, double height, bool dark,
                                      bool show_enzymes, bool show_primers, const char *hidden_types, int frames,
                                      double text_scale, OCMeasureText measure, void *context)
{
    CallbackMeasurer tm(measure, context);
    view::MapOptions opt;
    opt.enzymes = show_enzymes;
    opt.primers = show_primers;
    opt.hiddenTypes = typeSet(hidden_types);
    opt.frames = frames;
    opt.textScale = text_scale;
    auto *l = new OCMapLayout;
    l->layout = std::make_unique<view::MapLayout>(doc->doc.displaySequence(), doc->doc.visibleSites(),
                                                  dark ? view::Theme::darkTheme() : view::Theme::light(), tm, width,
                                                  height, opt);
    return l;
}

void oc_map_layout_free(OCMapLayout *layout) { delete layout; }

OCScene *oc_map_layout_render(const OCMapLayout *layout, const OCDocument *doc)
{
    view::MapState st;
    st.hasSelection = doc->doc.hasSelection();
    st.selection = doc->doc.selection();
    st.activeFeature = doc->doc.activeFeature();
    st.activeSite = doc->doc.activeSite();
    return new OCScene{layout->layout->render(st)};
}

void oc_map_layout_hit(const OCMapLayout *layout, double x, double y, int *kind, int *index, int *position)
{
    const view::MapHit h = layout->layout->hitTest({x, y});
    if (kind)
        *kind = h.kind == view::MapHit::Feature ? OC_HIT_FEATURE
              : h.kind == view::MapHit::Site    ? OC_HIT_SITE
              : h.kind == view::MapHit::Sequence ? OC_HIT_SEQUENCE
              : h.kind == view::MapHit::Primer   ? OC_HIT_PRIMER
                                                 : OC_HIT_NONE;
    if (index)
        *index = h.index;
    if (position)
        *position = h.position;
}

bool oc_map_layout_is_circular(const OCMapLayout *layout) { return layout->layout->isCircular(); }

double oc_map_layout_radius(const OCMapLayout *layout)
{
    return layout->layout->isCircular() ? layout->layout->radius() : 0;
}
double oc_map_layout_angle_at(const OCMapLayout *layout, double x, double y) { return layout->layout->angleAt({x, y}); }
int oc_map_layout_position_at(const OCMapLayout *layout, double x, double y) { return layout->layout->positionAt({x, y}); }
char *oc_map_layout_tooltip(const OCMapLayout *layout, double x, double y) { return dupString(layout->layout->tooltipAt({x, y})); }

OCSeqLayout *oc_seq_layout_new(const OCDocument *doc, double width, bool show_complement, bool dark,
                               OCMeasureText measure, void *context)
{
    return oc_seq_layout_new_frames(doc, width, show_complement, 0, dark, measure, context);
}

OCSeqLayout *oc_seq_layout_new_frames(const OCDocument *doc, double width, bool show_complement, int frames,
                                      bool dark, OCMeasureText measure, void *context)
{
    CallbackMeasurer tm(measure, context);
    auto *l = new OCSeqLayout;
    l->layout = std::make_unique<view::SequenceLayout>(doc->doc.displaySequence(), doc->doc.visibleSites(),
                                                       dark ? view::Theme::darkTheme() : view::Theme::light(), tm,
                                                       width, show_complement, frames, doc->doc.reads());
    return l;
}

OCSeqLayout *oc_seq_layout_new_options(const OCDocument *doc, double width, bool show_complement,
                                       const OCSeqOptions *options, bool dark, OCMeasureText measure,
                                       void *context)
{
    CallbackMeasurer tm(measure, context);
    view::SeqOptions opt;
    if (options) {
        opt.frames = options->frames;
        opt.groupTens = options->group_tens;
        opt.enzymes = options->show_enzymes;
        opt.features = options->show_features;
        opt.primers = options->show_primers;
        opt.hiddenTypes = typeSet(options->hidden_types);
    }
    auto *l = new OCSeqLayout;
    l->layout = std::make_unique<view::SequenceLayout>(doc->doc.displaySequence(), doc->doc.visibleSites(),
                                                       dark ? view::Theme::darkTheme() : view::Theme::light(), tm,
                                                       width, show_complement, opt, doc->doc.reads());
    return l;
}

void oc_seq_layout_free(OCSeqLayout *layout) { delete layout; }
double oc_seq_layout_total_height(const OCSeqLayout *layout) { return layout->layout->totalHeight(); }

OCScene *oc_seq_layout_render(const OCSeqLayout *layout, const OCDocument *doc, double scroll_y, double view_height)
{
    view::SeqViewState st;
    st.hasSelection = doc->doc.hasSelection();
    st.selection = doc->doc.selection();
    st.activeFeature = doc->doc.activeFeature();
    st.activeSite = doc->doc.activeSite();
    st.hoverCodon = layout->hoverCodon;
    return new OCScene{layout->layout->render(scroll_y, view_height, st)};
}

void oc_seq_layout_set_hover_codon(OCSeqLayout *l, int codon_start) { l->hoverCodon = codon_start; }

int oc_seq_layout_position_at(const OCSeqLayout *l, double x, double y) { return l->layout->positionAt({x, y}); }
int oc_seq_layout_feature_at(const OCSeqLayout *l, double x, double y) { return l->layout->featureAt({x, y}); }
int oc_seq_layout_site_at(const OCSeqLayout *l, double x, double y) { return l->layout->siteAt({x, y}); }
int oc_seq_layout_primer_at(const OCSeqLayout *l, double x, double y) { return l->layout->primerAt({x, y}); }
int oc_seq_layout_codon_at(const OCSeqLayout *l, double x, double y) { return l->layout->codonAt({x, y}); }
int oc_seq_layout_base_at(const OCSeqLayout *l, double x, double y) { return l->layout->baseAt({x, y}); }
double oc_seq_layout_line_top(const OCSeqLayout *l, int position) { return l->layout->lineTopFor(position); }
double oc_seq_layout_line_height(const OCSeqLayout *l, int position) { return l->layout->lineHeightFor(position); }
char *oc_seq_layout_tooltip(const OCSeqLayout *l, double x, double y) { return dupString(l->layout->tooltipAt({x, y})); }

// ---- scenes ---------------------------------------------------------------------

void oc_scene_free(OCScene *scene) { delete scene; }
double oc_scene_width(const OCScene *scene) { return scene->scene.width; }
double oc_scene_height(const OCScene *scene) { return scene->scene.height; }
OCColor oc_scene_background(const OCScene *scene) { return toC(scene->scene.background); }
int oc_scene_item_count(const OCScene *scene) { return int(scene->scene.items.size()); }

int oc_scene_item_kind(const OCScene *scene, int index)
{
    if (index < 0 || index >= int(scene->scene.items.size()))
        return -1;
    return std::holds_alternative<view::PathItem>(scene->scene.items[size_t(index)]) ? OC_ITEM_PATH : OC_ITEM_TEXT;
}

bool oc_scene_path(const OCScene *scene, int index, OCPathInfo *out)
{
    if (index < 0 || index >= int(scene->scene.items.size()) || !out)
        return false;
    const auto *p = std::get_if<view::PathItem>(&scene->scene.items[size_t(index)]);
    if (!p)
        return false;
    static_assert(sizeof(OCPoint) == sizeof(view::Point), "OCPoint must mirror view::Point");
    out->points = reinterpret_cast<const OCPoint *>(p->points.data());
    out->point_count = int(p->points.size());
    out->closed = p->closed;
    out->fill = toC(p->fill);
    out->stroke = toC(p->stroke);
    out->line_width = p->lineWidth;
    out->dashed = p->dashed;
    return true;
}

bool oc_scene_text(const OCScene *scene, int index, OCTextInfo *out)
{
    if (index < 0 || index >= int(scene->scene.items.size()) || !out)
        return false;
    const auto *t = std::get_if<view::TextItem>(&scene->scene.items[size_t(index)]);
    if (!t)
        return false;
    out->text = t->text.c_str();
    out->x = t->pos.x;
    out->y = t->pos.y;
    out->size = t->font.size;
    out->bold = t->font.bold;
    out->mono = t->font.mono;
    out->color = toC(t->color);
    out->halign = t->halign == view::HAlign::Center ? OC_HALIGN_CENTER
                : t->halign == view::HAlign::Right  ? OC_HALIGN_RIGHT
                                                    : OC_HALIGN_LEFT;
    out->valign = t->valign == view::VAlign::Middle ? OC_VALIGN_MIDDLE
                : t->valign == view::VAlign::Top    ? OC_VALIGN_TOP
                                                    : OC_VALIGN_BASELINE;
    out->angle = t->angle;
    return true;
}

} // extern "C"
