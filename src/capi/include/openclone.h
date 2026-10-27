/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Plain C interface over the OpenClone core and layout code, for frontends
 * written in other languages (the Swift/AppKit app on macOS).
 *
 * Ownership: *_new returns an object you free with the matching *_free.
 * Strings returned as `char *` are malloc'd: free them with oc_string_free.
 * `const char *` results stay valid until the next call on the same object.
 *
 * Layouts keep pointers into their document. Rebuild them (free + new)
 * after the document reports OC_CHANGED_SEQUENCE or OC_CHANGED_ENZYMES.
 */
#ifndef OPENCLONE_H
#define OPENCLONE_H

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

enum { OC_CHANGED_SEQUENCE = 1, OC_CHANGED_SELECTION = 2, OC_CHANGED_ENZYMES = 4, OC_CHANGED_HISTORY = 8 };
enum { OC_FORMAT_GENBANK = 1, OC_FORMAT_FASTA = 2, OC_FORMAT_SNAPGENE = 3 };
enum { OC_STRAND_NONE = 0, OC_STRAND_FORWARD = 1, OC_STRAND_REVERSE = 2 };
enum { OC_HIT_NONE = 0, OC_HIT_FEATURE = 1, OC_HIT_SITE = 2, OC_HIT_SEQUENCE = 3 };
enum { OC_ITEM_PATH = 0, OC_ITEM_TEXT = 1 };
enum { OC_HALIGN_LEFT = 0, OC_HALIGN_CENTER = 1, OC_HALIGN_RIGHT = 2 };
enum { OC_VALIGN_BASELINE = 0, OC_VALIGN_MIDDLE = 1, OC_VALIGN_TOP = 2 };

void oc_string_free(char *s);

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

bool oc_document_is_empty(const OCDocument *doc);
const char *oc_document_name(OCDocument *doc);
const char *oc_document_description(OCDocument *doc);
int oc_document_length(const OCDocument *doc);
bool oc_document_is_circular(const OCDocument *doc);
char *oc_document_status_text(const OCDocument *doc);

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

typedef struct {
    const char *name;
    const char *bases;   /* 5'->3' */
    int start;           /* binding site, 0-based; length 0 when it doesn't bind */
    int length;
    int strand;          /* OC_STRAND_FORWARD or OC_STRAND_REVERSE */
} OCPrimerInfo;

/* How the construct was made, oldest step first ("2026-10-27 PCR with ..."). */
int oc_document_history_count(const OCDocument *doc);
const char *oc_document_history(OCDocument *doc, int index);

int oc_document_primer_count(const OCDocument *doc);
bool oc_document_primer(OCDocument *doc, int index, OCPrimerInfo *out);
/* A primer binding the selection; returns its index or -1. */
int oc_document_add_primer_from_selection(OCDocument *doc, const char *name, int strand);
void oc_document_remove_primer(OCDocument *doc, int index);

/* ---- text metrics supplied by the frontend ------------------------------ */

typedef double (*OCMeasureText)(void *context, const char *text, double size, bool bold, bool mono);

/* ---- layouts ------------------------------------------------------------- */

OCMapLayout *oc_map_layout_new(const OCDocument *doc, double width, double height, bool dark,
                               OCMeasureText measure, void *context);
void oc_map_layout_free(OCMapLayout *layout);
OCScene *oc_map_layout_render(const OCMapLayout *layout, const OCDocument *doc);
/* kind = OC_HIT_*; index = feature or visible-site index; position = base boundary. */
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
void oc_seq_layout_free(OCSeqLayout *layout);
double oc_seq_layout_total_height(const OCSeqLayout *layout);
/* Lines visible in [scroll_y, scroll_y + view_height), shifted up by scroll_y. */
OCScene *oc_seq_layout_render(const OCSeqLayout *layout, const OCDocument *doc, double scroll_y, double view_height);
/* Hit tests take content coordinates. */
int oc_seq_layout_position_at(const OCSeqLayout *layout, double x, double y);
int oc_seq_layout_feature_at(const OCSeqLayout *layout, double x, double y);
int oc_seq_layout_site_at(const OCSeqLayout *layout, double x, double y);
int oc_seq_layout_primer_at(const OCSeqLayout *layout, double x, double y);
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
bool oc_golden_gate(const OCDocument *vector, const OCDocument *const *parts, int part_count, const char *enzyme,
                    OCDocument *out, char **error);
/* Replaces the selection with `bases` (empty deletes; a caret inserts) and
 * puts the mutant, carrying its back-to-back primers, into `out`. */
bool oc_mutagenesis(const OCDocument *doc, const char *bases, double target_tm, OCDocument *out, char **error);
/* The enzymes are ignored for a linear vector. */
bool oc_gibson(const OCDocument *vector, const char *vector_enzyme1, const char *vector_enzyme2,
               const OCDocument *const *fragments, int fragment_count, int min_overlap, OCDocument *out,
               char **error);

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

#endif /* OPENCLONE_H */
