/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Plain C interface over the Restricto core and layout code, for frontends
 * written in other languages (the Swift/AppKit app on macOS).
 *
 * Ownership: *_new returns an object you free with the matching *_free.
 * Strings returned as `char *` are malloc'd: free them with oc_string_free.
 * `const char *` results stay valid until the next call on the same object.
 *
 * Layouts keep pointers into their document. Rebuild them (free + new)
 * after the document reports OC_CHANGED_SEQUENCE or OC_CHANGED_ENZYMES.
 */
#ifndef RESTRICTO_H
#define RESTRICTO_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct OCDocument OCDocument;
typedef struct OCMapLayout OCMapLayout;
typedef struct OCSeqLayout OCSeqLayout;
typedef struct OCScene OCScene;

typedef struct { double r, g, b, a; } OCColor;
typedef struct { double x, y; } OCPoint;

enum {
    OC_CHANGED_SEQUENCE = 1,
    OC_CHANGED_SELECTION = 2,
    OC_CHANGED_ENZYMES = 4,
    OC_CHANGED_HISTORY = 8,
    OC_CHANGED_PENDING = 16,   /* the pending primer was set or cleared: redraw */
};
enum { OC_FORMAT_GENBANK = 1, OC_FORMAT_FASTA = 2, OC_FORMAT_SNAPGENE = 3, OC_FORMAT_EMBL = 4 };
enum { OC_STRAND_NONE = 0, OC_STRAND_FORWARD = 1, OC_STRAND_REVERSE = 2 };
enum { OC_HIT_NONE = 0, OC_HIT_FEATURE = 1, OC_HIT_SITE = 2, OC_HIT_SEQUENCE = 3, OC_HIT_PRIMER = 4 };
enum { OC_ITEM_PATH = 0, OC_ITEM_TEXT = 1 };
enum { OC_HALIGN_LEFT = 0, OC_HALIGN_CENTER = 1, OC_HALIGN_RIGHT = 2 };
enum { OC_VALIGN_BASELINE = 0, OC_VALIGN_MIDDLE = 1, OC_VALIGN_TOP = 2 };

void oc_string_free(char *s);
/* "0.1.0-alpha" */
const char *oc_version(void);

/* ---- document ---------------------------------------------------------- */

typedef void (*OCChangeCallback)(void *context, int what);

OCDocument *oc_document_new(void);
void oc_document_free(OCDocument *doc);
void oc_document_set_callback(OCDocument *doc, OCChangeCallback cb, void *context);

bool oc_document_load(OCDocument *doc, const char *path, char **error);
/* Saving as GenBank or .dna also makes `path` the document's path and clears
 * the modified flag; FASTA is an export and changes neither. */
bool oc_document_save(OCDocument *doc, const char *path, int format, char **error);
/* Starts the document over with typed or pasted bases (cleaned to IUPAC).
 * The result counts as unsaved. Returns false when `bases` holds no DNA. */
bool oc_document_new_sequence(OCDocument *doc, const char *name, const char *bases, bool circular);
/* Copies `src`'s sequence (with its features and primers) and selection into
 * `out`, as an unsaved document; for previews. */
bool oc_document_copy(const OCDocument *src, OCDocument *out);

bool oc_document_is_empty(const OCDocument *doc);
const char *oc_document_name(OCDocument *doc);
const char *oc_document_description(OCDocument *doc);
int oc_document_length(const OCDocument *doc);
bool oc_document_is_circular(const OCDocument *doc);
char *oc_document_status_text(const OCDocument *doc);
/* The status without the length and topology: only the caret or selection
   (empty when there is neither), for a window that shows the length elsewhere. */
char *oc_document_selection_text(const OCDocument *doc);

typedef struct {
    const char *name;
    const char *type;
    int start;          /* 0-based footprint start (may wrap) */
    int length;
    int part_count;     /* number of ranges */
    int strand;         /* OC_STRAND_* */
    OCColor color;      /* display color */
    bool drawn;         /* false for "source" features */
} OCFeatureInfo;

int oc_document_feature_count(const OCDocument *doc);
bool oc_document_feature(OCDocument *doc, int index, OCFeatureInfo *out);

typedef struct {
    const char *name;
    const char *site;
    int cuts;
    bool hidden;
    bool passes_filter;
    int methylation_blocked;   /* of its sites, how many Dam/Dcm methylation blocks */
    const char *methylation;   /* "Dam", "Dcm", "Dam and Dcm", or "" */
} OCEnzymeInfo;

int oc_document_enzyme_count(const OCDocument *doc);           /* every known enzyme */
bool oc_document_enzyme(OCDocument *doc, int index, OCEnzymeInfo *out);
void oc_document_set_enzyme_hidden(OCDocument *doc, const char *name, bool hidden);
int oc_document_max_cuts(const OCDocument *doc);
void oc_document_set_max_cuts(OCDocument *doc, int max_cuts);  /* 1, 2, 3 or 0 = all */
int oc_document_visible_site_count(const OCDocument *doc);
const char *oc_document_visible_site_enzyme(const OCDocument *doc, int index);

bool oc_document_has_selection(const OCDocument *doc);
int oc_document_selection_start(const OCDocument *doc);
int oc_document_selection_length(const OCDocument *doc);
void oc_document_set_selection(OCDocument *doc, int start, int length);
void oc_document_clear_selection(OCDocument *doc);
char *oc_document_selected_bases(const OCDocument *doc);
char *oc_document_selected_reverse_complement(const OCDocument *doc);
int oc_document_active_feature(const OCDocument *doc);
int oc_document_active_site(const OCDocument *doc);
void oc_document_activate_feature(OCDocument *doc, int index);
void oc_document_activate_site(OCDocument *doc, int visible_index);

/* Searches both strands from just after the current selection start and
 * selects the next match. Returns the 1-based match number, 0 when there is
 * no match, or -1 when the query isn't DNA. *count gets the total. */
int oc_document_find_next(OCDocument *doc, const char *query, int *count);

/* ---- editing --------------------------------------------------------------
 * Each call is one undo step and reports OC_CHANGED_SEQUENCE, then
 * OC_CHANGED_SELECTION and OC_CHANGED_HISTORY. Typing at the caret joins the
 * previous typing step. */

bool oc_document_can_undo(const OCDocument *doc);
bool oc_document_can_redo(const OCDocument *doc);
char *oc_document_undo_label(const OCDocument *doc);   /* e.g. "Typing"; "" when none */
char *oc_document_redo_label(const OCDocument *doc);
void oc_document_undo(OCDocument *doc);
void oc_document_redo(OCDocument *doc);
bool oc_document_is_modified(const OCDocument *doc);
const char *oc_document_path(OCDocument *doc);

/* Replaces the selection, or inserts at the caret. Returns false when `text`
 * holds no DNA bases. */
bool oc_document_type_bases(OCDocument *doc, const char *text);
void oc_document_delete_selection(OCDocument *doc);
void oc_document_delete_backward(OCDocument *doc);
void oc_document_delete_forward(OCDocument *doc);
void oc_document_reverse_complement(OCDocument *doc);
void oc_document_set_origin_at_selection(OCDocument *doc);
void oc_document_set_circular(OCDocument *doc, bool circular);
void oc_document_rename(OCDocument *doc, const char *name);

/* Features by footprint: `start` is 0-based, `length` may run across the
 * origin of a circular sequence. update keeps the feature's qualifiers and,
 * when the footprint is unchanged, its parts. */
int oc_document_add_feature(OCDocument *doc, const char *name, const char *type, int strand, int start, int length);
void oc_document_update_feature(OCDocument *doc, int index, const char *name, const char *type, int strand,
                                int start, int length);
void oc_document_remove_feature(OCDocument *doc, int index);
/* Annotates common elements (promoters, origins, resistance genes, primer
 * sites) found in the sequence, as one undo step. Returns how many were added. */
int oc_document_annotate_common(OCDocument *doc);
/* "#rrggbb", or "" (or NULL) for the type's default colour. Undoable. */
void oc_document_set_feature_color(OCDocument *doc, int index, const char *color);

typedef struct {
    const char *name;
    const char *bases;   /* 5'->3' */
    int start;           /* binding site, 0-based; length 0 when it doesn't bind */
    int length;
    int strand;          /* OC_STRAND_FORWARD or OC_STRAND_REVERSE */
    const char *color;   /* "#rrggbb", or "" for the default primer colour */
    const char *description;
} OCPrimerInfo;

/* How the construct was made, oldest step first ("2026-10-27 PCR with ..."). */
int oc_document_history_count(const OCDocument *doc);
const char *oc_document_history(OCDocument *doc, int index);

int oc_document_primer_count(const OCDocument *doc);
bool oc_document_primer(OCDocument *doc, int index, OCPrimerInfo *out);
/* A primer binding the selection; returns its index or -1. */
int oc_document_add_primer_from_selection(OCDocument *doc, const char *name, int strand);
void oc_document_remove_primer(OCDocument *doc, int index);
/* Adds a primer by its sequence where its 3' part binds best (a 5' tail is
 * allowed); returns its index, or -1 with *error when it doesn't bind. */
int oc_document_add_primer_sequence(OCDocument *doc, const char *name, const char *bases, char **error);

typedef struct {
    int start, length;   /* the annealing part, on the top strand, 0-based */
    int strand;          /* OC_STRAND_FORWARD or OC_STRAND_REVERSE */
    int mismatches;      /* in the annealing part */
} OCPrimerSite;
/* Where a primer (5'->3', a 5' tail allowed) anneals, best first; returns how
 * many sites there are and fills up to `max`. */
int oc_primer_sites(const OCDocument *doc, const char *bases, OCPrimerSite *out, int max);
/* Adds a primer at a site from oc_primer_sites as one undo step; `color` and
 * `description` may be NULL or "". Returns its index, or -1 for an empty
 * primer or bad site. */
int oc_document_add_primer_at(OCDocument *doc, const char *name, const char *bases, const char *color,
                              const char *description, const OCPrimerSite *site);
/* Draws a primer at a site as if it were added, while it is being designed.
 * It isn't part of the document (not undoable, saved or listed); any change
 * to the sequence drops it. Reports OC_CHANGED_PENDING. Returns false for an
 * empty primer or bad site, and then clears it. */
bool oc_document_set_pending_primer(OCDocument *doc, const char *name, const char *bases, const char *color,
                                    const OCPrimerSite *site);
void oc_document_clear_pending_primer(OCDocument *doc);
bool oc_document_has_pending_primer(const OCDocument *doc);
/* The primer with its annealing part at `site` grown or shrunk (its 5' end
 * moving along the template; tail and 3' end kept) so the annealing part's
 * Tm comes closest to target_tm, 15 to 40 bases. Sets *fitted. Free with
 * oc_string_free. */
char *oc_primer_fit_tm(const OCDocument *doc, const char *bases, const OCPrimerSite *site, double target_tm,
                       OCPrimerSite *fitted);
/* Length of the product of two primers at these sites, tails included, or -1
 * when they don't face each other. */
int oc_pcr_product_length(const OCDocument *doc, const OCPrimerSite *a, int tail_a, const OCPrimerSite *b,
                          int tail_b);
typedef struct {
    bool applies;       /* a CDS meets the primer where its tail joins the annealing part */
    bool against_cds;   /* the part reads the other way to it */
    bool in_frame;
    int fix;            /* bases to add between the part and the annealing part (1 or 2), else 0 */
    char cds[64];       /* the CDS's name */
} OCFrameCheck;
/* Whether a part in a primer's tail (bases part_start.. from the 5' end)
 * lands in frame with the CDS its annealing part joins. */
void oc_primer_part_frame(const OCDocument *doc, const OCPrimerSite *site, int tail_length, int part_start,
                          int part_length, bool part_forward, OCFrameCheck *out);

/* ---- text metrics supplied by the frontend ------------------------------ */

typedef double (*OCMeasureText)(void *context, const char *text, double size, bool bold, bool mono);

/* ---- layouts ------------------------------------------------------------- */

OCMapLayout *oc_map_layout_new(const OCDocument *doc, double width, double height, bool dark,
                               OCMeasureText measure, void *context);
/* Map Display options: hidden_types is a comma-separated list of feature types. */
OCMapLayout *oc_map_layout_new_options(const OCDocument *doc, double width, double height, bool dark,
                                       bool show_enzymes, bool show_primers, const char *hidden_types,
                                       OCMeasureText measure, void *context);
/* Like oc_map_layout_new_options, with the ORFs of 3 (forward) or 6 reading
   frames drawn as arrows, as the sequence view translates them; 0 for none. */
OCMapLayout *oc_map_layout_new_frames(const OCDocument *doc, double width, double height, bool dark,
                                      bool show_enzymes, bool show_primers, const char *hidden_types, int frames,
                                      OCMeasureText measure, void *context);
void oc_map_layout_free(OCMapLayout *layout);
OCScene *oc_map_layout_render(const OCMapLayout *layout, const OCDocument *doc);
/* kind = OC_HIT_*; index = feature, visible-site or primer index; position = base boundary. */
void oc_map_layout_hit(const OCMapLayout *layout, double x, double y, int *kind, int *index, int *position);
bool oc_map_layout_is_circular(const OCMapLayout *layout);
double oc_map_layout_angle_at(const OCMapLayout *layout, double x, double y);
int oc_map_layout_position_at(const OCMapLayout *layout, double x, double y);
char *oc_map_layout_tooltip(const OCMapLayout *layout, double x, double y);

OCSeqLayout *oc_seq_layout_new(const OCDocument *doc, double width, bool show_complement, bool dark,
                               OCMeasureText measure, void *context);
// Like oc_seq_layout_new, with 0, 3 (forward) or 6 reading frames translated.
OCSeqLayout *oc_seq_layout_new_frames(const OCDocument *doc, double width, bool show_complement, int frames,
                                      bool dark, OCMeasureText measure, void *context);
// What a sequence layout shows: reading frames (0, 3 or 6; only ORFs are
// translated), a gap after every 10 bases, enzymes, features and primers.
// `hidden_types` is a comma-separated list of feature types to leave out, or NULL.
typedef struct OCSeqOptions {
    int frames;
    bool group_tens;
    bool show_enzymes;
    bool show_features;
    bool show_primers;
    const char *hidden_types;
} OCSeqOptions;
OCSeqLayout *oc_seq_layout_new_options(const OCDocument *doc, double width, bool show_complement,
                                       const OCSeqOptions *options, bool dark, OCMeasureText measure,
                                       void *context);
void oc_seq_layout_free(OCSeqLayout *layout);
double oc_seq_layout_total_height(const OCSeqLayout *layout);
/* Lines visible in [scroll_y, scroll_y + view_height), shifted up by scroll_y. */
OCScene *oc_seq_layout_render(const OCSeqLayout *layout, const OCDocument *doc, double scroll_y, double view_height);
/* Hit tests take content coordinates. */
int oc_seq_layout_position_at(const OCSeqLayout *layout, double x, double y);
int oc_seq_layout_feature_at(const OCSeqLayout *layout, double x, double y);
int oc_seq_layout_site_at(const OCSeqLayout *layout, double x, double y);
int oc_seq_layout_primer_at(const OCSeqLayout *layout, double x, double y);
/* First base of the codon whose amino acid is at (x, y) in a translation row, or -1. */
int oc_seq_layout_codon_at(const OCSeqLayout *layout, double x, double y);
/* The base (0-based) at (x, y) on either strand, or -1. */
int oc_seq_layout_base_at(const OCSeqLayout *layout, double x, double y);
/* Outlines the codon starting at codon_start on the strands in later renders
   (the one whose amino acid is under the mouse); -1 for none. */
void oc_seq_layout_set_hover_codon(OCSeqLayout *layout, int codon_start);
double oc_seq_layout_line_top(const OCSeqLayout *layout, int position);
double oc_seq_layout_line_height(const OCSeqLayout *layout, int position);
char *oc_seq_layout_tooltip(const OCSeqLayout *layout, double x, double y);

/* ---- digest and gel ------------------------------------------------------ */

typedef struct {
    int start;              /* top-strand cut that begins the fragment */
    int length;
    char left_enzyme[24];   /* "" = the end of a linear sequence */
    char right_enzyme[24];
    int left_overhang;      /* > 0 = 5', < 0 = 3', 0 = blunt */
    int right_overhang;
} OCFragmentInfo;

/* Cuts with the named enzymes at once. Returns the number of fragments (one
 * uncut molecule when nothing cuts) and fills up to `max` of them. */
int oc_digest(const OCDocument *doc, const char *const *enzymes, int enzyme_count, OCFragmentInfo *out, int max);
int oc_ladder_count(void);
const char *oc_ladder_name(int index);
/* A virtual gel with three lanes: ladder, uncut, and the digest. */
OCScene *oc_gel_render(const OCDocument *doc, const char *const *enzymes, int enzyme_count, int ladder, double width,
                       double height, bool dark, OCMeasureText measure, void *context);

/* ---- primers -------------------------------------------------------------- */

typedef struct {
    int length;
    double gc;            /* 0..1 */
    double tm;            /* deg C, NaN for ambiguous bases */
    int clamp_gc;         /* G or C among the last 5 bases */
    int longest_run;
    int self_dimer;
    int self_dimer3;      /* self-dimers that pair the 3' end */
    int hairpin_stem;
} OCPrimerCheck;

/* Returns the warnings, one per line ("" when the primer looks fine). */
char *oc_primer_check(const char *bases, OCPrimerCheck *out);
char *oc_primer_pair_check(const char *a, const char *b);
/* Designs a pair that amplifies the selection and adds it as one undo step. */
bool oc_document_design_primers(OCDocument *doc, double target_tm, char **error);
/* Like oc_document_design_primers; the primers may sit up to `flank` bases
 * outside the selection (0 = on its ends). */
bool oc_document_design_primers_flank(OCDocument *doc, double target_tm, int flank, char **error);

/* ---- ORFs ------------------------------------------------------------------ */

/* Open reading frames of at least min_codons, longest first, one per line:
 * "start<TAB>length<TAB>strand(1/-1)<TAB>protein" (0-based start). */
char *oc_find_orfs(const OCDocument *doc, int min_codons);
/* Adds ORF number `index` (from oc_find_orfs with the same min_codons) as a
 * CDS feature named `name`; returns the feature index or -1. */
int oc_document_add_orf(OCDocument *doc, int min_codons, int index, const char *name);

typedef struct {
    int length;              /* residues up to the first stop */
    int unknown;             /* residues left out of the mass */
    bool truncated;          /* an internal stop ended the protein early */
    double mass;             /* average, daltons */
    double pI;
    int extinction_reduced;  /* 280 nm, M^-1 cm^-1 */
    int extinction_cystines;
    int positive, negative;  /* K + R, D + E */
} OCProteinInfo;
/* Translates the selection (or the selected feature, on its own strand) and
 * returns the protein without its stop; NULL when fewer than 3 bases are selected. */
char *oc_document_selection_protein(const OCDocument *doc, OCProteinInfo *out);

/* ---- enzyme sets ------------------------------------------------------------ */

/* Built-in sets (Type IIS, then one per supplier), one per line:
 * "name<TAB>Enzyme,Enzyme,...". Saved sets use the text format of
 * parseEnzymeSets ("name: A, B") and are kept by the frontend. */
char *oc_builtin_enzyme_sets(void);
char *oc_enzyme_suppliers(const char *enzyme);   /* REBASE supplier codes */
/* Shows only these enzymes (keeps the cut filter). */
void oc_document_show_enzyme_set(OCDocument *doc, const char *const *enzymes, int count);

/* ---- printing --------------------------------------------------------------- */

/* Printed pages (map, then the sequence in whole lines), laid out at
 * page_width x page_height layout units; scale them to the paper. */
int oc_print_page_count(const OCDocument *doc, double page_width, double page_height, OCMeasureText measure,
                        void *context);
OCScene *oc_print_page(const OCDocument *doc, double page_width, double page_height, int page, OCMeasureText measure,
                       void *context);

/* ---- choosing enzymes ----------------------------------------------------- */

enum { OC_ENDS_ANY = 0, OC_ENDS_5PRIME = 1, OC_ENDS_3PRIME = 2, OC_ENDS_BLUNT = 3 };

/* Enzymes cutting 1..max_cuts times, optionally only inside the selection
 * and never inside feature `spare_feature` (-1 = none); one name per line. */
char *oc_document_choose_enzymes(const OCDocument *doc, int max_cuts, bool inside_selection, int spare_feature,
                                 int ends);
void oc_document_show_only_enzymes(OCDocument *doc, const char *const *names, int count, int max_cuts);

/* ---- cloning ---------------------------------------------------------------
 * Products go into `out` (from oc_document_new) as a new, unsaved sequence. */

bool oc_pcr_product(const OCDocument *templ, int forward_primer, int reverse_primer, OCDocument *out, char **error);
bool oc_restriction_clone(const OCDocument *vector, const char *vector_enzyme1, const char *vector_enzyme2,
                          const OCDocument *insert, const char *insert_enzyme1, const char *insert_enzyme2,
                          bool reversed, OCDocument *out, char **error);
/* TA (overhang 'T'), GC ('C') or blunt (0) cloning of a PCR product into a
 * linearized vector (its double-stranded part). */
bool oc_overhang_clone(const OCDocument *vector, const OCDocument *insert, char overhang, bool reversed,
                       OCDocument *out, char **error);
bool oc_golden_gate(const OCDocument *vector, const OCDocument *const *parts, int part_count, const char *enzyme,
                    OCDocument *out, char **error);
/* Replaces the selection with `bases` (empty deletes; a caret inserts) and
 * puts the mutant, carrying its back-to-back primers, into `out`. */
bool oc_mutagenesis(const OCDocument *doc, const char *bases, double target_tm, OCDocument *out, char **error);
/* The enzymes are ignored for a linear vector. */
bool oc_gibson(const OCDocument *vector, const char *vector_enzyme1, const char *vector_enzyme2,
               const OCDocument *const *fragments, int fragment_count, int min_overlap, OCDocument *out,
               char **error);
/* Overlap extension PCR: linear fragments joined by their end overlaps,
 * starting from the first, into one linear product. */
bool oc_overlap_extension(const OCDocument *const *fragments, int fragment_count, int min_overlap, OCDocument *out,
                          char **error);

/* Insertion by PCR: primers that amplify the insert's selection (or all of
 * it) with tails copying `overlap` bases of the vector on each side of the
 * vector's selection or caret. `out` gets the construct, carrying the two
 * primers; *primers gets "name<TAB>bases" lines for both. */
bool oc_insertion_by_pcr(const OCDocument *vector, const OCDocument *insert, double target_tm, int overlap,
                         OCDocument *out, char **primers, char **error);
/* ---- search ------------------------------------------------------------------ */
/* Sequence files under `folder` (and subfolders) matching `query` by name,
 * description, feature, primer or DNA on both strands. One hit per line:
 * "path<TAB>name<TAB>length<TAB>circular(0/1)<TAB>what matched". */
char *oc_search_folder(const char *folder, const char *query);
/* ---- verification ---------------------------------------------------------- */

typedef struct {
    bool reverse;          /* the read matches the bottom strand */
    int ref_start;         /* aligned reference span; may run across the origin */
    int ref_length;
    int matches, mismatches, gaps;
    double identity;       /* 0..1 */
    int diff_count;
} OCAlignmentInfo;

/* Aligns `read` (e.g. a Sanger trace opened with oc_document_load) to
 * `reference`. Returns the differences, one per line as
 * "position<TAB>reference bases<TAB>read bases" (1-based, "" for an
 * insertion or deletion side), or NULL with *error when it doesn't align. */
char *oc_align_read(const OCDocument *reference, const OCDocument *read, OCAlignmentInfo *out, char **error);
/* Aligns `read` and shows it under `reference`'s sequence (layouts made
 * afterwards draw it). Aligning the same read again replaces it. */
bool oc_document_show_read(OCDocument *reference, const OCDocument *read, char **error);
int oc_document_read_count(const OCDocument *doc);
void oc_document_clear_reads(OCDocument *doc);

/* ---- scenes ---------------------------------------------------------------- */

typedef struct {
    const OCPoint *points;
    int point_count;
    bool closed;
    OCColor fill;      /* a == 0: no fill */
    OCColor stroke;    /* a == 0: no stroke */
    double line_width;
    bool dashed;
} OCPathInfo;

typedef struct {
    const char *text;  /* UTF-8 */
    double x, y;
    double size;
    bool bold, mono;
    OCColor color;
    int halign;        /* OC_HALIGN_* */
    int valign;        /* OC_VALIGN_* */
    double angle;      /* radians, clockwise, around (x, y) */
} OCTextInfo;

void oc_scene_free(OCScene *scene);
double oc_scene_width(const OCScene *scene);
double oc_scene_height(const OCScene *scene);
OCColor oc_scene_background(const OCScene *scene);
int oc_scene_item_count(const OCScene *scene);
int oc_scene_item_kind(const OCScene *scene, int index);
bool oc_scene_path(const OCScene *scene, int index, OCPathInfo *out);
bool oc_scene_text(const OCScene *scene, int index, OCTextInfo *out);

#ifdef __cplusplus
}
#endif

#endif /* RESTRICTO_H */
