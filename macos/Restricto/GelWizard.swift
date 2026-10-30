// SPDX-License-Identifier: GPL-3.0-or-later
// The gel wizard: lanes on the left (a ladder, then any open sequence, cut
// with any enzymes or left uncut), and the gel on the right as an imager
// shows it. Changing a lane runs it again. Hovering a band names it;
// clicking a fragment selects it in its sequence. Copy Image puts the gel on
// the clipboard for a notebook.
import AppKit
import RestrictoCore

/// A row that acts as one control: clicking anywhere on it calls `onClick`.
/// With a badge, it shows a numbered disc on its left (the lanes list).
final class ClickRow: NSView {
    var onClick: (() -> Void)?
    var isSelected = false { didSet { needsDisplay = true } }
    var badge: String? { didSet { needsDisplay = true } }
    private var hovering = false { didSet { needsDisplay = true } }

    init(content: NSView, badge: String? = nil) {
        self.badge = badge
        super.init(frame: .zero)
        translatesAutoresizingMaskIntoConstraints = false
        content.translatesAutoresizingMaskIntoConstraints = false
        addSubview(content)
        NSLayoutConstraint.activate([
            content.leadingAnchor.constraint(equalTo: leadingAnchor, constant: badge == nil ? 8 : 36),
            content.trailingAnchor.constraint(equalTo: trailingAnchor, constant: -8),
            content.topAnchor.constraint(equalTo: topAnchor, constant: 5),
            content.bottomAnchor.constraint(equalTo: bottomAnchor, constant: -5),
        ])
        setAccessibilityElement(true)
        setAccessibilityRole(.button)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    override func hitTest(_ point: NSPoint) -> NSView? {
        frame.contains(point) ? self : nil
    }

    override func draw(_ dirtyRect: NSRect) {
        let box = bounds.insetBy(dx: 0, dy: 1)
        if isSelected || hovering {
            NSColor.controlAccentColor.withAlphaComponent(isSelected ? 0.22 : 0.08).setFill()
            NSBezierPath(roundedRect: box, xRadius: 7, yRadius: 7).fill()
        }
        if let badge = badge {
            let disc = NSRect(x: 8, y: bounds.midY - 10, width: 20, height: 20)
            (isSelected ? NSColor.controlAccentColor : NSColor.tertiaryLabelColor).setFill()
            NSBezierPath(ovalIn: disc).fill()
            let text = NSAttributedString(string: badge, attributes: [
                .font: NSFont.systemFont(ofSize: 11, weight: .bold), .foregroundColor: NSColor.white])
            let size = text.size()
            text.draw(at: NSPoint(x: disc.midX - size.width / 2, y: disc.midY - size.height / 2))
        }
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseEnteredAndExited, .activeInKeyWindow,
                                                              .inVisibleRect], owner: self, userInfo: nil))
    }

    override func mouseEntered(with event: NSEvent) { hovering = true }
    override func mouseExited(with event: NSEvent) { hovering = false }
    override func mouseDown(with event: NSEvent) { onClick?() }
    override func accessibilityPerformPress() -> Bool { onClick?(); return true }
}

private final class FlippedView: NSView {
    override var isFlipped: Bool { true }
}

final class GelWizard: NSWindowController, NSWindowDelegate, NSTokenFieldDelegate {
    private struct Lane {
        var doc: CoreDocument
        var enzymes: [String]
        /// What decides the bands: when it changes, the lane runs again.
        var key: String { "\(ObjectIdentifier(doc).hashValue)|\(doc.length)|\(enzymes.joined(separator: ","))" }
    }

    private struct Fragment {
        let start: Int
        let length: Int
        let caption: String
    }

    private let template: CoreDocument
    private var lanes: [Lane] = []
    /// The lane being edited, counted as on the gel (0 is the ladder).
    private var selected = 1
    private var fragments: [[Fragment]] = []
    private var drawnKeys: [String] = []
    private var observed: Set<ObjectIdentifier> = []
    private static let maxLanes = 10

    private let gelView = GelView()
    private let ladderPopup = NSPopUpButton()
    private let laneList = NSStackView()
    private let addLaneButton = NSButton()
    private let removeLaneButton = NSButton()
    private let laneHeading = NSTextField(labelWithString: "LANE 1")
    private let sequencePopup = NSPopUpButton()
    private let enzymeField = NSTokenField()
    private let enzymeMenu = NSPopUpButton(frame: .zero, pullsDown: true)
    private let enzymeNote = NSTextField(wrappingLabelWithString: "")
    private let fragmentHeading = NSTextField(labelWithString: "FRAGMENTS")
    private let fragmentList = NSStackView()
    private let agarosePopup = NSPopUpButton()
    private let stainPopup = NSPopUpButton()
    /// How long the gel ran: drag it back to see the gel part way through.
    private let runSlider = NSSlider(value: 1, minValue: 0.1, maxValue: 1, target: nil, action: nil)
    private let runLabel = NSTextField(labelWithString: "Full run")
    private let copyButton = NSButton()
    private var chrome: WizardChrome?
    /// Sequences listed in the sequence popup, in its order.
    private var openDocs: [CoreDocument] = []

    private static var showing: [GelWizard] = []

    /// Brings up the gel for `template`, opening one if there is none: the
    /// sequence uncut, and cut with the first enzyme shown on its map.
    static func show(for template: CoreDocument, near parent: NSWindow?) {
        if let open = visible.first(where: { $0.template === template }) {
            open.window?.makeKeyAndOrderFront(nil)
            return
        }
        let wizard = GelWizard(template: template)
        showing.append(wizard)
        if let window = wizard.window {
            wizard.chrome = WizardChrome(window: window, parent: parent)
            if let parent = parent { window.title = "Gel · \(Self.withoutExtension(parent.title))" }
        }
        wizard.showWindow(nil)
        wizard.chrome?.attach()
        wizard.window?.makeFirstResponder(nil)
    }

    private init(template: CoreDocument) {
        self.template = template
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1040, height: 660),
                              styleMask: [.titled, .closable, .resizable, .miniaturizable, .fullSizeContentView],
                              backing: .buffered, defer: false)
        window.title = "Gel · \(template.name)"
        window.titlebarAppearsTransparent = true
        // The sidebar names it; a title would sit over the dark gel.
        window.titleVisibility = .hidden
        window.minSize = NSSize(width: 760, height: 480)
        window.isReleasedWhenClosed = false
        window.tabbingMode = .disallowed
        super.init(window: window)
        window.delegate = self
        lanes = [Lane(doc: template, enzymes: [])]
        if let first = template.visibleSiteEnzymes.first { lanes.append(Lane(doc: template, enzymes: [first])) }
        selected = lanes.count
        window.contentView = buildView()
        refresh()
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    // MARK: Layout

    private func buildView() -> NSView {
        func heading(_ text: String) -> NSTextField {
            let label = NSTextField(labelWithString: text)
            label.font = .systemFont(ofSize: NSFont.smallSystemFontSize, weight: .semibold)
            label.textColor = .secondaryLabelColor
            return label
        }
        func symbolButton(_ button: NSButton, _ symbol: String, _ tip: String, _ action: Selector) {
            button.image = NSImage(systemSymbolName: symbol, accessibilityDescription: tip)
            button.bezelStyle = .accessoryBarAction
            button.isBordered = true
            button.controlSize = .small
            button.toolTip = tip
            button.target = self
            button.action = action
        }

        let title = NSTextField(labelWithString: "Gel")
        title.font = .systemFont(ofSize: NSFont.systemFontSize + 2, weight: .semibold)
        let done = NSButton(title: "Done", target: self, action: #selector(done(_:)))
        done.bezelColor = .controlAccentColor
        done.keyEquivalent = "\r"
        let header = NSStackView(views: [title, NSView(), done])
        let subtitle = NSTextField(wrappingLabelWithString: "Digests run side by side on an agarose gel.")
        subtitle.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        subtitle.textColor = .secondaryLabelColor

        for i in 0..<Int(oc_ladder_count()) { ladderPopup.addItem(withTitle: String(cString: oc_ladder_name(Int32(i)))) }
        let savedLadder = UserDefaults.standard.integer(forKey: "GelLadder")
        ladderPopup.selectItem(at: savedLadder < ladderPopup.numberOfItems ? savedLadder : 0)
        ladderPopup.target = self
        ladderPopup.action = #selector(ladderChanged(_:))
        // Percentages as a bench would cast them; 1% unless chosen before.
        let savedPercent = UserDefaults.standard.object(forKey: "GelPercent") as? Double ?? 1
        for i in 0..<Int(oc_gel_percent_count()) {
            let percent = oc_gel_percent(Int32(i))
            agarosePopup.addItem(withTitle: "\(percent.formatted(.number.precision(.fractionLength(0...1))))% agarose")
            agarosePopup.lastItem?.representedObject = NSNumber(value: percent)
            if abs(percent - savedPercent) < 0.01 { agarosePopup.select(agarosePopup.lastItem) }
        }
        agarosePopup.toolTip = "Denser gels separate small fragments; thinner ones, large ones"
        agarosePopup.target = self
        agarosePopup.action = #selector(agaroseChanged(_:))

        symbolButton(addLaneButton, "plus", "Add a lane", #selector(addLane(_:)))
        symbolButton(removeLaneButton, "minus", "Remove the selected lane", #selector(removeLane(_:)))
        let lanesRow = NSStackView(views: [heading("LANES"), NSView(), addLaneButton, removeLaneButton])
        lanesRow.spacing = 2
        laneList.orientation = .vertical
        laneList.alignment = .leading
        laneList.spacing = 0

        laneHeading.font = .systemFont(ofSize: NSFont.smallSystemFontSize, weight: .semibold)
        laneHeading.textColor = .secondaryLabelColor
        sequencePopup.target = self
        sequencePopup.action = #selector(sequenceChanged(_:))
        enzymeField.delegate = self
        enzymeField.placeholderString = "Uncut. Type an enzyme…"
        enzymeField.tokenStyle = .rounded
        enzymeField.completionDelay = 0.1
        enzymeField.target = self
        enzymeField.action = #selector(enzymesEdited(_:))
        enzymeMenu.pullsDown = true
        enzymeMenu.isBordered = false
        enzymeMenu.controlSize = .small
        enzymeMenu.toolTip = "Add an enzyme that cuts this sequence"
        (enzymeMenu.cell as? NSPopUpButtonCell)?.arrowPosition = .arrowAtBottom
        let enzymeRow = NSStackView(views: [enzymeField, enzymeMenu])
        enzymeRow.spacing = 4
        enzymeField.setContentHuggingPriority(.defaultLow, for: .horizontal)
        enzymeMenu.setContentHuggingPriority(.required, for: .horizontal)
        let enzymeHint = NSTextField(wrappingLabelWithString: "Enzymes in one lane cut together.")
        enzymeHint.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        enzymeHint.textColor = .secondaryLabelColor
        enzymeNote.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        enzymeNote.textColor = .systemOrange
        enzymeNote.isHidden = true

        fragmentHeading.font = .systemFont(ofSize: NSFont.smallSystemFontSize, weight: .semibold)
        fragmentHeading.textColor = .secondaryLabelColor
        fragmentList.orientation = .vertical
        fragmentList.alignment = .leading
        fragmentList.spacing = 0

        let inputs = NSStackView(views: [
            header, subtitle,
            heading("LADDER"), ladderPopup,
            heading("GEL"), agarosePopup,
            lanesRow, laneList,
            laneHeading, sequencePopup, enzymeRow, enzymeHint, enzymeNote,
            fragmentHeading, fragmentList,
        ])
        inputs.orientation = .vertical
        inputs.alignment = .leading
        inputs.spacing = 6
        inputs.edgeInsets = NSEdgeInsets(top: 38, left: 16, bottom: 16, right: 16)
        for v in [header, subtitle, ladderPopup, agarosePopup, lanesRow, laneList, sequencePopup, enzymeRow, enzymeNote, enzymeHint,
                  fragmentList] as [NSView] {
            v.widthAnchor.constraint(equalTo: inputs.widthAnchor, constant: -32).isActive = true
        }
        inputs.setCustomSpacing(16, after: subtitle)
        inputs.setCustomSpacing(16, after: agarosePopup)
        inputs.setCustomSpacing(16, after: laneList)
        inputs.setCustomSpacing(16, after: enzymeNote)
        inputs.setCustomSpacing(16, after: enzymeHint)

        // The sidebar scrolls when the lanes and fragments outgrow it.
        let document = FlippedView()
        document.translatesAutoresizingMaskIntoConstraints = false
        inputs.translatesAutoresizingMaskIntoConstraints = false
        document.addSubview(inputs)
        let scroll = NSScrollView()
        scroll.drawsBackground = false
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.documentView = document
        NSLayoutConstraint.activate([
            inputs.topAnchor.constraint(equalTo: document.topAnchor),
            inputs.leadingAnchor.constraint(equalTo: document.leadingAnchor),
            inputs.trailingAnchor.constraint(equalTo: document.trailingAnchor),
            inputs.bottomAnchor.constraint(equalTo: document.bottomAnchor),
            document.topAnchor.constraint(equalTo: scroll.contentView.topAnchor),
            document.leadingAnchor.constraint(equalTo: scroll.contentView.leadingAnchor),
            document.widthAnchor.constraint(equalTo: scroll.contentView.widthAnchor),
        ])
        let sidebar = Glass.sidePanelBackground(frame: .zero)
        sidebar.translatesAutoresizingMaskIntoConstraints = false
        scroll.translatesAutoresizingMaskIntoConstraints = false
        sidebar.addSubview(scroll)
        NSLayoutConstraint.activate([
            sidebar.widthAnchor.constraint(equalToConstant: 300),
            scroll.topAnchor.constraint(equalTo: sidebar.topAnchor),
            scroll.bottomAnchor.constraint(equalTo: sidebar.bottomAnchor),
            scroll.leadingAnchor.constraint(equalTo: sidebar.leadingAnchor),
            scroll.trailingAnchor.constraint(equalTo: sidebar.trailingAnchor),
        ])

        // The gel, always dark as an imager is, with its controls floating over it.
        gelView.onSelectLane = { [weak self] lane in self?.selectLane(lane) }
        gelView.onPickBand = { [weak self] lane, band in self?.pickFragment(lane: lane, index: band) }
        let saved = UserDefaults.standard.integer(forKey: "GelStain")
        gelView.stain = GelStain(rawValue: saved) ?? .ethidiumUV
        for stain in GelStain.allCases { stainPopup.addItem(withTitle: stain.title) }
        stainPopup.selectItem(at: gelView.stain.rawValue)
        stainPopup.isBordered = false
        stainPopup.toolTip = "How the gel was stained and photographed"
        stainPopup.target = self
        stainPopup.action = #selector(stainChanged(_:))
        runSlider.target = self
        runSlider.action = #selector(runChanged(_:))
        runSlider.isContinuous = true
        runSlider.controlSize = .small
        runSlider.toolTip = "How long the gel ran"
        runSlider.widthAnchor.constraint(equalToConstant: 110).isActive = true
        runLabel.font = .monospacedDigitSystemFont(ofSize: NSFont.smallSystemFontSize, weight: .regular)
        runLabel.textColor = .secondaryLabelColor
        runLabel.widthAnchor.constraint(equalToConstant: 54).isActive = true
        let runIcon = NSImageView(image: NSImage(systemSymbolName: "bolt.fill", accessibilityDescription: "Run")!)
        runIcon.contentTintColor = .secondaryLabelColor
        runIcon.toolTip = "How long the gel ran"
        copyButton.image = NSImage(systemSymbolName: "doc.on.doc", accessibilityDescription: "Copy Image")
        copyButton.title = "Copy Image"
        // Icon only, so the pill fits over the gel at the window's smallest.
        copyButton.imagePosition = .imageOnly
        copyButton.bezelStyle = .accessoryBarAction
        copyButton.target = self
        copyButton.action = #selector(copyImage(_:))
        copyButton.toolTip = "Copy the gel as a picture"
        let pillContent = NSStackView(views: [stainPopup, runIcon, runSlider, runLabel, copyButton])
        pillContent.setCustomSpacing(18, after: stainPopup)
        pillContent.setCustomSpacing(18, after: runLabel)
        pillContent.spacing = 10
        pillContent.edgeInsets = NSEdgeInsets(top: 0, left: 10, bottom: 0, right: 10)
        let pill = Glass.capsule(around: pillContent, height: 36)

        let stage = NSView()
        stage.appearance = NSAppearance(named: .darkAqua)
        for v in [gelView, pill] as [NSView] {
            v.translatesAutoresizingMaskIntoConstraints = false
            stage.addSubview(v)
        }
        NSLayoutConstraint.activate([
            gelView.topAnchor.constraint(equalTo: stage.topAnchor),
            gelView.bottomAnchor.constraint(equalTo: stage.bottomAnchor),
            gelView.leadingAnchor.constraint(equalTo: stage.leadingAnchor),
            gelView.trailingAnchor.constraint(equalTo: stage.trailingAnchor),
            pill.centerXAnchor.constraint(equalTo: stage.centerXAnchor),
            pill.bottomAnchor.constraint(equalTo: stage.bottomAnchor, constant: -16),
        ])
        gelView.bottomInset = 68

        let root = NSStackView(views: [sidebar, stage])
        root.orientation = .horizontal
        root.spacing = 0
        root.alignment = .top
        for v in [sidebar, stage] as [NSView] {
            v.heightAnchor.constraint(equalTo: root.heightAnchor).isActive = true
        }
        return root
    }

    // MARK: Lanes

    /// Rebuilds the gel from the lanes, running the ones that changed.
    private func refresh() {
        let ladder = Int32(max(0, ladderPopup.indexOfSelectedItem))
        guard let gel = oc_gel_new(ladder) else { return }
        defer { oc_gel_free(gel) }
        let percent = (agarosePopup.selectedItem?.representedObject as? NSNumber)?.doubleValue ?? 1
        oc_gel_set_percent(gel, percent)
        // Bromophenol blue runs with ~300 bp fragments, xylene cyanol with ~4 kb.
        gelView.dyeRuns = [oc_gel_run_fraction(300, percent), oc_gel_run_fraction(4000, percent)]
        fragments = [[]]
        for lane in lanes {
            withCStrings(lane.enzymes) { oc_gel_add_lane(gel, lane.doc.handle, $0, Int32(lane.enzymes.count)) }
            fragments.append(fragmentsOf(lane))
            if observed.insert(ObjectIdentifier(lane.doc)).inserted {
                lane.doc.observe { [weak self] what in
                    guard let self = self, what & Change.sequence != 0 else { return }
                    self.refresh()
                }
            }
        }
        var models: [GelLaneModel] = []
        var info = [OCGelBand](repeating: OCGelBand(), count: 512)
        for i in 0..<Int(oc_gel_lane_count(gel)) {
            let n = Int(min(oc_gel_lane_band_info(gel, Int32(i), &info, Int32(info.count)), Int32(info.count)))
            let bands = info.prefix(n).enumerated().map { j, b in
                GelBandModel(bp: Int(b.bp), run: b.run, intensity: b.intensity, reference: b.reference,
                             caption: i < fragments.count && j < fragments[i].count ? fragments[i][j].caption : "")
            }
            models.append(GelLaneModel(name: i == 0 ? "L" : "\(i)", bands: bands, isLadder: i == 0))
        }
        // A new percentage runs every lane again.
        let keys = ["ladder \(ladder) \(percent)"] + lanes.map { "\(percent) \($0.key)" }
        let running = Set(keys.indices.filter { $0 >= drawnKeys.count || drawnKeys[$0] != keys[$0] })
        drawnKeys = keys
        selected = min(max(1, selected), lanes.count)
        gelView.selectedLane = selected
        gelView.setLanes(models, running: running)
        fillSidebar()
    }

    /// The lane's fragments, largest first as its bands are, with their ends.
    private func fragmentsOf(_ lane: Lane) -> [Fragment] {
        let doc = lane.doc
        if lane.enzymes.isEmpty {
            return [Fragment(start: 0, length: doc.length,
                             caption: doc.isCircular ? "uncut, supercoiled" : "uncut, linear")]
        }
        var out = [OCFragmentInfo](repeating: OCFragmentInfo(), count: 400)
        let n = withCStrings(lane.enzymes) {
            oc_digest(doc.handle, $0, Int32(lane.enzymes.count), &out, Int32(out.count))
        }
        let frags = out.prefix(Int(min(n, Int32(out.count)))).map { f -> Fragment in
            let left = tupleString(f.left_enzyme), right = tupleString(f.right_enzyme)
            let ends = left.isEmpty && right.isEmpty ? "uncut"
                : "\(left.isEmpty ? "end" : left) – \(right.isEmpty ? "end" : right)"
            return Fragment(start: Int(f.start), length: Int(f.length), caption: ends)
        }
        // Nothing cut a circle: it runs supercoiled, as the gel draws it.
        if frags.count == 1 && doc.isCircular && frags[0].caption == "uncut" {
            return [Fragment(start: 0, length: doc.length, caption: "not cut, supercoiled")]
        }
        return frags.sorted { $0.length > $1.length }
    }

    private func fillSidebar() {
        // Lanes, numbered as on the gel.
        laneList.arrangedSubviews.forEach { $0.removeFromSuperview() }
        for (i, lane) in lanes.enumerated() {
            let name = NSTextField(labelWithString: lane.doc.name.isEmpty ? "Untitled" : lane.doc.name)
            name.font = .systemFont(ofSize: NSFont.systemFontSize, weight: .medium)
            name.lineBreakMode = .byTruncatingMiddle
            let what = NSTextField(labelWithString: lane.enzymes.isEmpty ? "Uncut"
                                   : lane.enzymes.joined(separator: " + "))
            what.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
            what.textColor = .secondaryLabelColor
            what.lineBreakMode = .byTruncatingTail
            let text = NSStackView(views: [name, what])
            text.orientation = .vertical
            text.alignment = .leading
            text.spacing = 1
            let row = ClickRow(content: text, badge: "\(i + 1)")
            row.isSelected = i + 1 == selected
            row.onClick = { [weak self] in self?.selectLane(i + 1) }
            row.setAccessibilityLabel("Lane \(i + 1), \(name.stringValue), \(what.stringValue)")
            laneList.addArrangedSubview(row)
            row.widthAnchor.constraint(equalTo: laneList.widthAnchor).isActive = true
        }
        addLaneButton.isEnabled = lanes.count < Self.maxLanes
        removeLaneButton.isEnabled = lanes.count > 1

        // The selected lane's sequence and enzymes.
        let lane = lanes[selected - 1]
        laneHeading.stringValue = "LANE \(selected)"
        openDocs = NSDocumentController.shared.documents.compactMap { ($0 as? SequenceDocument)?.core }
            .filter { !$0.isEmpty }
        if !openDocs.contains(where: { $0 === lane.doc }) { openDocs.insert(lane.doc, at: 0) }
        sequencePopup.removeAllItems()
        for d in openDocs {
            sequencePopup.addItem(withTitle: "\(d.name.isEmpty ? "Untitled" : d.name)  ·  \(d.length.formatted()) bp")
        }
        if let i = openDocs.firstIndex(where: { $0 === lane.doc }) { sequencePopup.selectItem(at: i) }
        enzymeField.objectValue = lane.enzymes
        fillEnzymeMenu(for: lane.doc)

        // Its fragments; clicking one selects it in its sequence.
        fragmentList.arrangedSubviews.forEach { $0.removeFromSuperview() }
        let frags = selected < fragments.count ? fragments[selected] : []
        fragmentHeading.stringValue = frags.count == 1 ? "1 FRAGMENT" : "\(frags.count) FRAGMENTS"
        for (j, f) in frags.enumerated() {
            let size = NSTextField(labelWithString: "\(f.length.formatted()) bp")
            size.font = .monospacedDigitSystemFont(ofSize: NSFont.smallSystemFontSize, weight: .semibold)
            size.alignment = .right
            size.widthAnchor.constraint(equalToConstant: 64).isActive = true
            let ends = NSTextField(labelWithString: f.caption)
            ends.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
            ends.textColor = .secondaryLabelColor
            ends.lineBreakMode = .byTruncatingTail
            let content = NSStackView(views: [size, ends])
            content.spacing = 10
            let row = ClickRow(content: content)
            let laneNumber = selected
            row.onClick = { [weak self] in self?.pickFragment(lane: laneNumber, index: j) }
            row.toolTip = "Select in \(lanes[selected - 1].doc.name)"
            row.setAccessibilityLabel("\(f.length) bp, \(f.caption)")
            fragmentList.addArrangedSubview(row)
            row.widthAnchor.constraint(equalTo: fragmentList.widthAnchor).isActive = true
        }
    }

    /// Enzymes that cut `doc`, the fewest cuts first, for the Add menu.
    private func fillEnzymeMenu(for doc: CoreDocument) {
        enzymeMenu.removeAllItems()
        enzymeMenu.addItem(withTitle: "")
        enzymeMenu.lastItem?.image = NSImage(systemSymbolName: "plus.circle", accessibilityDescription: "Add Enzyme")
        let cutting = doc.enzymes.filter { $0.cuts > 0 }
        let groups: [(String, (Int) -> Bool)] = [("Cut once", { $0 == 1 }), ("Cut twice", { $0 == 2 }),
                                                 ("Cut 3 to 6 times", { $0 >= 3 && $0 <= 6 })]
        let menu = enzymeMenu.menu
        for (title, test) in groups {
            let names = Set(cutting.filter { test($0.cuts) }.map { $0.name }).sorted()
            guard !names.isEmpty else { continue }
            if (menu?.items.count ?? 0) > 1 { menu?.addItem(.separator()) }
            menu?.addItem(NSMenuItem.sectionHeader(title: title))
            for name in names {
                let item = NSMenuItem(title: name, action: #selector(addEnzyme(_:)), keyEquivalent: "")
                item.target = self
                menu?.addItem(item)
            }
        }
    }

    private func selectLane(_ lane: Int) {
        guard lane >= 1 && lane <= lanes.count, lane != selected else { return }
        selected = lane
        gelView.selectedLane = lane
        fillSidebar()
    }

    /// Selects fragment `index` of `lane` in its sequence's window.
    private func pickFragment(lane: Int, index: Int) {
        guard lane >= 1 && lane <= lanes.count, lane < fragments.count, index < fragments[lane].count else { return }
        let f = fragments[lane][index]
        lanes[lane - 1].doc.select(start: f.start, length: f.length)
    }

    @objc private func addLane(_ sender: Any?) {
        guard lanes.count < Self.maxLanes else { return }
        // Same sequence as the lane being edited, uncut, ready for an enzyme.
        lanes.insert(Lane(doc: lanes[selected - 1].doc, enzymes: []), at: selected)
        selected += 1
        refresh()
        window?.makeFirstResponder(enzymeField)
    }

    @objc private func removeLane(_ sender: Any?) {
        guard lanes.count > 1 else { return }
        lanes.remove(at: selected - 1)
        selected = max(1, selected - 1)
        // Lanes after it moved over: they run again in their new places.
        refresh()
    }

    @objc private func ladderChanged(_ sender: Any?) {
        UserDefaults.standard.set(ladderPopup.indexOfSelectedItem, forKey: "GelLadder")
        refresh()
    }

    @objc private func sequenceChanged(_ sender: Any?) {
        let i = sequencePopup.indexOfSelectedItem
        guard i >= 0 && i < openDocs.count, openDocs[i] !== lanes[selected - 1].doc else { return }
        // Another sequence's enzymes may not cut this one; keep those that do.
        let cutting = Set(openDocs[i].enzymes.filter { $0.cuts > 0 }.map { $0.name })
        lanes[selected - 1] = Lane(doc: openDocs[i], enzymes: lanes[selected - 1].enzymes.filter { cutting.contains($0) })
        refresh()
    }

    @objc private func addEnzyme(_ sender: NSMenuItem) {
        guard !lanes[selected - 1].enzymes.contains(sender.title) else { return }
        lanes[selected - 1].enzymes.append(sender.title)
        refresh()
    }

    @objc private func stainChanged(_ sender: Any?) {
        gelView.stain = GelStain(rawValue: stainPopup.indexOfSelectedItem) ?? .ethidiumUV
        UserDefaults.standard.set(gelView.stain.rawValue, forKey: "GelStain")
    }

    @objc private func agaroseChanged(_ sender: Any?) {
        if let percent = (agarosePopup.selectedItem?.representedObject as? NSNumber)?.doubleValue {
            UserDefaults.standard.set(percent, forKey: "GelPercent")
        }
        refresh()
    }

    @objc private func runChanged(_ sender: Any?) {
        gelView.runLevel = runSlider.doubleValue
        runLabel.stringValue = runSlider.doubleValue >= 0.995 ? "Full run"
            : "\(Int((runSlider.doubleValue * 100).rounded()))% run"
    }


    @objc private func copyImage(_ sender: Any?) {
        guard let image = gelView.snapshot() else { return }
        NSPasteboard.general.clearContents()
        NSPasteboard.general.writeObjects([image])
        // A moment's tick on the button says it worked.
        copyButton.image = NSImage(systemSymbolName: "checkmark", accessibilityDescription: "Copied")
        copyButton.title = "Copied"
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.2) { [weak self] in
            self?.copyButton.image = NSImage(systemSymbolName: "doc.on.doc", accessibilityDescription: "Copy Image")
            self?.copyButton.title = "Copy Image"
        }
    }

    @objc private func done(_ sender: Any?) { window?.close() }
    override func cancelOperation(_ sender: Any?) { window?.close() }

    // MARK: Enzymes typed as tokens

    /// Known enzyme names, by lowercase name, for matching what's typed.
    private lazy var enzymeNames: [String: String] = {
        Dictionary(self.template.enzymes.map { ($0.name.lowercased(), $0.name) }, uniquingKeysWith: { a, _ in a })
    }()

    func tokenField(_ tokenField: NSTokenField, completionsForSubstring substring: String, indexOfToken tokenIndex: Int,
                    indexOfSelectedItem selectedIndex: UnsafeMutablePointer<Int>?) -> [Any]? {
        let s = substring.lowercased()
        let doc = lanes[selected - 1].doc
        // Enzymes that cut this sequence first, then the rest.
        let all = doc.enzymes
        let cutting = all.filter { $0.cuts > 0 && $0.name.lowercased().hasPrefix(s) }.map { $0.name }
        let others = all.filter { $0.cuts == 0 && $0.name.lowercased().hasPrefix(s) }.map { $0.name }
        selectedIndex?.pointee = 0
        return Array((Set(cutting).sorted() + Set(others).sorted()).prefix(30))
    }

    func tokenField(_ tokenField: NSTokenField, shouldAdd tokens: [Any], at index: Int) -> [Any] {
        DispatchQueue.main.async { [weak self] in self?.enzymesEdited(nil) }
        return tokens
    }

    func controlTextDidEndEditing(_ obj: Notification) { enzymesEdited(nil) }

    /// Takes the field's tokens as the lane's enzymes; names it doesn't know are left out.
    @objc private func enzymesEdited(_ sender: Any?) {
        let typed = (enzymeField.objectValue as? [Any] ?? []).compactMap { $0 as? String }
            .map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
        var known: [String] = []
        var unknown: [String] = []
        for t in typed {
            if let name = enzymeNames[t.lowercased()] {
                if !known.contains(name) { known.append(name) }
            } else {
                unknown.append(t)
            }
        }
        enzymeNote.stringValue = unknown.isEmpty ? "" : "Not a known enzyme: \(unknown.joined(separator: ", "))"
        enzymeNote.isHidden = unknown.isEmpty
        guard known != lanes[selected - 1].enzymes else { return }
        lanes[selected - 1].enzymes = known
        let note = enzymeNote.stringValue
        refresh()
        // Keep the warning (refresh resets the field to the known names).
        enzymeNote.stringValue = note
        enzymeNote.isHidden = note.isEmpty
    }

    // MARK: Window

    private static func withoutExtension(_ title: String) -> String {
        let ext = (title as NSString).pathExtension.lowercased()
        let known: Set<String> = ["dna", "gb", "gbk", "genbank", "embl", "emb", "fasta", "fa", "fna", "ape", "seq"]
        return known.contains(ext) ? (title as NSString).deletingPathExtension : title
    }

    func windowDidBecomeKey(_ notification: Notification) { chrome?.setDimmed(true) }
    func windowDidResignKey(_ notification: Notification) { chrome?.setDimmed(false) }

    func windowWillClose(_ notification: Notification) {
        chrome?.detach()
        // Let go after the close finishes.
        DispatchQueue.main.async { Self.showing.removeAll { $0 === self } }
    }

    // MARK: Launch check

    private static var visible: [GelWizard] { showing.filter { $0.window?.isVisible == true } }
    static var isAnyOpen: Bool { !visible.isEmpty }
    static var openWindows: [NSWindow] { visible.compactMap { $0.window } }
    /// Each lane's band sizes in the first open gel, the ladder first.
    static var bands: [[Int]] { visible.first?.gelView.lanes.map { $0.bands.map { $0.bp } } ?? [] }
    /// As adding a lane and typing `enzymes` into it.
    static func addLane(enzymes: [String]) {
        guard let w = visible.first else { return }
        w.addLane(nil)
        w.enzymeField.objectValue = enzymes
        w.enzymesEdited(nil)
    }
    /// Each lane's band runs in the first open gel.
    static var runs: [[Double]] { visible.first?.gelView.lanes.map { $0.bands.map { $0.run } } ?? [] }
    /// As picking `percent` in the GEL popup.
    static func setAgarose(_ percent: Double) {
        guard let w = visible.first,
              let item = w.agarosePopup.itemArray.first(where: {
                  abs(((($0.representedObject as? NSNumber)?.doubleValue) ?? 0) - percent) < 0.01 }) else { return }
        w.agarosePopup.select(item)
        w.agaroseChanged(nil)
    }
    /// As dragging the run slider to `level`, and choosing `stain`.
    static func setRun(_ level: Double, stain: GelStain) {
        guard let w = visible.first else { return }
        w.runSlider.doubleValue = level
        w.runChanged(nil)
        w.stainPopup.selectItem(at: stain.rawValue)
        w.stainChanged(nil)
    }
    static func closeAll() { visible.forEach { $0.window?.close() } }
}
