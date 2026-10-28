// SPDX-License-Identifier: GPL-3.0-or-later
// Swift wrappers over the C API (src/capi/include/restricto.h).
import AppKit
import RestrictoCore

/// Takes ownership of a malloc'd C string from the core.
func takeString(_ p: UnsafeMutablePointer<CChar>?) -> String {
    guard let p = p else { return "" }
    defer { oc_string_free(p) }
    return String(cString: p)
}

extension OCColor {
    var nsColor: NSColor { NSColor(srgbRed: r, green: g, blue: b, alpha: a) }
    var cgColor: CGColor { CGColor(srgbRed: r, green: g, blue: b, alpha: a) }
}

enum Change {
    static let sequence = Int32(OC_CHANGED_SEQUENCE)
    static let selection = Int32(OC_CHANGED_SELECTION)
    static let enzymes = Int32(OC_CHANGED_ENZYMES)
    static let history = Int32(OC_CHANGED_HISTORY)
}

struct FeatureInfo {
    let index: Int
    let name: String
    let type: String
    let start: Int
    let length: Int
    let parts: Int
    let strand: Int32
    let color: NSColor
    let drawn: Bool
}

struct PrimerInfo {
    let index: Int
    let name: String
    let bases: String
    /// Binding site, 0-based; length 0 when the primer doesn't bind.
    let start: Int
    let length: Int
    let strand: Int32
}

struct EnzymeInfo {
    let name: String
    let site: String
    let cuts: Int
    let hidden: Bool
    let passesFilter: Bool
}

/// The document model shared with the Linux app (selection, enzyme filter, ...).
final class CoreDocument {
    let handle: OpaquePointer
    private var observers: [(Int32) -> Void] = []

    init() {
        handle = oc_document_new()
        oc_document_set_callback(handle, { context, what in
            guard let context = context else { return }
            let me = Unmanaged<CoreDocument>.fromOpaque(context).takeUnretainedValue()
            for observer in me.observers { observer(what) }
        }, Unmanaged.passUnretained(self).toOpaque())
    }

    deinit {
        oc_document_set_callback(handle, nil, nil)
        oc_document_free(handle)
    }

    func observe(_ observer: @escaping (Int32) -> Void) { observers.append(observer) }

    // MARK: View options (the window's map and sequence views follow these)

    /// Reading frames translated in the sequence view: 0, 3 or 6.
    var frames = 0 { didSet { viewOptionsChanged() } }
    var mapShowsEnzymes = true { didSet { viewOptionsChanged() } }
    var mapShowsPrimers = true { didSet { viewOptionsChanged() } }
    var mapHiddenTypes: Set<String> = [] { didSet { viewOptionsChanged() } }

    /// Views rebuild their layouts on an enzyme change, so report one.
    private func viewOptionsChanged() {
        for observer in observers { observer(Change.enzymes) }
    }

    func load(path: String) throws {
        var err: UnsafeMutablePointer<CChar>?
        if !oc_document_load(handle, path, &err) {
            throw NSError(domain: "Restricto", code: 1,
                          userInfo: [NSLocalizedDescriptionKey: takeString(err)])
        }
    }

    func save(path: String, format: Int32) throws {
        var err: UnsafeMutablePointer<CChar>?
        if !oc_document_save(handle, path, format, &err) {
            throw NSError(domain: "Restricto", code: 2,
                          userInfo: [NSLocalizedDescriptionKey: takeString(err)])
        }
    }

    var isEmpty: Bool { oc_document_is_empty(handle) }
    var isModified: Bool { oc_document_is_modified(handle) }
    var name: String { String(cString: oc_document_name(handle)) }
    var length: Int { Int(oc_document_length(handle)) }
    var isCircular: Bool { oc_document_is_circular(handle) }
    var statusText: String { takeString(oc_document_status_text(handle)) }

    var features: [FeatureInfo] {
        (0..<Int(oc_document_feature_count(handle))).compactMap { i in
            var f = OCFeatureInfo()
            guard oc_document_feature(handle, Int32(i), &f) else { return nil }
            return FeatureInfo(index: i, name: String(cString: f.name), type: String(cString: f.type),
                               start: Int(f.start), length: Int(f.length), parts: Int(f.part_count),
                               strand: f.strand, color: f.color.nsColor, drawn: f.drawn)
        }
    }

    var enzymes: [EnzymeInfo] {
        (0..<Int(oc_document_enzyme_count(handle))).compactMap { i in
            var e = OCEnzymeInfo()
            guard oc_document_enzyme(handle, Int32(i), &e) else { return nil }
            return EnzymeInfo(name: String(cString: e.name), site: String(cString: e.site), cuts: Int(e.cuts),
                              hidden: e.hidden, passesFilter: e.passes_filter)
        }
    }

    var primers: [PrimerInfo] {
        (0..<Int(oc_document_primer_count(handle))).compactMap { i in
            var p = OCPrimerInfo()
            guard oc_document_primer(handle, Int32(i), &p) else { return nil }
            return PrimerInfo(index: i, name: String(cString: p.name), bases: String(cString: p.bases),
                              start: Int(p.start), length: Int(p.length), strand: p.strand)
        }
    }

    var maxCuts: Int {
        get { Int(oc_document_max_cuts(handle)) }
        set { oc_document_set_max_cuts(handle, Int32(newValue)) }
    }

    func setEnzyme(_ name: String, hidden: Bool) { oc_document_set_enzyme_hidden(handle, name, hidden) }

    var visibleSiteEnzymes: [String] {
        (0..<Int(oc_document_visible_site_count(handle))).map {
            String(cString: oc_document_visible_site_enzyme(handle, Int32($0)))
        }
    }

    var hasSelection: Bool { oc_document_has_selection(handle) }
    var selectionStart: Int { Int(oc_document_selection_start(handle)) }
    var selectionLength: Int { Int(oc_document_selection_length(handle)) }
    func select(start: Int, length: Int) { oc_document_set_selection(handle, Int32(start), Int32(length)) }
    var selectedBases: String { takeString(oc_document_selected_bases(handle)) }
    var selectedReverseComplement: String { takeString(oc_document_selected_reverse_complement(handle)) }
    var activeFeature: Int { Int(oc_document_active_feature(handle)) }
    var activeSite: Int { Int(oc_document_active_site(handle)) }
    func activateFeature(_ i: Int) { oc_document_activate_feature(handle, Int32(i)) }
    func activateSite(_ i: Int) { oc_document_activate_site(handle, Int32(i)) }

    /// 1-based match number, 0 for no match, -1 when the query isn't DNA.
    func findNext(_ query: String) -> (match: Int, count: Int) {
        var count: Int32 = 0
        let n = oc_document_find_next(handle, query, &count)
        return (Int(n), Int(count))
    }
}

// MARK: - Fonts and text metrics

enum SceneFonts {
    static func font(size: Double, bold: Bool, mono: Bool) -> NSFont {
        if mono {
            return NSFont.monospacedSystemFont(ofSize: size, weight: bold ? .bold : .regular)
        }
        return NSFont.systemFont(ofSize: size, weight: bold ? .semibold : .regular)
    }
}

/// Text widths for the shared layout code, measured with AppKit fonts.
let measureText: OCMeasureText = { _, text, size, bold, mono in
    guard let text = text else { return 0 }
    let font = SceneFonts.font(size: size, bold: bold, mono: mono)
    return Double((String(cString: text) as NSString).size(withAttributes: [.font: font]).width)
}

extension NSView {
    var isDarkAppearance: Bool {
        effectiveAppearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
    }
}

// MARK: - C strings

/// Calls `body` with the strings as a C array of `const char *`.
func withCStrings<R>(_ strings: [String], _ body: (UnsafePointer<UnsafePointer<CChar>?>) -> R) -> R {
    let copies = strings.map { strdup($0) }
    defer { copies.forEach { free($0) } }
    // One trailing nil keeps the buffer non-empty for an empty list.
    let pointers: [UnsafePointer<CChar>?] = copies.map { $0.map { UnsafePointer($0) } } + [nil]
    return pointers.withUnsafeBufferPointer { body($0.baseAddress!) }
}

/// A fixed-size C char array (imported as a tuple) as a String.
func tupleString<T>(_ tuple: T) -> String {
    withUnsafeBytes(of: tuple) { raw in
        String(decoding: raw.prefix { $0 != 0 }, as: UTF8.self)
    }
}
