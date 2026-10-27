// SPDX-License-Identifier: GPL-3.0-or-later
// Small sheets for the tools: a form of labelled controls, and a result panel
// with text or an image. Plain AppKit, the same on every macOS version.
import AppKit

/// Labelled rows in a grid, shown in an alert sheet with OK and Cancel.
final class Form {
    private let title: String
    private let message: String
    private let grid = NSGridView(numberOfColumns: 2, rows: 0)

    init(_ title: String, _ message: String = "") {
        self.title = title
        self.message = message
        grid.rowSpacing = 8
        grid.columnSpacing = 10
        grid.xPlacement = .leading
        grid.rowAlignment = .firstBaseline
    }

    private func addRow(_ label: String, _ control: NSView) {
        let text = NSTextField(labelWithString: label.isEmpty ? "" : label + ":")
        text.alignment = .right
        grid.addRow(with: [text, control])
        grid.column(at: 0).xPlacement = .trailing
    }

    @discardableResult
    func text(_ label: String, _ value: String = "", placeholder: String = "", width: CGFloat = 260) -> NSTextField {
        let field = NSTextField(string: value)
        field.placeholderString = placeholder
        field.widthAnchor.constraint(equalToConstant: width).isActive = true
        addRow(label, field)
        return field
    }

    @discardableResult
    func number(_ label: String, _ value: Double) -> NSTextField {
        let field = NSTextField(string: value == value.rounded() ? String(Int(value)) : String(value))
        field.alignment = .right
        field.widthAnchor.constraint(equalToConstant: 70).isActive = true
        addRow(label, field)
        return field
    }

    @discardableResult
    func popup(_ label: String, _ items: [String], selected: Int = 0) -> NSPopUpButton {
        let popup = NSPopUpButton(frame: .zero, pullsDown: false)
        popup.addItems(withTitles: items)
        if selected >= 0 && selected < items.count { popup.selectItem(at: selected) }
        addRow(label, popup)
        return popup
    }

    @discardableResult
    func check(_ label: String, _ title: String, on: Bool = false) -> NSButton {
        let box = NSButton(checkboxWithTitle: title, target: nil, action: nil)
        box.state = on ? .on : .off
        addRow(label, box)
        return box
    }

    /// One checkbox per item, stacked under one label.
    func checks(_ label: String, _ items: [String], on: Bool = true) -> [NSButton] {
        let boxes = items.map { item -> NSButton in
            let box = NSButton(checkboxWithTitle: item, target: nil, action: nil)
            box.state = on ? .on : .off
            return box
        }
        let stack = NSStackView(views: boxes)
        stack.orientation = .vertical
        stack.alignment = .leading
        stack.spacing = 4
        addRow(label, stack)
        return boxes
    }

    func note(_ text: String) {
        let label = NSTextField(wrappingLabelWithString: text)
        label.textColor = .secondaryLabelColor
        label.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        label.preferredMaxLayoutWidth = 300
        addRow("", label)
    }

    /// Shows the form as a sheet (or a window-less alert) and calls `then` on OK.
    func run(in window: NSWindow?, ok: String = "OK", then: @escaping () -> Void) {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = message
        alert.addButton(withTitle: ok)
        alert.addButton(withTitle: "Cancel")
        grid.layoutSubtreeIfNeeded()
        grid.frame = NSRect(origin: .zero, size: grid.fittingSize)
        alert.accessoryView = grid
        if let window = window {
            alert.beginSheetModal(for: window) { response in
                if response == .alertFirstButtonReturn { then() }
            }
        } else if alert.runModal() == .alertFirstButtonReturn {
            then()
        }
    }
}

extension NSTextField {
    /// The field's number, or `fallback` when it isn't one.
    func number(or fallback: Double) -> Double {
        Double(stringValue.trimmingCharacters(in: .whitespaces)) ?? fallback
    }
}

/// Shows text (monospaced, selectable) in a sheet.
func showText(_ title: String, _ message: String, _ text: String, in window: NSWindow?) {
    let alert = NSAlert()
    alert.messageText = title
    alert.informativeText = message
    if !text.isEmpty {
        let scroll = NSTextView.scrollableTextView()
        scroll.frame = NSRect(x: 0, y: 0, width: 460, height: min(320, 40 + 15 * CGFloat(text.split(separator: "\n").count)))
        if let view = scroll.documentView as? NSTextView {
            view.string = text
            view.isEditable = false
            view.font = .monospacedSystemFont(ofSize: 11, weight: .regular)
        }
        alert.accessoryView = scroll
    }
    if let window = window { alert.beginSheetModal(for: window) } else { alert.runModal() }
}

/// Shows an image (a gel) in a sheet.
func showImage(_ title: String, _ message: String, _ image: NSImage, in window: NSWindow?) {
    let alert = NSAlert()
    alert.messageText = title
    alert.informativeText = message
    let view = NSImageView(image: image)
    view.frame = NSRect(origin: .zero, size: image.size)
    alert.accessoryView = view
    if let window = window { alert.beginSheetModal(for: window) } else { alert.runModal() }
}

/// An error from the core (a malloc'd message) in a sheet.
func showError(_ title: String, _ error: UnsafeMutablePointer<CChar>?, in window: NSWindow?) {
    let text = takeString(error)
    showText(title, text.isEmpty ? "That didn't work." : text.prefix(1).uppercased() + text.dropFirst(), "",
             in: window)
}
