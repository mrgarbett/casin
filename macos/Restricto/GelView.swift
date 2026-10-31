// SPDX-License-Identifier: GPL-3.0-or-later
// The gel as it looks on the imager: an agarose slab lying on the light box,
// its wells cut by the comb, bands glowing in the stain's colour (brighter
// where there's more DNA, a little wider and softer the further they ran,
// smiling slightly as real bands do), and the loading dyes' shadows marking
// how far the run went. When a lane changes, its bands run down from the
// well; the run slider stops the whole gel part way. The core says where each
// band runs and how bright it is (oc_gel_*); this view only draws.
import AppKit
import QuartzCore

struct GelBandModel {
    let bp: Int
    /// 0 at the wells, 1 at the bottom of a full run (more: ran off the gel).
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

/// The comb that cast the wells: how many, how wide each is, and how thick
/// a plug of DNA it loads (a thin comb gives sharper, brighter bands).
struct GelComb: Equatable {
    let name: String
    let teeth: Int
    /// Tooth width and thickness, in millimetres.
    let width: Double
    let thickness: Double

    /// Combs as sold for a 7 cm mini gel tray.
    static let common: [GelComb] = [
        GelComb(name: "8 wells · 1.5 mm", teeth: 8, width: 6.0, thickness: 1.5),
        GelComb(name: "10 wells · 1.0 mm", teeth: 10, width: 5.0, thickness: 1.0),
        GelComb(name: "12 wells · 1.0 mm", teeth: 12, width: 4.0, thickness: 1.0),
        GelComb(name: "15 wells · 0.75 mm", teeth: 15, width: 3.2, thickness: 0.75),
        GelComb(name: "20 wells · 0.75 mm", teeth: 20, width: 2.3, thickness: 0.75),
    ]
    static let standard = common[1]
    /// How much thinner (or thicker) than a 1 mm comb's a fresh band is.
    var plug: CGFloat { CGFloat(0.6 + 0.4 * thickness) }
}

/// How the gel was stained and photographed.
enum GelStain: Int, CaseIterable {
    case ethidiumUV = 0, sybrSafe = 1, blackAndWhite = 2

    var title: String {
        switch self {
        case .ethidiumUV: return "EtBr · UV"
        case .sybrSafe: return "SYBR Safe · Blue Light"
        case .blackAndWhite: return "Black & White"
        }
    }

    private static func rgb(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat) -> NSColor {
        NSColor(srgbRed: r, green: g, blue: b, alpha: 1)
    }

    /// The light box behind the gel, at the gel's top and bottom.
    var gelTop: NSColor {
        switch self {
        case .ethidiumUV: return Self.rgb(0.20, 0.10, 0.22)
        case .sybrSafe: return Self.rgb(0.06, 0.12, 0.16)
        case .blackAndWhite: return Self.rgb(0.09, 0.09, 0.09)
        }
    }
    var gelBottom: NSColor {
        switch self {
        case .ethidiumUV: return Self.rgb(0.11, 0.05, 0.13)
        case .sybrSafe: return Self.rgb(0.03, 0.07, 0.09)
        case .blackAndWhite: return Self.rgb(0.04, 0.04, 0.04)
        }
    }
    /// The middle of a band, and the glow around it.
    var core: NSColor {
        switch self {
        case .ethidiumUV: return Self.rgb(1.0, 0.90, 0.82)
        case .sybrSafe: return Self.rgb(0.90, 1.0, 0.80)
        case .blackAndWhite: return .white
        }
    }
    var glow: NSColor {
        switch self {
        case .ethidiumUV: return Self.rgb(1.0, 0.42, 0.22)
        case .sybrSafe: return Self.rgb(0.35, 1.0, 0.45)
        case .blackAndWhite: return Self.rgb(0.85, 0.85, 0.85)
        }
    }
    /// The light box's tubes show through the gel as faint stripes.
    var hasTubes: Bool { self != .blackAndWhite }
    /// A camera's grain: stronger on a black-and-white print.
    var grain: CGFloat { self == .blackAndWhite ? 0.07 : 0.035 }
}

final class GelView: NSView {
    override var isFlipped: Bool { true }

    var stain: GelStain = .ethidiumUV { didSet { needsDisplay = true } }
    /// The comb that cast the wells: how thick they are, and so the bands.
    var comb: GelComb = .standard { didSet { needsDisplay = true } }
    /// How long the gel ran, as a fraction of a full run.
    var runLevel: Double = 1 { didSet { needsDisplay = true } }
    /// Where the loading dyes' fronts are after a full run (bromophenol blue,
    /// then xylene cyanol), as band runs.
    var dyeRuns: [Double] = [] { didSet { needsDisplay = true } }
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

    /// A fixed scatter of points, 0..1, for the grain.
    private static let grainPoints: [CGPoint] = {
        var seed: UInt32 = 12345
        func next() -> CGFloat {
            seed = seed &* 1_664_525 &+ 1_013_904_223
            return CGFloat(seed >> 8) / CGFloat(1 << 24)
        }
        return (0..<700).map { _ in CGPoint(x: next(), y: next()) }
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

    /// How far lane `lane` has got, eased and scaled by the run slider.
    private func reach(_ lane: Int) -> CGFloat {
        let t = lane < progress.count ? progress[lane] : 1
        return CGFloat(1 - pow(1 - t, 3)) * CGFloat(runLevel)
    }

    // MARK: Geometry

    private struct Geometry {
        var slab: NSRect
        var laneW: CGFloat
        var gap: CGFloat
        /// How much bigger than the smallest lanes these are, for text and bands.
        var scale: CGFloat
        var labelW: CGFloat
        var runTop: CGFloat
        var runBottom: CGFloat
        func laneX(_ i: Int) -> CGFloat { slab.minX + gap + CGFloat(i) * (laneW + gap) }
        /// Where a band of `run` is when its lane has `reach`ed this far.
        func y(_ run: Double, _ reach: CGFloat) -> CGFloat { runTop + CGFloat(run) * reach * (runBottom - runTop) }
    }

    private var geometry: Geometry {
        // Below the floating tabs at the top, above the controls at the bottom.
        let area = NSRect(x: 24, y: 54, width: max(100, bounds.width - 48),
                          height: max(120, bounds.height - 54 - bottomInset))
        // Lanes grow to fill the pane (a few lanes make a wide gel, not a
        // narrow strip), with the gaps and the size labels growing with them.
        let n = CGFloat(max(1, lanes.count))
        let unit = (area.width - 60) / (n * 1.25 + 0.25)
        let laneW = min(120, max(26, unit))
        let scale = max(1, laneW / 64)
        let gap = min(28, max(10, laneW * 0.25))
        // The ladder's sizes sit left of the slab; the slab is centred in the
        // pane, and only moves over when they wouldn't fit beside it.
        let labelW = lanes.contains(where: { $0.isLadder }) ? 60 * min(1.4, scale) : 0
        let slabW = n * laneW + (n + 1) * gap
        let slabX = area.minX + max(labelW, (area.width - slabW) / 2)
        let slab = NSRect(x: slabX, y: area.minY + 24 * scale, width: slabW, height: area.maxY - area.minY - 24 * scale)
        return Geometry(slab: slab, laneW: laneW, gap: gap, scale: scale, labelW: labelW,
                        runTop: slab.minY + 34 * scale, runBottom: slab.maxY - 22)
    }

    /// Thicker for more DNA; wider and softer the further it ran, as bands diffuse.
    private func bandHeight(_ b: GelBandModel, at position: CGFloat, scale: CGFloat = 1) -> CGFloat {
        ((2.5 + 4.5 * CGFloat(b.intensity)) * comb.plug + 2.5 * max(0, position)) * min(1.5, scale)
    }

    /// A band past the bottom has run off the gel; one near the edge fades out.
    private func onGel(_ position: CGFloat) -> CGFloat { max(0, min(1, (1.03 - position) / 0.05)) }

    /// Lanes are never loaded quite evenly: a fixed, slight difference per lane.
    private func loading(_ lane: Int) -> CGFloat {
        let wobble: [CGFloat] = [1, 0.94, 1.03, 0.97, 1.05, 0.95, 1.01, 0.96, 1.04, 0.98, 1.02]
        return wobble[lane % wobble.count]
    }

    // MARK: Drawing

    override func draw(_ dirtyRect: NSRect) {
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }
        // The imager's dark box, a little lighter in the middle.
        let backdrop = NSGradient(colors: [NSColor(white: 0.13, alpha: 1), NSColor(white: 0.03, alpha: 1)])
        backdrop?.draw(in: bounds, relativeCenterPosition: NSPoint(x: 0, y: 0.1))

        let g = geometry
        drawSlab(g)

        // A well for each lane loaded (the imager crops to what was loaded).
        let wellH = (5 + 3 * CGFloat(comb.thickness)) * min(1.5, g.scale)
        for i in 0..<lanes.count {
            let x = g.laneX(i)
            drawWell(NSRect(x: x + 2, y: g.slab.minY + 14 * g.scale, width: g.laneW - 4, height: wellH))
        }
        let nameFont = NSFont.systemFont(ofSize: min(16, 11 * g.scale), weight: .semibold)
        for (i, lane) in lanes.enumerated() {
            let x = g.laneX(i)
            // The lane being edited, and the one under the pointer.
            let lit: CGFloat = i == selectedLane ? 0.07 : (hover?.lane == i ? 0.035 : 0)
            if lit > 0 {
                NSColor.white.withAlphaComponent(lit).setFill()
                NSBezierPath(roundedRect: NSRect(x: x - 4, y: g.slab.minY + 6, width: g.laneW + 8,
                                                 height: g.slab.height - 12), xRadius: 7, yRadius: 7).fill()
            }
            let nameColor = i == selectedLane ? NSColor.white : NSColor.white.withAlphaComponent(0.6)
            drawText(lane.name, font: nameFont, color: nameColor,
                     centeredAt: NSPoint(x: x + g.laneW / 2, y: g.slab.minY - 13 * g.scale))

            let far = reach(i)
            let fadeIn = CGFloat(min(1, (i < progress.count ? progress[i] : 1) * 4))
            // The loading dyes, as faint shadows in the gel.
            for dye in dyeRuns {
                let p = CGFloat(dye) * far
                let y = g.y(dye, far)
                drawDye(NSRect(x: x + 4, y: y - 3, width: g.laneW - 8, height: 6), alpha: 0.22 * onGel(p) * fadeIn)
            }
            for (b, band) in lane.bands.enumerated() {
                let p = CGFloat(band.run) * far
                let visible = onGel(p)
                guard visible > 0 else { continue }
                let h = bandHeight(band, at: p, scale: g.scale)
                let y = g.y(band.run, far)
                let rect = NSRect(x: x + 3, y: y - h / 2, width: g.laneW - 6, height: h)
                let hovered = hover?.lane == i && hover?.band == b
                drawBand(rect, intensity: CGFloat(band.intensity) * fadeIn * visible * loading(i),
                         hovered: hovered, in: ctx)
            }
        }

        // The first ladder's sizes on the left, the reference bands bolder.
        if let li = lanes.firstIndex(where: { $0.isLadder }) {
            let ladder = lanes[li]
            let far = reach(li)
            var lastY = -CGFloat.infinity
            for band in ladder.bands.sorted(by: { $0.run < $1.run }) {
                let p = CGFloat(band.run) * far
                let y = g.y(band.run, far)
                guard onGel(p) > 0.5, y - lastY >= 11 * min(1.4, g.scale) else { continue }
                lastY = y
                let font = NSFont.monospacedDigitSystemFont(ofSize: min(14, 10 * g.scale),
                                                            weight: band.reference ? .semibold : .regular)
                let full = far / CGFloat(max(0.01, runLevel))
                let color = NSColor.white.withAlphaComponent((band.reference ? 0.8 : 0.5) * full)
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

    /// The slab on the light box: a cut edge that catches the light, a slight
    /// thickness below it, the tubes faintly through it, free stain hazing the
    /// top, and the camera's grain.
    private func drawSlab(_ g: Geometry) {
        let slab = g.slab
        let path = NSBezierPath(roundedRect: slab, xRadius: 10, yRadius: 10)
        NSGraphicsContext.saveGraphicsState()
        let shadow = NSShadow()
        shadow.shadowColor = NSColor.black.withAlphaComponent(0.7)
        shadow.shadowBlurRadius = 28
        shadow.shadowOffset = NSSize(width: 0, height: -8)
        shadow.set()
        stain.gelBottom.setFill()
        path.fill()
        NSGraphicsContext.restoreGraphicsState()
        // Its thickness: a darker lip along the bottom edge.
        NSColor.black.withAlphaComponent(0.35).setFill()
        NSBezierPath(roundedRect: NSRect(x: slab.minX + 2, y: slab.maxY - 4, width: slab.width - 4, height: 5),
                     xRadius: 3, yRadius: 3).fill()
        NSGradient(starting: stain.gelTop, ending: stain.gelBottom)?.draw(in: path, angle: 90)

        NSGraphicsContext.saveGraphicsState()
        path.addClip()
        if stain.hasTubes {
            // The light box's tubes, running across under the gel.
            let tube = NSGradient(colors: [stain.glow.withAlphaComponent(0), stain.glow.withAlphaComponent(0.06),
                                           stain.glow.withAlphaComponent(0)])
            var y = slab.minY + 18
            while y < slab.maxY {
                tube?.draw(in: NSRect(x: slab.minX, y: y, width: slab.width, height: 26), angle: 90)
                y += 56
            }
        }
        // Stain that hasn't bound anything glows faintly near the wells.
        NSGradient(starting: stain.glow.withAlphaComponent(0.08), ending: stain.glow.withAlphaComponent(0))?
            .draw(in: NSRect(x: slab.minX, y: slab.minY, width: slab.width, height: slab.height * 0.35), angle: 90)
        NSColor.white.withAlphaComponent(stain.grain).setFill()
        for p in Self.grainPoints {
            NSRect(x: slab.minX + p.x * slab.width, y: slab.minY + p.y * slab.height, width: 1, height: 1).fill()
        }
        NSGraphicsContext.restoreGraphicsState()

        // The cut edge, catching the light along the top more than the sides.
        let rim = NSBezierPath(roundedRect: slab.insetBy(dx: 0.5, dy: 0.5), xRadius: 10, yRadius: 10)
        rim.lineWidth = 1
        NSColor.white.withAlphaComponent(0.09).setStroke()
        rim.stroke()
        NSGraphicsContext.saveGraphicsState()
        NSRect(x: slab.minX, y: slab.minY, width: slab.width, height: 12).clip()
        NSColor.white.withAlphaComponent(0.12).setStroke()
        rim.stroke()
        NSGraphicsContext.restoreGraphicsState()
    }

    /// A well cut by the comb: dark inside, its far wall shaded, a lit lip below.
    private func drawWell(_ well: NSRect) {
        NSColor.black.withAlphaComponent(0.75).setFill()
        NSBezierPath(roundedRect: well, xRadius: 1.5, yRadius: 1.5).fill()
        NSGradient(starting: NSColor.black.withAlphaComponent(0.5), ending: NSColor.black.withAlphaComponent(0))?
            .draw(in: NSRect(x: well.minX, y: well.minY, width: well.width, height: well.height / 2), angle: 90)
        NSColor.white.withAlphaComponent(0.16).setFill()
        NSRect(x: well.minX + 1, y: well.maxY, width: well.width - 2, height: 1).fill()
        NSColor.white.withAlphaComponent(0.05).setFill()
        NSRect(x: well.minX, y: well.minY - 1, width: well.width, height: 1).fill()
    }

    /// A loading dye's front: it quenches the light a little, so it reads as a soft shadow.
    private func drawDye(_ rect: NSRect, alpha: CGFloat) {
        guard alpha > 0.01 else { return }
        NSGraphicsContext.saveGraphicsState()
        let shadow = NSShadow()
        shadow.shadowColor = NSColor.black.withAlphaComponent(alpha)
        shadow.shadowBlurRadius = 6
        shadow.shadowOffset = .zero
        shadow.set()
        NSColor.black.withAlphaComponent(alpha).setFill()
        NSBezierPath(roundedRect: rect, xRadius: 3, yRadius: 3).fill()
        NSGraphicsContext.restoreGraphicsState()
    }

    /// A band: brightest along its middle, fading at the lane's edges and
    /// smiling slightly, with a glow in the stain's colour that grows with the
    /// amount of DNA.
    private func drawBand(_ rect: NSRect, intensity: CGFloat, hovered: Bool, in ctx: CGContext) {
        guard intensity > 0.01 else { return }
        let a = min(1, intensity * (hovered ? 1.25 : 1))
        let smile = min(1.5, rect.width * 0.03)
        let path = CGMutablePath()
        let r = rect.height / 2
        path.move(to: CGPoint(x: rect.minX + r, y: rect.minY))
        path.addQuadCurve(to: CGPoint(x: rect.maxX - r, y: rect.minY), control: CGPoint(x: rect.midX, y: rect.minY + 2 * smile))
        path.addArc(center: CGPoint(x: rect.maxX - r, y: rect.midY), radius: r, startAngle: -.pi / 2, endAngle: .pi / 2,
                    clockwise: false)
        path.addQuadCurve(to: CGPoint(x: rect.minX + r, y: rect.maxY), control: CGPoint(x: rect.midX, y: rect.maxY + 2 * smile))
        path.addArc(center: CGPoint(x: rect.minX + r, y: rect.midY), radius: r, startAngle: .pi / 2, endAngle: 3 * .pi / 2,
                    clockwise: false)
        path.closeSubpath()

        ctx.saveGState()
        ctx.setShadow(offset: .zero, blur: 4 + 12 * a, color: stain.glow.withAlphaComponent(0.9 * a).cgColor)
        ctx.beginTransparencyLayer(auxiliaryInfo: nil)
        ctx.addPath(path)
        ctx.clip()
        let c = stain.core
        let colors = [c.withAlphaComponent(0.4 * a), c.withAlphaComponent(a), c.withAlphaComponent(a),
                      c.withAlphaComponent(0.4 * a)].map { $0.cgColor } as CFArray
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
        let y = g.y(b.run, reach(lane))
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
        let far = reach(lane)
        var best: (Int, CGFloat)?
        for (b, band) in lanes[lane].bands.enumerated() {
            let position = CGFloat(band.run) * far
            guard onGel(position) > 0 else { continue }
            let d = abs(p.y - g.y(band.run, far))
            if d <= max(5, bandHeight(band, at: position, scale: g.scale) / 2 + 3), d < (best?.1 ?? .infinity) {
                best = (b, d)
            }
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

    /// The gel as a picture for a notebook: cropped to the gel and its
    /// labels, without the pointer's bubble or the selected lane's light,
    /// with what each lane holds listed underneath.
    func snapshot(legend: [(lane: String, what: String)], caption: String) -> NSImage? {
        let keepHover = hover, keepSelected = selectedLane
        hover = nil
        selectedLane = -1
        finishRunning()
        defer { hover = keepHover; selectedLane = keepSelected; needsDisplay = true }
        let g = geometry
        let crop = NSRect(x: g.slab.minX - g.labelW - 8, y: g.slab.minY - 30 * g.scale,
                          width: g.slab.width + g.labelW + 24, height: g.slab.height + 30 * g.scale + 16)
            .intersection(bounds)
        guard !crop.isEmpty, let rep = bitmapImageRepForCachingDisplay(in: crop) else { return nil }
        cacheDisplay(in: crop, to: rep)

        let laneFont = NSFont.systemFont(ofSize: 12, weight: .bold)
        let whatFont = NSFont.systemFont(ofSize: 12)
        let captionFont = NSFont.systemFont(ofSize: 11)
        let lineH: CGFloat = 18
        let legendH = 14 + CGFloat(legend.count) * lineH + 6 + lineH + 10
        let size = NSSize(width: crop.width, height: crop.height + legendH)
        return NSImage(size: size, flipped: true) { _ in
            NSColor(white: 0.05, alpha: 1).setFill()
            NSRect(origin: .zero, size: size).fill()
            rep.draw(in: NSRect(origin: .zero, size: crop.size), from: .zero, operation: .copy, fraction: 1,
                     respectFlipped: true, hints: nil)
            var y = crop.height + 14
            for row in legend {
                NSAttributedString(string: row.lane, attributes: [
                    .font: laneFont, .foregroundColor: NSColor.white]).draw(at: NSPoint(x: 16, y: y))
                NSAttributedString(string: row.what, attributes: [
                    .font: whatFont, .foregroundColor: NSColor.white.withAlphaComponent(0.8)])
                    .draw(at: NSPoint(x: 40, y: y))
                y += lineH
            }
            NSAttributedString(string: caption, attributes: [
                .font: captionFont, .foregroundColor: NSColor.white.withAlphaComponent(0.55)])
                .draw(at: NSPoint(x: 16, y: y + 6))
            return true
        }
    }
}
