// SPDX-License-Identifier: GPL-3.0-or-later
// EXPERIMENTAL: the gel as a lit 3D scene (RealityKit, on the GPU): a clear
// slab lying on a violet UV box whose bars glow through it, comb-cut wells,
// smeared lanes and glowing bands inside the agarose, with a bloom pass in
// Core Image on Metal. Drag to orbit, scroll to move closer. View only: the
// sidebar and the other tabs still do the editing and hovering.
import AppKit
import CoreImage
import CoreImage.CIFilterBuiltins
import Metal
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
    private var pitch: Float = 1.05
    private var distance: Float = 0.30
    private var ciContext: CIContext?
    private var lanes: [GelLaneModel] = []
    private var runLevel: Double = 1

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
        arView.environment.background = .color(NSColor(srgbRed: 0.03, green: 0.01, blue: 0.08, alpha: 1))
        world.addChild(gel)
        // Light from above and in front, for the slab's sheen.
        let sun = DirectionalLight()
        sun.light.intensity = 2500
        sun.look(at: .zero, from: [0.1, 0.4, 0.3], relativeTo: nil)
        world.addChild(sun)
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
            self.distance = min(0.7, max(0.12, self.distance * (1 - Float(dy) * 0.01)))
            self.placeCamera()
        }
        // The glow a camera sees around bright bands, done on the GPU.
        arView.renderCallbacks.postProcess = { [weak self] context in self?.bloom(context) }
        setAccessibilityElement(true)
        setAccessibilityRole(.image)
        setAccessibilityLabel("Gel in 3D")
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    private func placeCamera() {
        let position = SIMD3<Float>(distance * sin(yaw) * cos(pitch), distance * sin(pitch),
                                    distance * cos(yaw) * cos(pitch))
        camera.look(at: [0, 0, 0.005], from: position, relativeTo: nil)
    }

    private func bloom(_ context: ARView.PostProcessContext) {
        if ciContext == nil { ciContext = CIContext(mtlDevice: context.device) }
        guard let ci = ciContext, let input = CIImage(mtlTexture: context.sourceColorTexture) else {
            copy(context)
            return
        }
        let filter = CIFilter.bloom()
        filter.inputImage = input
        filter.radius = 16
        filter.intensity = 1.1
        guard let output = filter.outputImage?.cropped(to: input.extent) else {
            copy(context)
            return
        }
        let destination = CIRenderDestination(mtlTexture: context.targetColorTexture,
                                              commandBuffer: context.commandBuffer)
        destination.isFlipped = false
        if (try? ci.startTask(toRender: output, to: destination)) == nil { copy(context) }
    }

    /// Passes the frame through untouched.
    private func copy(_ context: ARView.PostProcessContext) {
        guard let blit = context.commandBuffer.makeBlitCommandEncoder() else { return }
        blit.copy(from: context.sourceColorTexture, to: context.targetColorTexture)
        blit.endEncoding()
    }

    // MARK: The scene

    /// Shows `lanes`, run `runLevel` of the way.
    func show(_ lanes: [GelLaneModel], runLevel: Double) {
        self.lanes = lanes
        self.runLevel = runLevel
        rebuild()
    }

    private static func color(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ a: CGFloat = 1) -> NSColor {
        NSColor(srgbRed: r, green: g, blue: b, alpha: a)
    }

    private func unlit(_ color: NSColor, opacity: Float? = nil) -> UnlitMaterial {
        var m = UnlitMaterial(color: color)
        if let opacity = opacity { m.blending = .transparent(opacity: .init(floatLiteral: opacity)) }
        return m
    }

    private func rebuild() {
        gel.children.removeAll()
        let n = Float(max(1, lanes.count))
        let laneW: Float = 0.010, gap: Float = 0.004
        let width = n * laneW + (n + 1) * gap + 0.01
        let depth: Float = 0.10, thick: Float = 0.007

        // The UV box: violet, its tubes' bars glowing under the gel.
        let box = ModelEntity(mesh: .generatePlane(width: width + 0.16, depth: depth + 0.14),
                              materials: [lightBoxMaterial()])
        gel.addChild(box)

        // The slab: clear, faintly violet, glossy.
        var slab = PhysicallyBasedMaterial()
        slab.baseColor = .init(tint: Self.color(0.55, 0.50, 1.0))
        slab.emissiveColor = .init(color: Self.color(0.25, 0.18, 0.75))
        slab.emissiveIntensity = 0.6
        slab.roughness = .init(floatLiteral: 0.12)
        slab.metallic = .init(floatLiteral: 0)
        slab.clearcoat = .init(floatLiteral: 1)
        slab.blending = .transparent(opacity: .init(floatLiteral: 0.4))
        let slabEntity = ModelEntity(mesh: .generateBox(size: [width, thick, depth], cornerRadius: 0.0015),
                                     materials: [slab])
        slabEntity.position = [0, thick / 2 + 0.0005, 0]
        gel.addChild(slabEntity)

        let top = thick + 0.0005
        let wellZ = -depth / 2 + 0.010
        let runTop = wellZ + 0.008, runBottom = depth / 2 - 0.010
        for (i, lane) in lanes.enumerated() {
            let x = -width / 2 + 0.005 + gap + Float(i) * (laneW + gap) + laneW / 2
            // The well, cut into the top.
            let well = ModelEntity(mesh: .generateBox(size: [laneW * 0.9, thick * 0.55, 0.0022], cornerRadius: 0.0005),
                                   materials: [unlit(Self.color(0.10, 0.05, 0.30), opacity: 0.75)])
            well.position = [x, top - thick * 0.27, wellZ]
            gel.addChild(well)
            // Its name in front of the well, lying on the gel.
            let name = ModelEntity(mesh: .generateText(lane.name, extrusionDepth: 0.0003,
                                                       font: .systemFont(ofSize: 0.005, weight: .semibold)),
                                   materials: [unlit(.white, opacity: 0.85)])
            let bounds = name.visualBounds(relativeTo: nil)
            name.orientation = simd_quatf(angle: -.pi / 2, axis: [1, 0, 0])
            name.position = [x - (bounds.max.x - bounds.min.x) / 2, top + 0.0002, wellZ - 0.004]
            gel.addChild(name)

            let reach = Float(runLevel)
            var lowest: Float = 0
            for band in lane.bands {
                let p = Float(band.run) * reach
                guard p <= 1.03 else { continue }
                lowest = max(lowest, p)
                let z = runTop + p * (runBottom - runTop)
                let k = CGFloat(band.intensity)
                let glow = Self.color(1.0, 0.55 + 0.3 * k, 0.45 + 0.25 * k)
                let entity = ModelEntity(mesh: .generateBox(size: [laneW * 0.88, thick * 0.3, 0.0010 + 0.0018 * Float(k)],
                                                            cornerRadius: 0.0004),
                                         materials: [unlit(glow, opacity: Float(0.35 + 0.65 * k))])
                entity.position = [x, thick * 0.55, z]
                gel.addChild(entity)
            }
            // A loaded lane glows faintly from the well to its last band.
            if !lane.isLadder, lowest > 0 {
                let length = lowest * (runBottom - runTop) + 0.006
                let smear = ModelEntity(mesh: .generateBox(size: [laneW * 0.8, thick * 0.2, length], cornerRadius: 0.0003),
                                        materials: [unlit(Self.color(1.0, 0.35, 0.6), opacity: 0.18)])
                smear.position = [x, thick * 0.5, wellZ + 0.001 + length / 2]
                gel.addChild(smear)
            }
        }
    }

    /// Violet with brighter bars, as the tubes under a UV box.
    private func lightBoxMaterial() -> UnlitMaterial {
        let w = 64, h = 512
        var material = unlit(Self.color(0.20, 0.08, 0.60))
        guard let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0,
                                  space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return material }
        ctx.setFillColor(Self.color(0.16, 0.06, 0.52).cgColor)
        ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
        let colors = [Self.color(0.55, 0.35, 1.0, 0), Self.color(0.70, 0.50, 1.0, 0.9),
                      Self.color(0.55, 0.35, 1.0, 0)].map { $0.cgColor } as CFArray
        if let bar = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB), colors: colors,
                                locations: [0, 0.5, 1]) {
            var y = 20
            while y < h {
                ctx.drawLinearGradient(bar, start: CGPoint(x: 0, y: y), end: CGPoint(x: 0, y: y + 70), options: [])
                y += 128
            }
        }
        guard let image = ctx.makeImage(),
              let texture = try? TextureResource.generate(from: image, options: .init(semantic: .color)) else {
            return material
        }
        material.color = .init(tint: .white, texture: .init(texture))
        return material
    }
}
