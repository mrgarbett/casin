// SPDX-License-Identifier: GPL-3.0-or-later
#include "style.h"

#include <cstring>

namespace oc::view {

Color FeatureStyle::fill(const std::string &type, const std::string &explicitColor)
{
    static const Color fallback = Color::rgb(0xc0, 0xbf, 0xbc);   // light 4
    if (!explicitColor.empty()) {
        Color c = Color::parse(explicitColor, Color::none());
        if (!c.isNone())
            return c;
    }
    struct Entry { const char *type; int r, g, b; };
    static const Entry table[] = {
        {"CDS", 0x62, 0xa0, 0xea},            // blue 2
        {"gene", 0x99, 0xc1, 0xf1},           // blue 1
        {"mRNA", 0x99, 0xc1, 0xf1},
        {"promoter", 0x57, 0xe3, 0x89},       // green 2
        {"regulatory", 0x8f, 0xf0, 0xa4},     // green 1
        {"enhancer", 0x2e, 0xc2, 0x7e},       // green 4
        {"terminator", 0xf6, 0x61, 0x51},     // red 1
        {"polyA_signal", 0xf6, 0x61, 0x51},
        {"rep_origin", 0xf8, 0xe4, 0x5c},     // yellow 2
        {"oriT", 0xf8, 0xe4, 0x5c},
        {"primer_bind", 0xc0, 0x61, 0xcb},    // purple 2
        {"protein_bind", 0xff, 0xa3, 0x48},   // orange 2
        {"misc_binding", 0xff, 0xa3, 0x48},
        {"RBS", 0xcd, 0xab, 0x8f},            // brown 1
        {"5'UTR", 0xcd, 0xab, 0x8f},
        {"3'UTR", 0xcd, 0xab, 0x8f},
        {"sig_peptide", 0xdc, 0x8a, 0xdd},    // purple 1
        {"LTR", 0xb5, 0x83, 0x5a},            // brown 2
        {"repeat_region", 0xb5, 0x83, 0x5a},
        {"mobile_element", 0xed, 0x33, 0x3b}, // red 2
        {"misc_RNA", 0xff, 0xbe, 0x6f},       // orange 1
        {"ncRNA", 0xff, 0xbe, 0x6f},
        {"tRNA", 0xff, 0xbe, 0x6f},
        {"rRNA", 0xff, 0xbe, 0x6f},
        {"exon", 0x99, 0xc1, 0xf1},
        {"intron", 0xde, 0xdd, 0xda},
        {"misc_feature", 0xc0, 0xbf, 0xbc},   // light 4
    };
    for (const Entry &e : table)
        if (type == e.type)
            return Color::rgb(e.r, e.g, e.b);
    return fallback;
}

Color FeatureStyle::textOn(const Color &fill)
{
    // Pick whichever of near-black / white has the higher contrast ratio.
    const double l = fill.luminance();
    const double contrastDark = (l + 0.05) / (0.0137 + 0.05);   // #1e1e1e-ish
    const double contrastLight = (1.0 + 0.05) / (l + 0.05);
    return contrastDark >= contrastLight ? Color::rgb(0x24, 0x1f, 0x31) : Color::rgb(0xff, 0xff, 0xff);
}

bool FeatureStyle::isDrawn(const Feature &f)
{
    return f.type != "source";
}

Theme Theme::light()
{
    Theme t;
    t.dark = false;
    t.background = Color::rgb(0xff, 0xff, 0xff);
    t.surface = Color::rgb(0xfa, 0xfa, 0xfb);
    t.text = Color::rgb(0x24, 0x1f, 0x31);
    t.mutedText = Color::rgb(0x77, 0x76, 0x7b);
    t.backbone = Color::rgb(0x3d, 0x38, 0x46);
    t.tick = Color::rgb(0x9a, 0x99, 0x96);
    t.selection = Color::rgb(0x35, 0x84, 0xe4, 0.22);
    t.selectionEdge = Color::rgb(0x1c, 0x71, 0xd8);
    t.enzymeLabel = Color::rgb(0x3d, 0x38, 0x46);
    t.enzymeUnique = Color::rgb(0x1a, 0x5f, 0xb4);
    t.enzymeTypeIIS = Color::rgb(0x81, 0x3d, 0x9c);
    t.leader = Color::rgb(0xb0, 0xaf, 0xac);
    t.cutMarker = Color::rgb(0xc0, 0x1c, 0x28);
    t.featureOutline = Color::rgb(0x24, 0x1f, 0x31, 0.35);
    t.accent = Color::rgb(0x35, 0x84, 0xe4);
    return t;
}

Theme Theme::darkTheme()
{
    Theme t;
    t.dark = true;
    t.background = Color::rgb(0x1e, 0x1e, 0x1e);
    t.surface = Color::rgb(0x24, 0x24, 0x24);
    t.text = Color::rgb(0xf6, 0xf5, 0xf4);
    t.mutedText = Color::rgb(0x9a, 0x99, 0x96);
    t.backbone = Color::rgb(0xde, 0xdd, 0xda);
    t.tick = Color::rgb(0x77, 0x76, 0x7b);
    t.selection = Color::rgb(0x35, 0x84, 0xe4, 0.35);
    t.selectionEdge = Color::rgb(0x62, 0xa0, 0xea);
    t.enzymeLabel = Color::rgb(0xde, 0xdd, 0xda);
    t.enzymeUnique = Color::rgb(0x99, 0xc1, 0xf1);
    t.enzymeTypeIIS = Color::rgb(0xdc, 0x8a, 0xdd);
    t.leader = Color::rgb(0x5e, 0x5c, 0x64);
    t.cutMarker = Color::rgb(0xf6, 0x61, 0x51);
    t.featureOutline = Color::rgb(0, 0, 0, 0.5);
    t.accent = Color::rgb(0x62, 0xa0, 0xea);
    return t;
}

} // namespace oc::view
