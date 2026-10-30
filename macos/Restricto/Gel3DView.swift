// SPDX-License-Identifier: GPL-3.0-or-later
// EXPERIMENTAL: the gel as it looks on a blue-light box, in 3D (RealityKit,
// on the GPU). The agarose glows pink all through with the stain; its cut
// edges carry the light and shine; the bands sit inside the slab, softer and
// brighter than the gel around them, so they shift against the top surface as
// the gel turns. The box is deep blue, speckled, darker toward its edges.
// Drag to orbit, scroll to move closer. View only: the sidebar and the
// Imager tab still do the editing and hovering.
import AppKit
import RealityKit

/// An ARView that orbits on drag and zooms on scroll instead of its defaults.
private final class OrbitView: ARView {
    var onDrag: ((CGFloat, CGFloat) -> Void)?
    var onScroll: ((CGFloat) -> Void)?
    override func mouseDragged(with event: NSEvent) { onDrag?(event.deltaX, event.deltaY) }
    override func scrollWheel(with event: NSEvent) { onScroll?(event.scrollingDeltaY) }
}

final class Gel3DView: NSView {
    private let arView = OrbitView(frame: .zero)
    private let world = AnchorEntity(world: .zero)
    private let gel = Entity()
    private let camera = PerspectiveCamera()
    private var yaw: Float = 0
    private var pitch: Float = 0.82
    private var zoom: Float = 1
    /// How far the camera stands back to take in the whole gel.
    private var fit: Float = 0.2
    private var lanes: [GelLaneModel] = []
    private var runLevel: Double = 1
    private var dyeRuns: [Double] = []

    override init(frame: NSRect) {
        super.init(frame: frame)
        arView.translatesAutoresizingMaskIntoConstraints = false
        addSubview(arView)
        NSLayoutConstraint.activate([
            arView.topAnchor.constraint(equalTo: topAnchor),
            arView.bottomAnchor.constraint(equalTo: bottomAnchor),
            arView.leadingAnchor.constraint(equalTo: leadingAnchor),
            arView.trailingAnchor.constraint(equalTo: trailingAnchor),
        ])
        arView.environment.background = .color(NSColor(srgbRed: 0.01, green: 0.01, blue: 0.10, alpha: 1))
        world.addChild(gel)
        world.addChild(camera)
        camera.camera.fieldOfViewInDegrees = 35
        arView.scene.addAnchor(world)
        placeCamera()
        arView.onDrag = { [weak self] dx, dy in
            guard let self = self else { return }
            self.yaw -= Float(dx) * 0.008
            self.pitch = min(1.45, max(0.25, self.pitch + Float(dy) * 0.008))
            self.placeCamera()
        }
        arView.onScroll = { [weak self] dy in
            guard let self = self else { return }
            self.zoom = min(2.5, max(0.35, self.zoom * (1 - Float(dy) * 0.01)))
            self.placeCamera()
        }
        // No post-processing pass: setting ARView's postProcess callback traps
        // on macOS 27 (RealityKit 453), so the glow is painted into the gel.
        setAccessibilityElement(true)
        setAccessibilityRole(.image)
        setAccessibilityLabel("Gel in 3D")
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    private func placeCamera() {
        let d = fit * zoom
        let position = SIMD3<Float>(d * sin(yaw) * cos(pitch), d * sin(pitch), d * cos(yaw) * cos(pitch))
        camera.look(at: [0, 0, 0.004], from: position, relativeTo: nil)
    }

    // MARK: The scene

    /// Shows `lanes`, run `runLevel` of the way; `dyeRuns` are the loading
    /// dyes' fronts after a full run.
    func show(_ lanes: [GelLaneModel], runLevel: Double, dyeRuns: [Double] = []) {
        self.lanes = lanes
        self.runLevel = runLevel
        self.dyeRuns = dyeRuns
        rebuild()
    }

    private static func color(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ a: CGFloat = 1) -> NSColor {
        NSColor(srgbRed: r, green: g, blue: b, alpha: a)
    }

    private func unlit(_ color: NSColor, opacity: Float) -> UnlitMaterial {
        var m = UnlitMaterial(color: color)
        m.blending = .transparent(opacity: .init(floatLiteral: opacity))
        return m
    }

    /// The see-through parts are drawn back to front, whatever RealityKit's
    /// own sorting would pick.
    private let order = ModelSortGroup(depthPass: nil)

    private func add(_ entity: ModelEntity, at position: SIMD3<Float>, order: Int32) {
        entity.position = position
        entity.components.set(ModelSortGroupComponent(group: self.order, order: order))
        gel.addChild(entity)
    }

    /// A flat picture lying face up, its top edge toward the wells (−z).
    private func lying(_ material: UnlitMaterial, width: Float, depth: Float) -> ModelEntity {
        let plane = ModelEntity(mesh: .generatePlane(width: width, height: depth), materials: [material])
        plane.orientation = simd_quatf(angle: -.pi / 2, axis: [1, 0, 0])
        return plane
    }

    private func rebuild() {
        gel.children.removeAll()
        let n = Float(max(1, lanes.count))
        let laneW: Float = 0.010, gap: Float = 0.004
        let width = n * laneW + (n + 1) * gap + 0.012
        let depth: Float = 0.10, thick: Float = 0.007
        fit = max(width * 1.25, depth * 1.7)
        placeCamera()

        let laneX = { (i: Int) -> Float in -width / 2 + 0.006 + gap + Float(i) * (laneW + gap) + laneW / 2 }
        let wellZ = -depth / 2 + 0.010
        let runTop = wellZ + 0.004, runBottom = depth / 2 - 0.008
        let runZ = { (p: Double) -> Float in runTop + Float(p) * (runBottom - runTop) }
        // Pixels per metre in the gel's pictures: sharp, but bounded.
        let ppm = min(20000, 2400 / max(width, depth))
        let px = { (x: Float) -> CGFloat in CGFloat((x + width / 2) * ppm) }
        let py = { (z: Float) -> CGFloat in CGFloat((z + depth / 2) * ppm) }
        let mm = CGFloat(ppm) / 1000

        // The box: deep blue, speckled.
        add(lying(Self.boxMaterial, width: width + 0.5, depth: depth + 0.5), at: [0, -0.0002, 0], order: 0)

        // Pink light spilling from the slab onto the box around it.
        let spillPPM = ppm / 4
        if let spill = Self.layer(width: width + 0.04, depth: depth + 0.04, ppm: spillPPM, draw: { ctx in
            let m = CGFloat(spillPPM) / 1000
            let rect = CGRect(x: 20 * m, y: 20 * m, width: CGFloat(width * spillPPM), height: CGFloat(depth * spillPPM))
            Self.softFill(ctx, CGPath(roundedRect: rect, cornerWidth: 2 * m, cornerHeight: 2 * m, transform: nil),
                          color: Self.color(0.85, 0.30, 0.95, 0.45), blur: 9 * m)
        }) {
            add(lying(spill, width: width + 0.04, depth: depth + 0.04), at: [0, 0.0001, 0], order: 1)
        }

        // The slab's underside, so the box shows pinker through it.
        add(lying(unlit(Self.color(0.80, 0.30, 0.95), opacity: 0.30), width: width, depth: depth), at: [0, 0.0003, 0], order: 2)

        // Its far walls, seen through the gel.
        let wall: Float = 0.0008
        let back = ModelEntity(mesh: .generateBox(size: [width, thick, wall]),
                               materials: [unlit(Self.color(1.0, 0.70, 0.95), opacity: 0.55)])
        add(back, at: [0, thick / 2, -depth / 2 + wall / 2], order: 3)
        for side in [-1, 1] as [Float] {
            let s = ModelEntity(mesh: .generateBox(size: [wall, thick, depth]),
                                materials: [unlit(Self.color(1.0, 0.68, 0.95), opacity: 0.6)])
            add(s, at: [side * (width / 2 - wall / 2), thick / 2, 0], order: 3)
        }

        // Inside the gel: the bands, the lanes' faint smears.
        if let inside = Self.layer(width: width, depth: depth, ppm: ppm, draw: { ctx in
            for (i, lane) in self.lanes.enumerated() {
                let cx = px(laneX(i)), half = CGFloat(laneW) * CGFloat(ppm) * 0.46
                var lowest = -1.0
                for band in lane.bands {
                    let p = band.run * self.runLevel
                    guard p <= 1.03 else { continue }
                    lowest = max(lowest, p)
                    let k = CGFloat(band.intensity)
                    let y = py(runZ(p))
                    let th = (0.55 + 1.1 * k) * mm * (1 + 0.35 * CGFloat(p))
                    let smile = 0.25 * mm * (1 + CGFloat(p))
                    let path = CGMutablePath()
                    path.move(to: CGPoint(x: cx - half, y: y - th / 2))
                    path.addQuadCurve(to: CGPoint(x: cx + half, y: y - th / 2), control: CGPoint(x: cx, y: y - th / 2 + 2 * smile))
                    path.addLine(to: CGPoint(x: cx + half, y: y + th / 2))
                    path.addQuadCurve(to: CGPoint(x: cx - half, y: y + th / 2), control: CGPoint(x: cx, y: y + th / 2 + 2 * smile))
                    path.closeSubpath()
                    // A wide soft glow, then the band itself, its edges a little blurred.
                    Self.softFill(ctx, path, color: Self.color(1.0, 0.38, 0.62, 0.35 + 0.5 * k), blur: (1.2 + 2.2 * k) * mm)
                    Self.softFill(ctx, path, color: Self.color(1.0, 0.62 + 0.3 * k, 0.72 + 0.22 * k, 0.45 + 0.55 * k),
                                  blur: 0.3 * mm)
                }
                if !lane.isLadder, lowest > 0 {
                    let top = py(wellZ) + 1.5 * mm, bottom = py(runZ(lowest))
                    let smear = CGRect(x: cx - half * 0.85, y: top, width: half * 1.7, height: max(0, bottom - top))
                    Self.softFill(ctx, CGPath(rect: smear, transform: nil), color: Self.color(1.0, 0.45, 0.80, 0.16),
                                  blur: 0.8 * mm)
                }
            }
        }) {
            add(lying(inside, width: width, depth: depth), at: [0, thick * 0.45, 0], order: 4)
        }

        // The top: agarose glowing pink with the stain, a little brighter in
        // the middle, the dyes' shadows across the lanes, the wells cut in.
        if let top = Self.layer(width: width, depth: depth, ppm: ppm, draw: { ctx in
            let all = CGRect(x: 0, y: 0, width: CGFloat(width * ppm), height: CGFloat(depth * ppm))
            ctx.setFillColor(Self.color(0.88, 0.42, 0.98, 0.40).cgColor)
            ctx.fill(all)
            let space = CGColorSpace(name: CGColorSpace.sRGB)!
            if let glow = CGGradient(colorsSpace: space, colors: [Self.color(1.0, 0.62, 1.0, 0.22).cgColor,
                                                                  Self.color(1.0, 0.62, 1.0, 0).cgColor] as CFArray,
                                     locations: [0, 1]) {
                ctx.drawRadialGradient(glow, startCenter: CGPoint(x: all.midX, y: all.midY * 0.8), startRadius: 0,
                                       endCenter: CGPoint(x: all.midX, y: all.midY * 0.8), endRadius: max(all.width, all.height) * 0.6,
                                       options: [])
            }
            for (i, lane) in self.lanes.enumerated() {
                let cx = px(laneX(i)), half = CGFloat(laneW) * CGFloat(ppm) * 0.46
                if !lane.isLadder {
                    for dye in self.dyeRuns {
                        let p = dye * self.runLevel
                        guard p <= 1.02 else { continue }
                        let r = CGRect(x: cx - half, y: py(runZ(p)) - 1.2 * mm, width: 2 * half, height: 2.4 * mm)
                        Self.softFill(ctx, CGPath(ellipseIn: r, transform: nil), color: Self.color(0.22, 0.06, 0.45, 0.35),
                                      blur: 1.2 * mm)
                    }
                }
                let well = CGRect(x: cx - half * 0.95, y: py(wellZ) - 0.9 * mm, width: 1.9 * half, height: 1.8 * mm)
                let slot = CGPath(roundedRect: well, cornerWidth: 0.6 * mm, cornerHeight: 0.6 * mm, transform: nil)
                Self.softFill(ctx, slot, color: Self.color(1.0, 0.70, 1.0, 0.55), blur: 0.4 * mm)
                ctx.addPath(slot)
                ctx.setStrokeColor(Self.color(1.0, 0.90, 1.0, 0.85).cgColor)
                ctx.setLineWidth(0.25 * mm)
                ctx.strokePath()
            }
            // Agarose isn't perfectly clear: a fine grain.
            var seed: UInt32 = 7
            ctx.setFillColor(Self.color(1, 1, 1, 0.10).cgColor)
            for _ in 0..<Int(all.width * all.height / 900) {
                seed = seed &* 1_664_525 &+ 1_013_904_223
                let x = CGFloat(seed % 10_000) / 10_000 * all.width
                seed = seed &* 1_664_525 &+ 1_013_904_223
                let y = CGFloat(seed % 10_000) / 10_000 * all.height
                ctx.fill(CGRect(x: x, y: y, width: 1.5, height: 1.5))
            }
        }) {
            add(lying(top, width: width, depth: depth), at: [0, thick, 0], order: 5)
        }

        // The near wall, and the cut edges, which carry the light and shine.
        let front = ModelEntity(mesh: .generateBox(size: [width, thick, wall]),
                                materials: [unlit(Self.color(1.0, 0.78, 0.97), opacity: 0.82)])
        add(front, at: [0, thick / 2, depth / 2 - wall / 2], order: 6)
        let rim: Float = 0.0007
        let edge = unlit(Self.color(1.0, 0.92, 1.0), opacity: 0.9)
        for z in [-depth / 2, depth / 2] {
            add(ModelEntity(mesh: .generateBox(size: [width, rim, rim]), materials: [edge]), at: [0, thick, z], order: 7)
        }
        for x in [-width / 2, width / 2] {
            add(ModelEntity(mesh: .generateBox(size: [rim, rim, depth]), materials: [edge]), at: [x, thick, 0], order: 7)
        }
        add(ModelEntity(mesh: .generateBox(size: [width, rim, rim]), materials: [edge]), at: [0, rim / 2, depth / 2], order: 7)
    }

    // MARK: Pictures

    /// Fills `path` blurred by `blur`: only the shadow of a copy far off to
    /// the side lands in the picture.
    private static func softFill(_ ctx: CGContext, _ path: CGPath, color: NSColor, blur: CGFloat) {
        let away: CGFloat = 100_000
        ctx.saveGState()
        ctx.setShadow(offset: CGSize(width: away, height: 0), blur: blur, color: color.cgColor)
        ctx.translateBy(x: -away, y: 0)
        ctx.addPath(path)
        ctx.setFillColor(NSColor.black.cgColor)
        ctx.fillPath()
        ctx.restoreGState()
    }

    /// A see-through picture `width` × `depth` metres, drawn by `draw` with y
    /// down from the top (the wells' side). Its colour and its opacity go to
    /// the GPU as two textures, so its soft edges stay soft.
    private static func layer(width: Float, depth: Float, ppm: Float, draw: (CGContext) -> Void) -> UnlitMaterial? {
        let w = max(8, Int(width * ppm)), h = max(8, Int(depth * ppm))
        guard let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                  space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return nil }
        ctx.translateBy(x: 0, y: CGFloat(h))
        ctx.scaleBy(x: 1, y: -1)
        draw(ctx)
        guard let data = ctx.data else { return nil }
        let pixels = data.bindMemory(to: UInt8.self, capacity: w * h * 4)
        var alpha = [UInt8](repeating: 0, count: w * h)
        for i in 0..<(w * h) {
            let o = i * 4, a = Int(pixels[o + 3])
            alpha[i] = UInt8(a)
            if a > 0 && a < 255 {
                for c in 0..<3 { pixels[o + c] = UInt8(min(255, Int(pixels[o + c]) * 255 / a)) }
            }
            pixels[o + 3] = 255
        }
        guard let colorImage = ctx.makeImage(),
              let provider = CGDataProvider(data: Data(alpha) as CFData),
              let alphaImage = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 8, bytesPerRow: w,
                                       space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGBitmapInfo(rawValue: 0),
                                       provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent),
              let colorTexture = try? TextureResource.generate(from: colorImage, options: .init(semantic: .color)),
              let alphaTexture = try? TextureResource.generate(from: alphaImage, options: .init(semantic: .raw))
        else { return nil }
        var m = UnlitMaterial()
        m.color = .init(tint: .white, texture: .init(colorTexture))
        m.blending = .transparent(opacity: .init(scale: 1, texture: .init(alphaTexture)))
        return m
    }

    /// The blue-light box: deep blue, brighter under the middle, darker toward
    /// its edges, with the specks every box collects.
    private static let boxMaterial: UnlitMaterial = {
        var material = UnlitMaterial(color: Gel3DView.color(0.08, 0.08, 0.62))
        let size = 1024
        guard let ctx = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8, bytesPerRow: 0,
                                  space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue),
              let light = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB),
                                     colors: [Gel3DView.color(0.14, 0.13, 0.92).cgColor, Gel3DView.color(0.07, 0.07, 0.55).cgColor,
                                              Gel3DView.color(0.02, 0.02, 0.22).cgColor] as CFArray,
                                     locations: [0, 0.45, 1]) else { return material }
        let mid = CGPoint(x: size / 2, y: size / 2)
        ctx.drawRadialGradient(light, startCenter: mid, startRadius: 0, endCenter: mid, endRadius: CGFloat(size) * 0.62,
                               options: [.drawsAfterEndLocation])
        var seed: UInt32 = 42
        func next() -> CGFloat {
            seed = seed &* 1_664_525 &+ 1_013_904_223
            return CGFloat(seed >> 8) / CGFloat(1 << 24)
        }
        for i in 0..<1400 {
            let r = 0.6 + 1.8 * next()
            let dark = i % 6 != 0
            ctx.setFillColor(dark ? Gel3DView.color(0.01, 0.01, 0.15, 0.55).cgColor : Gel3DView.color(0.45, 0.45, 1.0, 0.35).cgColor)
            ctx.fillEllipse(in: CGRect(x: next() * CGFloat(size), y: next() * CGFloat(size), width: r, height: r))
        }
        guard let image = ctx.makeImage(),
              let texture = try? TextureResource.generate(from: image, options: .init(semantic: .color)) else {
            return material
        }
        material.color = .init(tint: .white, texture: .init(texture))
        return material
    }()
}
