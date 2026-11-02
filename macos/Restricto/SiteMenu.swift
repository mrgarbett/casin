// SPDX-License-Identifier: GPL-3.0-or-later
import AppKit
import RestrictoCore

/// Right-click on an enzyme site, on the map or in the sequence: select from
/// it to another site, hide the enzyme, or digest with it.
enum SiteMenu {
    /// Menu items keep their target weakly, so the closures ride along as
    /// each item's represented object and one shared target runs them.
    private final class Action: NSObject {
        let run: () -> Void
        init(_ run: @escaping () -> Void) { self.run = run }
    }

    private final class Runner: NSObject {
        @objc func run(_ sender: NSMenuItem) { (sender.representedObject as? Action)?.run() }
    }

    private static let runner = Runner()

    private static func item(_ title: String, _ run: @escaping () -> Void) -> NSMenuItem {
        let item = NSMenuItem(title: title, action: #selector(Runner.run(_:)), keyEquivalent: "")
        item.target = runner
        item.representedObject = Action(run)
        return item
    }

    /// The menu for visible site `site`; `onDigest` opens Digest and Gel with an enzyme.
    static func menu(for site: Int, in document: CoreDocument, onDigest: @escaping (String) -> Void) -> NSMenu {
        let names = document.visibleSiteEnzymes
        let name = names[site]
        let menu = NSMenu()
        // The other sites in map order, with where they cut to tell repeats apart.
        let others = NSMenu()
        for (i, other) in names.enumerated() where i != site {
            others.addItem(item("\(other) (\((document.visibleSiteCut(i) + 1).formatted()))") {
                document.activateSite(site)
                document.extendToSite(i)
            })
        }
        if others.numberOfItems > 0 {
            let select = NSMenuItem(title: "Select to", action: nil, keyEquivalent: "")
            select.submenu = others
            menu.addItem(select)
        }
        menu.addItem(item("Hide \(name)") { document.setEnzyme(name, hidden: true) })
        menu.addItem(.separator())
        menu.addItem(item("Digest with \(name)…") { onDigest(name) })
        return menu
    }
}
