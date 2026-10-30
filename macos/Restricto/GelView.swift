// SPDX-License-Identifier: GPL-3.0-or-later
// The gel as an imager shows it: a dark agarose slab with its wells, and
// bands that glow in the stain's colour, brighter where there's more DNA.
// When a lane changes, its bands run down from the well to where they stop.
// The core says where each band runs and how bright it is (oc_gel_*); this
// view only draws.
import AppKit
import QuartzCore

struct GelBandModel {
    let bp: Int
    /// 0 at the wells, 1 at the bottom of the run.
    let run: Double
    let intensity: Double
    let reference: Bool
    /// What the band is: "EcoRI – BamHI", "uncut, supercoiled"…
    let caption: String
}

struct GelLaneModel {
    let name: String
    let bands: [GelBandModel]
    let isLadder: Bool
}

enum GelStain: Int {
    case uv = 0, blueLight = 1

    var gelTop: NSColor { self == .uv ? NSColor(srgbRed: 0.17, green: 0.12, blue: 0.21, alpha: 1)
                                      : NSColor(srgbRed: 0.07, green: 0.13, blue: 0.13, alpha: 1) }
    var gelBottom: NSColor { self == .uv ? NSColor(srgbRed: 0.10, green: 0.07, blue: 0.13, alpha: 1)
                                         : NSColor(srgbRed: 0.04, green: 0.08, blue: 0.08, alpha: 1) }
    var core: NSColor { self == .uv ? NSColor(srgbRed: 1.0, green: 0.94, blue: 0.97, alpha: 1)
                                    : NSColor(srgbRed: 0.93, green: 1.0, blue: 0.86, alpha: 1) }
    var glow: NSColor { self == .uv ? NSColor(srgbRed: 1.0, green: 0.36, blue: 0.64, alpha: 1)
                                    : NSColor(srgbRed: 0.36, green: 1.0, blue: 0.55, alpha: 1) }
}

final class GelView: NSView {
    override var isFlipped: Bool { true }

    var stain: GelStain = .uv { didSet { needsDisplay = true } }
    /// The lane being edited, lit a little.
    var selectedLane = 1 { didSet { needsDisplay = true } }
    /// Room left at the bottom for the floating controls.
    var bottomInset: CGFloat = 64 { didSet { needsDisplay = true } }
    var onSelectLane: ((Int) -> Void)?
    var onPickBand: ((Int, Int) -> Void)?

    private(set) var lanes: [GelLaneModel] = []
    /// How far each lane has run, 0..1 (linear time; eased when drawn).
    private var progress: [Double] = []
    private var link: CADisplayLink?
    private var lastTick: CFTimeInterval = 0
    private var hover: (lane: Int, band: Int?)?
    private static let runSeconds = 0.9

    /// Agarose isn't glass: a faint, fixed grain over the slab.
    private static let grain: [CGPoint] = {
        var seed: UInt32 = 12345
        func next() -> CGFloat {
            seed = seed &* 1_664_525 &+ 1_013_904_223
            return CGFloat(seed >> 8) / CGFloat(1 << 24)
        }
        return (0..<420).map { _ in CGPoint(x: next(), y: next()) }
    }()

    /// Shows `lanes`; the lanes in `running` run from their wells again.
    func setLanes(_ lanes: [GelLaneModel], running: Set<Int>) {
        let old = progress
        self.lanes = lanes
        progress = lanes.indices.map { running.contains($0) ? 0 : ($0 < old.count ? old[$0] : 1) }
        if NSWorkspace.shared.accessibilityDisplayShouldReduceMotion {
            progress = progress.map { _ in 1 }
        }
        if progress.contains(where: { $0 < 1 }) { startRunning() }
        if let h = hover, h.lane >= lanes.count { hover = nil }
        let described: [String] = lanes.map { lane in
            let sizes: [String] = lane.bands.map { band in "\(band.bp) bp" }
            return lane.name + ", " + sizes.joined(separator: ", ")
        }
        setAccessibilityLabel("Gel, \(lanes.count) lanes: " + described.joined(separator: "; "))
        needsDisplay = true
    }

    override init(frame: NSRect) {
        super.init(frame: frame)
        setAccessibilityElement(true)
        setAccessibilityRole(.image)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    // MARK: Running

    private func startRunning() {
        guard link == nil else { return }
        lastTick = CACurrentMediaTime()
        let l = displayLink(target: self, selector: #selector(tick(_:)))
        l.add(to: .main, forMode: .common)
        link = l
    }

    @objc private func tick(_ sender: CADisplayLink) {
        let now = CACurrentMediaTime()
        let step = min(0.05, now - lastTick) / Self.runSeconds
        lastTick = now
        progress = progress.map { min(1, $0 + step) }
        if !progress.contains(where: { $0 < 1 }) {
            link?.invalidate()
            link = nil
        }
        needsDisplay = true
    }

    /// Ends every run at once (for a still image).
    func finishRunning() {
        progress = progress.map { _ in 1 }
        link?.invalidate()
        link = nil
        needsDisplay = true
    }

    private func eased(_ lane: Int) -> CGFloat {
        let t = lane < progress.count ? progress[lane] : 1
        return CGFloat(1 - pow(1 - t, 3))
    }

    // MARK: Geometry

    private struct Geometry {
        var slab: NSRect
        var laneW: CGFloat
        var gap: CGFloat
        var runTop: CGFloat
        var runBottom: CGFloat
        func laneX(_ i: Int) -> CGFloat { slab.minX + gap + CGFloat(i) * (laneW + gap) }
        func y(_ run: Double, _ eased: CGFloat) -> CGFloat { runTop + CGFloat(run) * eased * (runBottom - runTop) }
    }

    private var geometry: Geometry {
        let area = NSRect(x: 24, y: 36, width: max(100, bounds.width - 48),
                          height: max(120, bounds.height - 36 - bottomInset))
        let labelW: CGFloat = 60
        let n = CGFloat(max(1, lanes.count))
        let gap: CGFloat = 14
        let laneW = min(64, max(26, (area.width - labelW - gap * (n + 1)) / n))
        let slabW = n * laneW + (n + 1) * gap
        let slabX = area.minX + labelW + max(0, (area.width - labelW - slabW) / 2)
        let slab = NSRect(x: slabX, y: area.minY + 24, width: slabW, height: area.maxY - area.minY - 24)
        return Geometry(slab: slab, laneW: laneW, gap: gap, runTop: slab.minY + 34, runBottom: slab.maxY - 22)
    }

    private func bandHeight(_ b: GelBandModel) -> CGFloat { 3.5 + 3 * CGFloat(b.intensity) }

    // MARK: Drawing

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        // The imager's dark box, a little lighter in the middle.
        let backdrop = NSGradient(colors: [NSColor(white: 0.12, alpha: 1), NSColor(white: 0.04, alpha: 1)])
        backdrop?.draw(in: bounds, relativeCenterPosition: NSPoint(x: 0, y: 0.1))

        let g = geometry
        // The slab, with a lit top edge and a faint grain.
        let slabPath = NSBezierPath(roundedRect: g.slab, xRadius: 12, yRadius: 12)
        NSGraphicsContext.saveGraphicsState()
        let shadow = NSShadow()
        shadow.shadowColor = NSColor.black.withAlphaComponent(0.6)
        shadow.shadowBlurRadius = 24
        shadow.shadowOffset = NSSize(width: 0, height: -6)
        shadow.set()
        stain.gelBottom.setFill()
        slabPath.fill()
        NSGraphicsContext.restoreGraphicsState()
        NSGradient(starting: stain.gelTop, ending: stain.gelBottom)?.draw(in: slabPath, angle: 90)
        NSGraphicsContext.saveGraphicsState()
        slabPath.addClip()
        NSColor.white.withAlphaComponent(0.035).setFill()
        for p in Self.grain {
            NSRect(x: g.slab.minX + p.x * g.slab.width, y: g.slab.minY + p.y * g.slab.height,
                   width: 1, height: 1).fill()
        }
        NSGraphicsContext.restoreGraphicsState()
        NSColor.white.withAlphaComponent(0.10).setStroke()
        let rim = NSBezierPath(roundedRect: g.slab.insetBy(dx: 0.5, dy: 0.5), xRadius: 12, yRadius: 12)
        rim.lineWidth = 1
        rim.stroke()

        let nameFont = NSFont.systemFont(ofSize: 11, weight: .semibold)
        for (i, lane) in lanes.enumerated() {
            let x = g.laneX(i)
            // The lane being edited, and the one under the pointer.
            let lit: CGFloat = i == selectedLane ? 0.07 : (hover?.lane == i ? 0.035 : 0)
            if lit > 0 {
                NSColor.white.withAlphaComponent(lit).setFill()
                NSBezierPath(roundedRect: NSRect(x: x - 4, y: g.slab.minY + 6, width: g.laneW + 8,
                                                 height: g.slab.height - 12), xRadius: 7, yRadius: 7).fill()
            }
            // Its name over the slab.
            let nameColor = i == selectedLane ? NSColor.white : NSColor.white.withAlphaComponent(0.6)
            drawText(lane.name, font: nameFont, color: nameColor, centeredAt: NSPoint(x: x + g.laneW / 2,
                                                                                      y: g.slab.minY - 13))
            // The well: a slot cut into the gel.
            let well = NSRect(x: x + 2, y: g.slab.minY + 14, width: g.laneW - 4, height: 8)
            NSColor.black.withAlphaComponent(0.7).setFill()
            NSBezierPath(roundedRect: well, xRadius: 2, yRadius: 2).fill()
            NSColor.white.withAlphaComponent(0.12).setFill()
            NSRect(x: well.minX + 2, y: well.maxY - 1, width: well.width - 4, height: 1).fill()

            let e = eased(i)
            let fadeIn = CGFloat(min(1, (i < progress.count ? progress[i] : 1) * 4))
            for (b, band) in lane.bands.enumerated() {
                let h = bandHeight(band)
                let y = g.y(band.run, e)
                let rect = NSRect(x: x + 3, y: y - h / 2, width: g.laneW - 6, height: h)
                let hovered = hover?.lane == i && hover?.band == b
                drawBand(rect, intensity: CGFloat(band.intensity) * fadeIn, hovered: hovered, in: ctx)
            }
        }

        // The ladder's sizes on the left, the reference bands bolder.
        if let ladder = lanes.first, ladder.isLadder {
            let e = eased(0)
            var lastY = -CGFloat.infinity
            for band in ladder.bands.sorted(by: { $0.run < $1.run }) {
                let y = g.y(band.run, e)
                guard y - lastY >= 11 else { continue }
                lastY = y
                let font = NSFont.monospacedDigitSystemFont(ofSize: 10, weight: band.reference ? .semibold : .regular)
                let color = NSColor.white.withAlphaComponent((band.reference ? 0.8 : 0.5) * e)
                drawText(Self.sizeText(band.bp), font: font, color: color, rightAt: NSPoint(x: g.slab.minX - 10, y: y))
            }
        }

        if lanes.count <= 1 {
            drawText("Add a lane to run a sample", font: .systemFont(ofSize: 13),
                     color: NSColor.white.withAlphaComponent(0.5), centeredAt: NSPoint(x: g.slab.midX, y: g.slab.midY))
        }
        if let h = hover, let b = h.band, h.lane < lanes.count, b < lanes[h.lane].bands.count {
            drawCallout(lane: h.lane, band: b, g)
        }
    }

    /// A band: brightest along its middle, fading at the lane's edges, with a
    /// glow in the stain's colour that grows with the amount of DNA.
    private func drawBand(_ rect: NSRect, intensity: CGFloat, hovered: Bool, in ctx: CGContext) {
        guard intensity > 0.01 else { return }
        let a = min(1, intensity * (hovered ? 1.25 : 1))
        ctx.saveGState()
        ctx.setShadow(offset: .zero, blur: 4 + 12 * a, color: stain.glow.withAlphaComponent(0.9 * a).cgColor)
        ctx.beginTransparencyLayer(auxiliaryInfo: nil)
        let path = CGPath(roundedRect: rect, cornerWidth: rect.height / 2, cornerHeight: rect.height / 2,
                          transform: nil)
        ctx.addPath(path)
        ctx.clip()
        let c = stain.core
        let colors = [c.withAlphaComponent(0.45 * a), c.withAlphaComponent(a), c.withAlphaComponent(a),
                      c.withAlphaComponent(0.45 * a)].map { $0.cgColor } as CFArray
        if let gradient = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB), colors: colors,
                                     locations: [0, 0.3, 0.7, 1]) {
            ctx.drawLinearGradient(gradient, start: CGPoint(x: rect.minX, y: rect.midY),
                                   end: CGPoint(x: rect.maxX, y: rect.midY), options: [])
        }
        ctx.endTransparencyLayer()
        ctx.restoreGState()
        if hovered {
            NSColor.white.withAlphaComponent(0.55).setStroke()
            let ring = NSBezierPath(roundedRect: rect.insetBy(dx: -3, dy: -3), xRadius: rect.height / 2 + 3,
                                    yRadius: rect.height / 2 + 3)
            ring.lineWidth = 1
            ring.stroke()
        }
    }

    /// The band's size and what it is, in a dark bubble beside the lane.
    private func drawCallout(lane: Int, band: Int, _ g: Geometry) {
        let b = lanes[lane].bands[band]
        let title = NSAttributedString(string: "\(b.bp.formatted()) bp", attributes: [
            .font: NSFont.systemFont(ofSize: 12, weight: .semibold), .foregroundColor: NSColor.white])
        let detail = NSAttributedString(string: b.caption, attributes: [
            .font: NSFont.systemFont(ofSize: 11), .foregroundColor: NSColor.white.withAlphaComponent(0.65)])
        let w = max(title.size().width, b.caption.isEmpty ? 0 : detail.size().width) + 20
        let h: CGFloat = b.caption.isEmpty ? 26 : 42
        let y = g.y(b.run, eased(lane))
        var x = g.laneX(lane) + g.laneW + 10
        if x + w > bounds.maxX - 8 { x = g.laneX(lane) - 10 - w }
        let box = NSRect(x: x, y: y - h / 2, width: w, height: h)
        let path = NSBezierPath(roundedRect: box, xRadius: 9, yRadius: 9)
        NSColor(white: 0.06, alpha: 0.88).setFill()
        path.fill()
        NSColor.white.withAlphaComponent(0.16).setStroke()
        path.lineWidth = 1
        path.stroke()
        title.draw(at: NSPoint(x: box.minX + 10, y: box.minY + 5))
        if !b.caption.isEmpty { detail.draw(at: NSPoint(x: box.minX + 10, y: box.minY + 22)) }
    }

    private func drawText(_ s: String, font: NSFont, color: NSColor, centeredAt p: NSPoint) {
        let a = NSAttributedString(string: s, attributes: [.font: font, .foregroundColor: color])
        let size = a.size()
        a.draw(at: NSPoint(x: p.x - size.width / 2, y: p.y - size.height / 2))
    }

    private func drawText(_ s: String, font: NSFont, color: NSColor, rightAt p: NSPoint) {
        let a = NSAttributedString(string: s, attributes: [.font: font, .foregroundColor: color])
        let size = a.size()
        a.draw(at: NSPoint(x: p.x - size.width, y: p.y - size.height / 2))
    }

    /// "500", "1.5 kb", "10 kb".
    static func sizeText(_ bp: Int) -> String {
        if bp >= 1000 && bp % 1000 == 0 { return "\(bp / 1000) kb" }
        if bp >= 1000 && bp % 100 == 0 { return "\(bp / 1000).\(bp / 100 % 10) kb" }
        return "\(bp)"
    }

    // MARK: Pointer

    private func hit(_ p: NSPoint) -> (lane: Int, band: Int?)? {
        let g = geometry
        guard g.slab.insetBy(dx: 0, dy: -24).contains(p) else { return nil }
        guard let lane = lanes.indices.first(where: {
            abs(p.x - (g.laneX($0) + g.laneW / 2)) <= (g.laneW + g.gap) / 2 }) else { return nil }
        var best: (Int, CGFloat)?
        for (b, band) in lanes[lane].bands.enumerated() {
            let d = abs(p.y - g.y(band.run, eased(lane)))
            if d <= max(5, bandHeight(band) / 2 + 3), d < (best?.1 ?? .infinity) { best = (b, d) }
        }
        return (lane, best?.0)
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseMoved, .mouseEnteredAndExited,
                                                              .activeInKeyWindow, .inVisibleRect],
                                       owner: self, userInfo: nil))
    }

    override func mouseMoved(with event: NSEvent) {
        let h = hit(convert(event.locationInWindow, from: nil))
        if h?.lane != hover?.lane || h?.band != hover?.band {
            hover = h
            needsDisplay = true
        }
        if h?.band != nil { NSCursor.pointingHand.set() } else { NSCursor.arrow.set() }
    }

    override func mouseExited(with event: NSEvent) {
        hover = nil
        NSCursor.arrow.set()
        needsDisplay = true
    }

    override func mouseDown(with event: NSEvent) {
        guard let h = hit(convert(event.locationInWindow, from: nil)) else { return }
        onSelectLane?(h.lane)
        if let b = h.band { onPickBand?(h.lane, b) }
    }

    /// The gel as a picture, without the pointer's bubble.
    func snapshot() -> NSImage? {
        let keep = hover
        hover = nil
        finishRunning()
        defer { hover = keep; needsDisplay = true }
        guard let rep = bitmapImageRepForCachingDisplay(in: bounds) else { return nil }
        cacheDisplay(in: bounds, to: rep)
        let image = NSImage(size: bounds.size)
        image.addRepresentation(rep)
        return image
    }
}
