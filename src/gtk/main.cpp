// SPDX-License-Identifier: GPL-3.0-or-later
// OpenClone for Linux: GTK4 + libadwaita frontend.
//
// Headless checks:
//   openclone --render-map out.png <file>      (no display needed; .svg/.pdf work too)
//   openclone --screenshot out.png <file>      (needs a display, e.g. xvfb-run)
// Options for both: --size WxH  --enzymes 1|2|3|all  --select A-B (1-based)  --dark
#include "window.h"

#include "core/seqio.h"
#include "view/cairo_render.h"
#include "view/maplayout.h"

#include <adwaita.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace oc;

namespace {

const char *kAppId = "io.github.openclone.OpenClone";

struct Options {
    std::string renderMap, screenshot;
    int width = 0, height = 0;
    int maxCuts = 1;
    int selStart = -1, selEnd = -1;
    bool dark = false;
    std::vector<std::string> files;
    std::vector<char *> passthrough;   // anything we don't consume goes to GApplication
};

bool parseOptions(int argc, char **argv, Options *o, std::string *error)
{
    o->passthrough.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string *out) {
            if (i + 1 >= argc) {
                *error = a + " needs a value";
                return false;
            }
            *out = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--render-map") {
            if (!next(&o->renderMap))
                return false;
        } else if (a == "--screenshot") {
            if (!next(&o->screenshot))
                return false;
        } else if (a == "--size") {
            if (!next(&v) || std::sscanf(v.c_str(), "%dx%d", &o->width, &o->height) != 2) {
                *error = "--size takes WxH";
                return false;
            }
        } else if (a == "--enzymes") {
            if (!next(&v))
                return false;
            o->maxCuts = v == "all" ? 0 : std::atoi(v.c_str());
        } else if (a == "--select") {
            if (!next(&v) || std::sscanf(v.c_str(), "%d-%d", &o->selStart, &o->selEnd) != 2) {
                *error = "--select takes A-B (1-based, inclusive)";
                return false;
            }
        } else if (a == "--dark") {
            o->dark = true;
        } else if (!a.empty() && a[0] == '-') {
            o->passthrough.push_back(argv[i]);
        } else {
            o->files.push_back(a);
            o->passthrough.push_back(argv[i]);
        }
    }
    return true;
}

void applyOptions(Document &doc, const Options &o)
{
    doc.setMaxCuts(o.maxCuts);
    if (o.selStart > 0 && o.selEnd >= o.selStart)
        doc.setSelection({o.selStart - 1, o.selEnd - o.selStart + 1});
}

int renderMap(const Options &o)
{
    if (o.files.size() != 1) {
        std::fprintf(stderr, "usage: openclone --render-map out.png <file>\n");
        return 2;
    }
    Sequence seq;
    std::string err;
    if (!loadSequenceFile(o.files[0], &seq, &err)) {
        std::fprintf(stderr, "openclone: %s\n", err.c_str());
        return 1;
    }
    Document doc;
    doc.setSequence(std::move(seq), o.files[0]);
    applyOptions(doc, o);
    view::PangoTextMeasurer tm;
    view::MapLayout layout(doc.sequence(), doc.visibleSites(), o.dark ? view::Theme::darkTheme() : view::Theme::light(),
                           tm, o.width > 0 ? o.width : 1000, o.height > 0 ? o.height : 820);
    view::MapState st;
    st.hasSelection = doc.hasSelection();
    st.selection = doc.selection();
    if (!view::exportScene(layout.render(st), o.renderMap, &err)) {
        std::fprintf(stderr, "openclone: %s\n", err.c_str());
        return 1;
    }
    std::printf("wrote %s (%d sites shown)\n", o.renderMap.c_str(), int(doc.visibleSites().size()));
    return 0;
}

Options g_options;
int g_exitCode = 0;

void actOpen(GSimpleAction *, GVariant *, gpointer app)
{
    gtk::showOpenDialog(GTK_APPLICATION(app), gtk_application_get_active_window(GTK_APPLICATION(app)));
}

void actNew(GSimpleAction *, GVariant *, gpointer app)
{
    GtkWindow *parent = gtk_application_get_active_window(GTK_APPLICATION(app));
    if (!parent) {
        gtk::Window *w = gtk::Window::create(GTK_APPLICATION(app));
        w->present();
        parent = w->gtkWindow();
    }
    gtk::showNewSequenceDialog(GTK_APPLICATION(app), parent);
}

void actQuit(GSimpleAction *, GVariant *, gpointer app)
{
    g_application_quit(G_APPLICATION(app));
}

void actAbout(GSimpleAction *, GVariant *, gpointer app)
{
    AdwDialog *about = adw_about_dialog_new();
    adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(about), "OpenClone");
    adw_about_dialog_set_application_icon(ADW_ABOUT_DIALOG(about), kAppId);
    adw_about_dialog_set_version(ADW_ABOUT_DIALOG(about), "0.1.0");
    adw_about_dialog_set_comments(ADW_ABOUT_DIALOG(about),
                                  "An open-source plasmid viewer. Opens GenBank, FASTA and SnapGene .dna files.");
    adw_about_dialog_set_license_type(ADW_ABOUT_DIALOG(about), GTK_LICENSE_GPL_3_0);
    static const char *rebase[] = {"REBASE, the Restriction Enzyme Database https://rebase.neb.com", nullptr};
    adw_about_dialog_add_acknowledgement_section(ADW_ABOUT_DIALOG(about), "Restriction enzyme data", rebase);
    adw_about_dialog_add_legal_section(
        ADW_ABOUT_DIALOG(about), "Melting temperature", "SantaLucia J. (1998) PNAS 95:1460–1465, unified nearest-neighbor parameters.",
        GTK_LICENSE_CUSTOM, nullptr);
    adw_dialog_present(about, GTK_WIDGET(gtk_application_get_active_window(GTK_APPLICATION(app))));
}

void onStartup(GApplication *app, gpointer)
{
    static const GActionEntry entries[] = {
        {"new", actNew, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"open", actOpen, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"quit", actQuit, nullptr, nullptr, nullptr, {0, 0, 0}},
        {"about", actAbout, nullptr, nullptr, nullptr, {0, 0, 0}},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app), entries, G_N_ELEMENTS(entries), app);
    struct Accel { const char *action; const char *keys[3]; };
    static const Accel accels[] = {
        {"app.open", {"<Control>o", nullptr}},
        {"app.quit", {"<Control>q", nullptr}},
        {"app.new", {"<Control>n", nullptr}},
        {"win.save", {"<Control>s", nullptr}},
        {"win.save-as", {"<Control><Shift>s", nullptr}},
        {"win.undo", {"<Control>z", nullptr}},
        {"win.redo", {"<Control><Shift>z", "<Control>y", nullptr}},
        {"win.cut", {"<Control>x", nullptr}},
        {"win.paste", {"<Control>v", nullptr}},
        {"win.select-all", {"<Control>a", nullptr}},
        {"win.add-feature", {"<Control>k", nullptr}},
        {"win.digest", {"<Control>d", nullptr}},
        {"win.export-map", {"<Control>e", nullptr}},
        {"win.copy", {"<Control>c", nullptr}},
        {"win.copy-rc", {"<Control><Shift>c", nullptr}},
        {"win.find", {"<Control>f", nullptr}},
        {"win.close", {"<Control>w", nullptr}},
    };
    for (const Accel &a : accels)
        gtk_application_set_accels_for_action(GTK_APPLICATION(app), a.action, a.keys);
    if (g_options.dark)
        adw_style_manager_set_color_scheme(adw_style_manager_get_default(), ADW_COLOR_SCHEME_FORCE_DARK);
}

void screenshotDone(bool ok, gpointer app)
{
    if (ok)
        std::printf("wrote %s\n", g_options.screenshot.c_str());
    else
        std::fprintf(stderr, "openclone: screenshot failed\n");
    g_exitCode = ok ? 0 : 1;
    g_application_quit(G_APPLICATION(app));
}

void startScreenshot(GtkApplication *app, gtk::Window *w)
{
    if (g_options.width > 0 && g_options.height > 0)
        gtk_window_set_default_size(w->gtkWindow(), g_options.width, g_options.height);
    applyOptions(w->document(), g_options);
    w->present();
    w->screenshot(g_options.screenshot, screenshotDone, app);
}

void onActivate(GApplication *app, gpointer)
{
    gtk::Window *w = gtk::Window::create(GTK_APPLICATION(app));
    if (!g_options.screenshot.empty()) {
        startScreenshot(GTK_APPLICATION(app), w);
        return;
    }
    w->present();
}

void onOpen(GApplication *app, GFile **files, int n, const char *, gpointer)
{
    std::vector<std::string> paths;
    for (int i = 0; i < n; ++i) {
        char *p = g_file_get_path(files[i]);
        if (p)
            paths.push_back(p);
        g_free(p);
    }
    if (!g_options.screenshot.empty()) {
        gtk::Window *w = gtk::Window::create(GTK_APPLICATION(app));
        std::string err;
        if (paths.empty() || !w->load(paths[0], &err)) {
            std::fprintf(stderr, "openclone: %s\n", err.c_str());
            g_exitCode = 1;
            g_application_quit(app);
            return;
        }
        startScreenshot(GTK_APPLICATION(app), w);
        return;
    }
    gtk::openFiles(GTK_APPLICATION(app), paths);
}

} // namespace

int main(int argc, char **argv)
{
    std::string error;
    if (!parseOptions(argc, argv, &g_options, &error)) {
        std::fprintf(stderr, "openclone: %s\n", error.c_str());
        return 2;
    }
    if (!g_options.renderMap.empty())
        return renderMap(g_options);

    GApplicationFlags flags = G_APPLICATION_HANDLES_OPEN;
    if (!g_options.screenshot.empty())
        flags = GApplicationFlags(flags | G_APPLICATION_NON_UNIQUE);
    AdwApplication *app = adw_application_new(kAppId, flags);
    g_signal_connect(app, "startup", G_CALLBACK(onStartup), nullptr);
    g_signal_connect(app, "activate", G_CALLBACK(onActivate), nullptr);
    g_signal_connect(app, "open", G_CALLBACK(onOpen), nullptr);
    int status = g_application_run(G_APPLICATION(app), int(g_options.passthrough.size()), g_options.passthrough.data());
    g_object_unref(app);
    return status ? status : g_exitCode;
}
