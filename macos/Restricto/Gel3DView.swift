// SPDX-License-Identifier: GPL-3.0-or-later
// EXPERIMENTAL: the gel photographed on a blue-light box, by path tracing on
// the GPU (Metal). Nothing here is painted to look like a gel: the scene is
// a block of agarose (a refracting, faintly scattering, stain-filled medium,
// shaped as a cast gel is: a sagging top, a lip at the edges, wells cut in)
// lying on a glowing plate, in spilt buffer, under a dark room with one lamp, and the
// picture is what a camera would see. Light enters the block from the plate,
// the stain fluoresces faintly everywhere and brightly where the DNA is, rays
// that leave at a shallow angle bounce inside the slab until they escape at
// a cut edge (which is why the edges shine), and the wet top catches the
// lamp. Each frame adds a sample per pixel; the picture converges while the
// gel is still and goes grainy while it moves.
// Drag to orbit, scroll to move closer, double-click to go back. View only:
// the sidebar and the Imager tab still do the editing and hovering.
import AppKit
import Metal
import MetalKit

/// The Metal view: orbits on drag, zooms on scroll, resets on double-click.
private final class TracerView: MTKView {
    var onDrag: ((CGFloat, CGFloat) -> Void)?
    var onScroll: ((CGFloat) -> Void)?
    var onReset: (() -> Void)?
    /// Views floating over the gel (the pills): a drag from one of them,
    /// even from its empty glass, doesn't turn the gel.
    var ignoredViews: [NSView] = []
    /// Only a drag that started on the gel turns it.
    private var grabbed = false
    override func mouseDown(with event: NSEvent) {
        let overPill = ignoredViews.contains {
            !$0.isHidden && $0.convert($0.bounds, to: nil).contains(event.locationInWindow)
        }
        grabbed = !overPill
        if grabbed, event.clickCount == 2 { onReset?() }
    }
    override func mouseUp(with event: NSEvent) { grabbed = false }
    override func mouseDragged(with event: NSEvent) {
        if grabbed { onDrag?(event.deltaX, event.deltaY) }
    }
    override func scrollWheel(with event: NSEvent) { onScroll?(event.scrollingDeltaY) }
}

/// How the gel is stained and lit: the plate's light, and the colours the
/// stain gives off, free in the gel and bound to the DNA.
enum Gel3DScene: Int, CaseIterable {
    case pinkOnBlue = 0, greenOnBlue = 1, blackAndWhite = 2

    var title: String {
        switch self {
        case .pinkOnBlue: return "Blue Light · Pink"
        case .greenOnBlue: return "Blue Light · Green"
        case .blackAndWhite: return "Black & White"
        }
    }

    /// The plate's colour (rgb) and whether the picture is monochrome (w).
    var plate: SIMD4<Float> {
        switch self {
        case .pinkOnBlue: return SIMD4<Float>(0.02, 0.0, 0.75, 0)
        case .greenOnBlue: return SIMD4<Float>(0.0, 0.10, 0.70, 0)
        case .blackAndWhite: return SIMD4<Float>(0.06, 0.06, 0.07, 1)
        }
    }
    /// The free stain's glow throughout the gel (rgb) and how strong (w).
    var stain: SIMD4<Float> {
        switch self {
        case .pinkOnBlue: return SIMD4<Float>(1.0, 0.10, 0.50, 1.0)
        case .greenOnBlue: return SIMD4<Float>(0.30, 0.95, 0.40, 0.45)
        case .blackAndWhite: return SIMD4<Float>(0.6, 0.6, 0.6, 0.7)
        }
    }
    /// The DNA's glow (rgb) and how much brighter than the free stain (w).
    var band: SIMD4<Float> {
        switch self {
        case .pinkOnBlue: return SIMD4<Float>(1.0, 0.22, 0.60, 12)
        case .greenOnBlue: return SIMD4<Float>(0.50, 1.0, 0.35, 14)
        case .blackAndWhite: return SIMD4<Float>(1.0, 1.0, 1.0, 16)
        }
    }
    /// The cut edges' shine (rgb) and how strong (w).
    var lip: SIMD4<Float> {
        switch self {
        case .pinkOnBlue: return SIMD4<Float>(0.55, 0.12, 0.9, 0.6)
        case .greenOnBlue: return SIMD4<Float>(0.35, 0.9, 0.55, 0.5)
        case .blackAndWhite: return SIMD4<Float>(0.8, 0.8, 0.8, 0.5)
        }
    }
    /// What the stained gel drinks per metre (rgb: the stain takes the light
    /// that excites it) and how much it scatters (w).
    var sigma: SIMD4<Float> {
        switch self {
        case .pinkOnBlue: return SIMD4<Float>(8, 40, 6, 18)
        case .greenOnBlue: return SIMD4<Float>(10, 8, 34, 18)
        case .blackAndWhite: return SIMD4<Float>(12, 12, 12, 18)
        }
    }
}

/// What the kernels need; laid out as the Metal `Uniforms` struct.
private struct GelUniforms {
    var camPos = SIMD4<Float>(repeating: 0)
    var camF = SIMD4<Float>(repeating: 0)
    var camR = SIMD4<Float>(repeating: 0)
    var camU = SIMD4<Float>(repeating: 0)
    /// Absorption per metre (rgb) and scattering per metre (w).
    var sigma = SIMD4<Float>(repeating: 0)
    /// tan(fov/2), aspect, lens radius, focus distance.
    var lens = SIMD4<Float>(repeating: 0)
    /// Width, depth, thickness, well depth (as a fraction of the thickness).
    var slab = SIMD4<Float>(repeating: 0)
    /// The plate: half width, how far it reaches in front, how far behind, brightness.
    var emitter = SIMD4<Float>(repeating: 0)
    /// Exposure, grain, fluorescence, scattering anisotropy.
    var look = SIMD4<Float>(repeating: 0)
    /// The scene's colours: see Gel3DScene.
    var plate = SIMD4<Float>(repeating: 0)
    var stain = SIMD4<Float>(repeating: 0)
    var band = SIMD4<Float>(repeating: 0)
    var lip = SIMD4<Float>(repeating: 0)
    /// Frame, samples so far, width, height.
    var counts = SIMD4<UInt32>(repeating: 0)
}

final class Gel3DView: NSView, MTKViewDelegate {
    private let metalView = TracerView(frame: .zero, device: nil)
    private let note = NSTextField(wrappingLabelWithString: "")
    private var queue: MTLCommandQueue?
    private var tracePipeline: MTLComputePipelineState?
    private var resolvePipeline: MTLComputePipelineState?
    /// Two, so a frame reads the samples so far from one and writes the sum to the other.
    private var accum: [MTLTexture] = []
    private var bandMap: MTLTexture?
    private var samples: UInt32 = 0
    private var frameIndex: UInt32 = 0
    /// Enough for a clean picture; then the GPU rests until something changes.
    private let maxSamples: UInt32 = 160
    /// Views floating over the gel that a drag may start on without turning it.
    var ignoredViews: [NSView] {
        get { metalView.ignoredViews }
        set { metalView.ignoredViews = newValue }
    }

    private static let homePitch: Float = 0.80
    private var yaw: Float = 0
    private var pitch: Float = homePitch
    private var zoom: Float = 1
    /// How far the camera stands back to take in the whole gel.
    private var fit: Float = 0.2
    private var lanes: [GelLaneModel] = []
    private var runLevel: Double = 1
    private var dyeRuns: [Double] = []
    /// How the gel is stained and lit; changing it starts the picture over.
    var scene: Gel3DScene = .pinkOnBlue {
        didSet { if scene != oldValue { restart() } }
    }
    private var width: Float = 0.1
    private let depth: Float = 0.10
    private let thick: Float = 0.006
    /// Redrawing the stain map is the slow part: at most ~30 times a second.
    private var lastBuild = Date.distantPast
    private var buildPending = false

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layer?.backgroundColor = NSColor(srgbRed: 0.01, green: 0.01, blue: 0.06, alpha: 1).cgColor
        metalView.translatesAutoresizingMaskIntoConstraints = false
        addSubview(metalView)
        NSLayoutConstraint.activate([
            metalView.topAnchor.constraint(equalTo: topAnchor),
            metalView.bottomAnchor.constraint(equalTo: bottomAnchor),
            metalView.leadingAnchor.constraint(equalTo: leadingAnchor),
            metalView.trailingAnchor.constraint(equalTo: trailingAnchor),
        ])
        note.translatesAutoresizingMaskIntoConstraints = false
        note.textColor = NSColor.white.withAlphaComponent(0.7)
        note.alignment = .center
        note.isHidden = true
        addSubview(note)
        NSLayoutConstraint.activate([
            note.centerXAnchor.constraint(equalTo: centerXAnchor),
            note.centerYAnchor.constraint(equalTo: centerYAnchor),
            note.widthAnchor.constraint(lessThanOrEqualTo: widthAnchor, constant: -60),
        ])
        setUpMetal()
        metalView.onDrag = { [weak self] dx, dy in
            guard let self = self else { return }
            self.yaw -= Float(dx) * 0.008
            self.pitch = min(1.45, max(0.22, self.pitch + Float(dy) * 0.008))
            self.restart()
        }
        metalView.onScroll = { [weak self] dy in
            guard let self = self else { return }
            self.zoom = min(2.0, max(0.45, self.zoom * (1 - Float(dy) * 0.004)))
            self.restart()
        }
        metalView.onReset = { [weak self] in
            guard let self = self else { return }
            self.yaw = 0
            self.pitch = Self.homePitch
            self.zoom = 1
            self.restart()
        }
        setAccessibilityElement(true)
        setAccessibilityRole(.image)
        setAccessibilityLabel("Gel in 3D")
        setAccessibilityHelp("Drag to turn the gel, scroll to come closer, double-click to go back")
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is not used") }

    private func setUpMetal() {
        guard let device = MTLCreateSystemDefaultDevice() else {
            fail("The 3D gel needs a Metal GPU.")
            return
        }
        metalView.device = device
        metalView.colorPixelFormat = .bgra8Unorm
        metalView.framebufferOnly = false
        metalView.autoResizeDrawable = false
        metalView.preferredFramesPerSecond = 60
        metalView.isPaused = true
        metalView.enableSetNeedsDisplay = false
        metalView.delegate = self
        do {
            let library = try device.makeLibrary(source: Self.shaders, options: nil)
            guard let trace = library.makeFunction(name: "gelTrace"),
                  let resolve = library.makeFunction(name: "gelResolve") else {
                fail("The 3D gel's shaders are missing.")
                return
            }
            tracePipeline = try device.makeComputePipelineState(function: trace)
            resolvePipeline = try device.makeComputePipelineState(function: resolve)
            queue = device.makeCommandQueue()
        } catch {
            fail("The 3D gel's shaders didn't compile: \(error.localizedDescription)")
        }
    }

    /// Why there's no picture, if there isn't one (the launch check reads it).
    private(set) var problem: String?

    private func fail(_ message: String) {
        problem = message
        note.stringValue = message
        note.isHidden = false
        metalView.isHidden = true
    }

    // MARK: Rendering

    /// Shows `lanes`, run `runLevel` of the way; `dyeRuns` are the loading
    /// dyes' fronts after a full run.
    func show(_ lanes: [GelLaneModel], runLevel: Double, dyeRuns: [Double] = []) {
        self.lanes = lanes
        self.runLevel = runLevel
        self.dyeRuns = dyeRuns
        let wait = 1.0 / 30 - Date().timeIntervalSince(lastBuild)
        if wait <= 0 {
            rebuild()
        } else if !buildPending {
            buildPending = true
            DispatchQueue.main.asyncAfter(deadline: .now() + wait) { [weak self] in
                self?.buildPending = false
                self?.rebuild()
            }
        }
    }

    private func rebuild() {
        lastBuild = Date()
        let n = Float(max(1, lanes.count))
        width = n * 0.010 + (n + 1) * 0.004 + 0.012
        fit = max(width * 1.15, depth * 1.6)
        bandMap = makeBandMap()
        restart()
    }

    /// Starts the picture over: the next frames sample it afresh.
    private func restart() {
        samples = 0
        if !isHidden, tracePipeline != nil { metalView.isPaused = false }
    }

    override func viewDidHide() {
        super.viewDidHide()
        metalView.isPaused = true
    }

    override func viewDidUnhide() {
        super.viewDidUnhide()
        restart()
    }

    override func layout() {
        super.layout()
        // Trace at the screen's resolution up to about two megapixels, then
        // let the display scale it: the picture is soft and grainy anyway.
        let scale = window?.backingScaleFactor ?? 2
        var w = bounds.width * scale, h = bounds.height * scale
        let cap: CGFloat = 2_000_000
        if w * h > cap {
            let s = sqrt(cap / (w * h))
            w *= s
            h *= s
        }
        let size = CGSize(width: max(8, w.rounded()), height: max(8, h.rounded()))
        if size != metalView.drawableSize { metalView.drawableSize = size }
    }

    func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {
        accum = []
        restart()
    }

    private func ensureTextures(_ device: MTLDevice, size: CGSize) -> Bool {
        let w = Int(size.width), h = Int(size.height)
        guard w > 0, h > 0 else { return false }
        if let first = accum.first, first.width == w, first.height == h { return true }
        let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba32Float, width: w, height: h,
                                                            mipmapped: false)
        desc.usage = [.shaderRead, .shaderWrite]
        desc.storageMode = .private
        guard let a = device.makeTexture(descriptor: desc), let b = device.makeTexture(descriptor: desc) else {
            return false
        }
        accum = [a, b]
        samples = 0
        return true
    }

    private func uniforms(width w: Int, height h: Int) -> GelUniforms {
        var u = GelUniforms()
        let distance = fit * zoom
        let target = SIMD3<Float>(0, thick / 2, 0.004)
        let position = target + SIMD3<Float>(distance * sin(yaw) * cos(pitch), distance * sin(pitch),
                                             distance * cos(yaw) * cos(pitch))
        let f = simd_normalize(target - position)
        let r = simd_normalize(simd_cross(f, SIMD3<Float>(0, 1, 0)))
        let up = simd_cross(r, f)
        u.camPos = SIMD4<Float>(position, 0)
        u.camF = SIMD4<Float>(f, 0)
        u.camR = SIMD4<Float>(r, 0)
        u.camU = SIMD4<Float>(up, 0)
        // The stained gel drinks some of the light that excites the stain;
        // 1% agarose scatters gently, mostly forward.
        u.sigma = scene.sigma
        u.lens = SIMD4<Float>(tan(17.5 * .pi / 180), Float(w) / Float(h), 0.0012, distance)
        u.slab = SIMD4<Float>(width, depth, thick, 0.7)
        u.emitter = SIMD4<Float>(width / 2 + 0.13, depth / 2 + 0.11, 0.025, 1.0)
        u.look = SIMD4<Float>(0.75, 0.02, 14, 0.75)
        u.plate = scene.plate
        u.stain = scene.stain
        u.band = scene.band
        u.lip = scene.lip
        u.counts = SIMD4<UInt32>(frameIndex, samples, UInt32(w), UInt32(h))
        return u
    }

    func draw(in view: MTKView) {
        guard let device = view.device, let queue = queue, let trace = tracePipeline, let resolve = resolvePipeline,
              let bandMap = bandMap, ensureTextures(device, size: view.drawableSize),
              let drawable = view.currentDrawable, let buffer = queue.makeCommandBuffer() else { return }
        let w = accum[0].width, h = accum[0].height
        var u = uniforms(width: w, height: h)
        let threads = MTLSize(width: w, height: h, depth: 1)
        let group = MTLSize(width: 16, height: 16, depth: 1)
        if samples < maxSamples, let encoder = buffer.makeComputeCommandEncoder() {
            encoder.setComputePipelineState(trace)
            encoder.setTexture(accum[0], index: 0)
            encoder.setTexture(accum[1], index: 1)
            encoder.setTexture(bandMap, index: 2)
            encoder.setBytes(&u, length: MemoryLayout<GelUniforms>.stride, index: 0)
            encoder.dispatchThreads(threads, threadsPerThreadgroup: group)
            encoder.endEncoding()
            accum.swapAt(0, 1)
            samples += 1
            frameIndex &+= 1
        }
        if let encoder = buffer.makeComputeCommandEncoder() {
            u.counts = SIMD4<UInt32>(frameIndex, samples, UInt32(w), UInt32(h))
            encoder.setComputePipelineState(resolve)
            encoder.setTexture(accum[0], index: 0)
            encoder.setTexture(drawable.texture, index: 1)
            encoder.setBytes(&u, length: MemoryLayout<GelUniforms>.stride, index: 0)
            encoder.dispatchThreads(MTLSize(width: drawable.texture.width, height: drawable.texture.height, depth: 1),
                                    threadsPerThreadgroup: group)
            encoder.endEncoding()
        }
        buffer.present(drawable)
        buffer.commit()
        if samples >= maxSamples { view.isPaused = true }
    }

    // MARK: The stain map

    /// Lanes are never loaded quite evenly: a fixed, slight difference per lane.
    private static func loading(_ lane: Int) -> Float {
        let wobble: [Float] = [1, 0.92, 1.04, 0.96, 1.06, 0.94, 1.01, 0.95, 1.05, 0.98, 1.02]
        return wobble[lane % wobble.count]
    }

    /// A small, repeatable random sequence, so the gel looks the same each time.
    private struct Noise {
        var seed: UInt32
        mutating func next() -> Float {
            seed = seed &* 1_664_525 &+ 1_013_904_223
            return Float(seed >> 8) / Float(1 << 24)
        }
        /// Smooth noise over a plane, 0 to 1, repeatable.
        static func value(_ x: Float, _ y: Float) -> Float {
            func hash(_ i: Int32, _ j: Int32) -> Float {
                var h = UInt32(bitPattern: i) &* 1973 &+ UInt32(bitPattern: j) &* 9277 &+ 7
                h ^= h >> 16; h &*= 0x7feb352d; h ^= h >> 15; h &*= 0x846ca68b; h ^= h >> 16
                return Float(h >> 8) / Float(1 << 24)
            }
            let fx = x.rounded(.down), fy = y.rounded(.down)
            let i = Int32(fx), j = Int32(fy)
            var u = x - fx, v = y - fy
            u = u * u * (3 - 2 * u); v = v * v * (3 - 2 * v)
            let top = hash(i, j) + (hash(i + 1, j) - hash(i, j)) * u
            let bottom = hash(i, j + 1) + (hash(i + 1, j + 1) - hash(i, j + 1)) * u
            return top + (bottom - top) * v
        }
    }

    /// Where the DNA is, seen from above, as a picture the shaders sample:
    /// red = DNA, green = loading dye, blue = well (half: its walls). Row 0
    /// is the wells' side (−z).
    ///
    /// A band is a cloud of DNA, not a shape: along the run its brightness is
    /// a bell curve that widens the farther it ran (diffusion), with a faint
    /// wider haze and, when heavily loaded, a tail back toward the well;
    /// across the lane it's a little brighter in the middle and fades at the
    /// walls; its ends curl toward the well; it leans a touch; the stain in
    /// it is speckled; and no two are alike.
    private func makeBandMap() -> MTLTexture? {
        guard let device = metalView.device else { return nil }
        let w = 1024, h = max(128, min(2048, Int(Float(w) * depth / width)))
        let ppm = Float(w) / width
        let mm = ppm / 1000
        let laneW: Float = 0.010, gap: Float = 0.004
        let laneX = { (i: Int) -> Float in -self.width / 2 + 0.006 + gap + Float(i) * (laneW + gap) + laneW / 2 }
        let px = { (x: Float) -> Float in (x + self.width / 2) * ppm }
        let pz = { (z: Float) -> Float in (z + self.depth / 2) * ppm }
        let wellZ = -depth / 2 + 0.010
        let runTop = wellZ + 0.004, runBottom = depth / 2 - 0.008
        let runZ = { (p: Double) -> Float in runTop + Float(p) * (runBottom - runTop) }
        var random = Noise(seed: 11)
        let half = laneW * ppm * 0.46
        var r = [Float](repeating: 0, count: w * h)
        var g = [Float](repeating: 0, count: w * h)
        var b = [Float](repeating: 0, count: w * h)
        // The rows or columns a thing covers, clipped to the picture (empty if it's off it).
        let span = { (a: Float, b: Float, n: Int) -> Range<Int> in
            let lo = min(n, max(0, Int(a.rounded(.down))))
            return lo ..< max(lo, min(n, Int(b.rounded(.up)) + 1))
        }
        let rows = { (y0: Float, y1: Float) -> Range<Int> in span(y0, y1, h) }
        let cols = { (x0: Float, x1: Float) -> Range<Int> in span(x0, x1, w) }

        // The loading dyes' fronts: broad, soft, dark bands across every lane,
        // the first (bromophenol blue) the darker, a little uneven lane to lane.
        for (d, dye) in dyeRuns.enumerated() {
            let p = dye * runLevel
            guard p <= 1.02 else { continue }
            let y = pz(runZ(p)), sigma = (1.3 + 0.3 * Float(d)) * mm, strength: Float = d == 0 ? 0.55 : 0.32
            let left = px(-width / 2) + 2 * mm, right = px(width / 2) - 2 * mm
            for j in rows(y - 4 * sigma, y + 4 * sigma) {
                let dz = (Float(j) + 0.5 - y) / sigma
                let along = exp(-0.5 * dz * dz)
                for i in cols(left, right) {
                    let x = Float(i) + 0.5
                    let side = min(1, min(x - left, right - x) / (2 * mm))
                    let uneven = 0.85 + 0.3 * Noise.value(x / (laneW * ppm) + 3, y / (8 * mm))
                    g[j * w + i] += strength * along * side * uneven
                }
            }
        }

        for (i, lane) in lanes.enumerated() {
            let cx = px(laneX(i)), load = Self.loading(i)
            for band in lane.bands {
                let p = band.run * runLevel
                guard p <= 1.03 else { continue }
                let k = min(1, Float(band.intensity) * load)
                let y0 = pz(runZ(p))
                // How far the DNA has spread: more for a longer run, a touch more when there's more of it.
                let sigma = (0.20 + 0.22 * k) * mm * (1 + 0.7 * Float(p))
                let smile = (0.12 + 0.12 * random.next()) * mm * (1 + Float(p))
                let lean = (random.next() - 0.5) * 0.3 * mm
                let reach = half * (0.97 + 0.05 * random.next())
                let tail = max(0, k - 0.45) * 0.5
                let tailLength = (0.6 + 1.6 * k) * mm
                let speckle = random.next() * 100
                let above = y0 - 4 * sigma - (tail > 0 ? 4 * tailLength : 0), below = y0 + 4 * sigma
                for row in rows(above, below) {
                    let yy = Float(row) + 0.5
                    for col in cols(cx - reach - 0.4 * mm, cx + reach + 0.4 * mm) {
                        let xx = Float(col) + 0.5
                        let u = (xx - cx) / reach
                        // The ends lag behind the middle, and the band leans a little.
                        let centre = y0 - smile * u * u + lean * u
                        let dz = yy - centre
                        var along = exp(-0.5 * (dz / sigma) * (dz / sigma))
                        along += 0.15 * exp(-0.5 * (dz / (3 * sigma)) * (dz / (3 * sigma)))
                        if tail > 0, dz < 0 { along += tail * exp(dz / tailLength) }
                        let across = min(1, max(0, (reach - abs(xx - cx)) / (0.3 * mm))) * (0.86 + 0.14 * (1 - u * u))
                        let grain = 0.88 + 0.24 * Noise.value(xx / (0.35 * mm) + speckle, yy / (0.35 * mm))
                        r[row * w + col] += k * along * across * grain
                    }
                }
            }
            // The well: its cavity, and its walls (half blue) which catch the light.
            let wx0 = cx - half * 0.95, wx1 = cx + half * 0.95
            let wy0 = pz(wellZ) - 0.9 * mm, wy1 = pz(wellZ) + 0.9 * mm
            let wall = 0.3 * mm
            for row in rows(wy0 - wall, wy1 + wall) {
                let yy = Float(row) + 0.5
                for col in cols(wx0 - wall, wx1 + wall) {
                    let xx = Float(col) + 0.5
                    let inside = xx > wx0 && xx < wx1 && yy > wy0 && yy < wy1
                    b[row * w + col] = inside ? 1 : 0.5
                }
            }
        }

        var bytes = [UInt8](repeating: 255, count: w * h * 4)
        for i in 0..<(w * h) {
            bytes[i * 4] = UInt8(min(255, max(0, r[i] * 255)))
            bytes[i * 4 + 1] = UInt8(min(255, max(0, g[i] * 255)))
            bytes[i * 4 + 2] = UInt8(min(255, max(0, b[i] * 255)))
        }
        let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: w, height: h, mipmapped: false)
        desc.usage = [.shaderRead]
        guard let texture = device.makeTexture(descriptor: desc) else { return nil }
        bytes.withUnsafeBytes { raw in
            texture.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: raw.baseAddress!, bytesPerRow: w * 4)
        }
        return texture
    }

    // MARK: The shaders

    /// Compiled when the view is made, so a bad shader is a note on the
    /// view rather than a broken build.
    private static let shaders = """
    #include <metal_stdlib>
    using namespace metal;

    struct Uniforms {
        float4 camPos, camF, camR, camU, sigma, lens, slab, emitter, look, plate, stain, band, lip;
        uint4 counts;
    };

    constant float IOR = 1.34;
    constant float EPS = 0.00004;

    inline uint hashu(uint x) {
        x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
        return x;
    }
    inline float rnd(thread uint &s) {
        s = hashu(s + 0x9e3779b9U);
        return float(s >> 8) * (1.0 / 16777216.0);
    }
    inline float hashf(float2 p) {
        uint2 q = uint2(int2(floor(p)) + int2(1 << 20));
        return float(hashu(q.x * 1973u + q.y * 9277u + 7u) >> 8) * (1.0 / 16777216.0);
    }
    inline float vnoise(float2 p) {
        float2 i = floor(p), f = fract(p);
        f = f * f * (3.0 - 2.0 * f);
        float a = hashf(i), b = hashf(i + float2(1, 0)), c = hashf(i + float2(0, 1)), d = hashf(i + float2(1, 1));
        return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
    }

    // The ray against the slab, an axis-aligned box: where it goes in and
    // out, and the outward normals there.
    inline float fresnel(float cosi, float cost, float eta) {
        float rs = (eta * cosi - cost) / (eta * cosi + cost);
        float rp = (cosi - eta * cost) / (cosi + eta * cost);
        return 0.5 * (rs * rs + rp * rp);
    }

    // A new direction after scattering in the gel (Henyey-Greenstein).
    inline float3 sampleHG(float3 d, float g, thread uint &seed) {
        float u1 = rnd(seed), u2 = rnd(seed);
        float cosT;
        if (abs(g) < 0.001) {
            cosT = 1.0 - 2.0 * u1;
        } else {
            float sq = (1.0 - g * g) / (1.0 - g + 2.0 * g * u1);
            cosT = (1.0 + g * g - sq * sq) / (2.0 * g);
        }
        cosT = clamp(cosT, -1.0, 1.0);
        float sinT = sqrt(1.0 - cosT * cosT), phi = 6.2831853 * u2;
        float3 a = abs(d.x) > 0.9 ? float3(0, 1, 0) : float3(1, 0, 0);
        float3 t = normalize(cross(a, d)), b = cross(d, t);
        return normalize(t * (sinT * cos(phi)) + b * (sinT * sin(phi)) + d * cosT);
    }

    // Dust on the plate: a round speck in a few of the cells.
    inline float dust(float2 p, float share) {
        float2 cell = floor(p);
        if (hashf(cell) > share) return 1.0;
        float2 c = cell + float2(hashf(cell + 13.0), hashf(cell + 29.0));
        float r = 0.2 + 0.25 * hashf(cell + 41.0);
        return mix(0.45, 1.0, smoothstep(r * 0.3, r, length(p - c)));
    }

    // The plate: a diffuser lit from below, brightest under the gel,
    // mottled, dusty, dark beyond its edges.
    inline float3 plate(float2 q, constant Uniforms &u) {
        float halfX = u.emitter.x, front = u.emitter.y, back = u.emitter.z;
        float zmin = -u.slab.y * 0.5 - back;
        float edge = smoothstep(0.0, 0.012, halfX - abs(q.x))
                   * smoothstep(0.0, 0.012, q.y - zmin) * smoothstep(0.0, 0.012, front - q.y);
        float3 dark = u.plate.xyz * 0.016 + 0.0004;
        if (edge <= 0.0) {
            // The box's black frame around the glass: matte, with a faint
            // bevel lit by the glass beside it.
            float past = max(max(abs(q.x) - halfX, q.y - front), zmin - q.y);
            float bevel = smoothstep(0.004, 0.0, past);
            float frame = smoothstep(0.03, 0.018, past);
            return dark + frame * (float3(0.012) * (0.8 + 0.4 * vnoise(q * 300.0)) + u.plate.xyz * u.emitter.w * 0.05 * bevel);
        }
        // Brightest a little behind the gel, near black at the corners.
        float2 rel = (q - float2(0.0, -0.3 * front)) / float2(halfX, front);
        float fall = mix(1.0, 0.06, smoothstep(0.12, 1.1, length(rel)));
        // Blotches, out of focus: the plate's scratches and smudges.
        float mott = 0.80 + 0.40 * vnoise(q * 25.0 + 3.0);
        float blot = 1.0 - 0.35 * smoothstep(0.55, 0.8, vnoise(q * 60.0 + 9.0));
        float speck = dust(q * 500.0, 0.06) * dust(q * 1500.0 + 5.0, 0.03);
        float grain = 0.95 + 0.10 * vnoise(q * 900.0);
        float3 lit = u.plate.xyz * u.emitter.w;
        return mix(dark, lit * fall * mott * blot * speck * grain, edge);
    }

    // The dim room above, with one small lamp high up behind the gel.
    inline float3 room(float3 p, float3 d) {
        float3 dark = float3(0.002, 0.001, 0.006);
        if (d.y <= 0.0001) return dark;
        float t = (0.55 - p.y) / d.y;
        float3 h = p + d * t;
        float lx = smoothstep(0.0, 0.02, 0.05 - abs(h.x - 0.03));
        float lz = smoothstep(0.0, 0.02, 0.05 - abs(h.z + 0.30));
        return dark + float3(14.0, 13.0, 12.0) * lx * lz;
    }

    // The wet top isn't quite smooth: a fine ripple tilts its reflections.
    inline float3 ripple(float3 x, float3 n) {
        if (n.y < 0.5) return n;
        float2 q = x.xz * 160.0;
        float dx = vnoise(q + float2(0.7, 0.0)) - vnoise(q - float2(0.7, 0.0));
        float dz = vnoise(q + float2(0.0, 0.7)) - vnoise(q - float2(0.0, 0.7));
        return normalize(n + float3(dx, 0.0, dz) * 0.035);
    }

    // The slab's shape. A cast gel isn't a box: its top sags a little and
    // climbs the tray's walls in a lip, its outline wanders by a fraction
    // of a millimetre, its edges are softened, and the wells are cut into
    // it. Height of the top at xz:
    inline float slabTop(float2 xz, constant Uniforms &u, texture2d<float> bands) {
        constexpr sampler s(address::clamp_to_edge, filter::linear);
        float hw = u.slab.x * 0.5, hd = u.slab.y * 0.5, thick = u.slab.z;
        float2 uv = float2(xz.x / u.slab.x + 0.5, xz.y / u.slab.y + 0.5);
        float well = bands.sample(s, uv).b;
        float inset = min(hw - abs(xz.x), hd - abs(xz.y));
        float lip = exp(-max(inset, 0.0) / 0.0015) * 0.00055;
        float sag = (vnoise(xz * 120.0) - 0.5) * 0.00014 + (vnoise(xz * 23.0 + 7.0) - 0.5) * 0.0003;
        float cut = smoothstep(0.55, 0.85, well) * u.slab.w * thick;
        return thick + lip + sag - cut;
    }
    // Signed distance to the slab: negative inside.
    inline float slabSDF(float3 p, constant Uniforms &u, texture2d<float> bands) {
        float hw = u.slab.x * 0.5, hd = u.slab.y * 0.5;
        float r = 0.0007;
        float2 e = float2(hw + (vnoise(float2(p.z * 90.0, 1.5)) - 0.5) * 0.0006,
                          hd + (vnoise(float2(p.x * 90.0, 8.5)) - 0.5) * 0.0006);
        float2 q = abs(p.xz) - (e - r);
        float dxz = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
        float dy = max(p.y - slabTop(p.xz, u, bands), -p.y);
        // Softened where the top meets the sides.
        float k = 0.0004;
        float h = clamp(0.5 + 0.5 * (dxz - dy) / k, 0.0, 1.0);
        return mix(dy, dxz, h) + k * h * (1.0 - h);
    }
    inline float3 slabNormal(float3 p, constant Uniforms &u, texture2d<float> bands) {
        float e = 0.00003;
        float3 n = float3(slabSDF(p + float3(e, 0, 0), u, bands) - slabSDF(p - float3(e, 0, 0), u, bands),
                          slabSDF(p + float3(0, e, 0), u, bands) - slabSDF(p - float3(0, e, 0), u, bands),
                          slabSDF(p + float3(0, 0, e), u, bands) - slabSDF(p - float3(0, 0, e), u, bands));
        return normalize(n);
    }
    // The box around the slab with room for its lip and wandering outline;
    // where a ray crosses it, or false.
    inline bool slabBox(float3 o, float3 d, constant Uniforms &u, thread float &t0, thread float &t1) {
        float pad = 0.0015;
        float3 bmin = float3(-u.slab.x * 0.5 - pad, -0.0001, -u.slab.y * 0.5 - pad);
        float3 bmax = float3(u.slab.x * 0.5 + pad, u.slab.z + pad, u.slab.y * 0.5 + pad);
        float3 inv = 1.0 / d;
        float3 ta = (bmin - o) * inv, tb = (bmax - o) * inv;
        float3 tmin = min(ta, tb), tmax = max(ta, tb);
        t0 = max(max(max(tmin.x, tmin.y), tmin.z), 0.0);
        t1 = min(min(tmax.x, tmax.y), tmax.z);
        return t1 > t0;
    }
    // Where a ray from outside meets the slab, walking the distance field;
    // false if it misses. Starts a little along, so a ray that has just
    // left the slab doesn't find the face it left.
    inline bool enterSlab(float3 o, float3 d, constant Uniforms &u, texture2d<float> bands, thread float &tHit) {
        float t0, t1;
        if (!slabBox(o, d, u, t0, t1)) return false;
        float t = max(t0, 0.0002), prev = t;
        float dp = slabSDF(o + d * t, u, bands);
        for (int i = 0; i < 80 && t < t1; i++) {
            if (dp < 0.00002) {
                if (dp < -0.00002) {
                    // Stepped through a well's wall or the lip: find the face.
                    float a = prev, b = t;
                    for (int j = 0; j < 5; j++) {
                        float m = 0.5 * (a + b);
                        if (slabSDF(o + d * m, u, bands) > 0.0) a = m; else b = m;
                    }
                    t = b;
                }
                tHit = t;
                return true;
            }
            prev = t;
            t += max(dp * 0.9, 0.00004);
            dp = slabSDF(o + d * t, u, bands);
        }
        return false;
    }

    // What a ray sees once it's clear of the slab: the plate, wet around the
    // gel where buffer spilt (a film that mirrors the room), or the room.
    inline float3 outside(float3 p, float3 d, constant Uniforms &u) {
        if (d.y >= -0.0001) return room(p, d);
        float t = -p.y / d.y;
        float3 h = p + d * t;
        float3 c = plate(h.xz, u);
        float2 q = abs(h.xz) - float2(u.slab.x * 0.5, u.slab.y * 0.5);
        float away = length(max(q, 0.0));
        float wet = smoothstep(0.012, 0.0, away + (vnoise(h.xz * 180.0) - 0.5) * 0.008)
                  + smoothstep(0.64, 0.76, vnoise(h.xz * 45.0 + 3.0)) * 0.7;
        wet = clamp(wet, 0.0, 1.0);
        if (wet > 0.0) {
            float2 w = h.xz * 320.0;
            float3 n = normalize(float3((vnoise(w) - 0.5) * 0.10, 1.0, (vnoise(w + 5.0) - 0.5) * 0.10));
            float cosi = max(0.01, -dot(d, n));
            float eta = 1.0 / 1.33;
            float cost = sqrt(max(0.0, 1.0 - eta * eta * (1.0 - cosi * cosi)));
            float F = fresnel(cosi, cost, eta);
            c = mix(c, c * (1.0 - F) + room(h, reflect(d, n)) * F, wet);
        }
        return c;
    }

    // The gel at a point: what it gives off and what it drinks.
    struct Medium { float3 emit; float3 absorb; };
    inline Medium medium(float3 p, constant Uniforms &u, texture2d<float> bands) {
        constexpr sampler s(address::clamp_to_edge, filter::linear);
        float2 uv = float2(p.x / u.slab.x + 0.5, p.z / u.slab.y + 0.5);
        float4 m = bands.sample(s, uv);
        float yf = p.y / u.slab.z;
        // The DNA fills the thickness, fading at the faces.
        float prof = smoothstep(0.0, 0.15, yf) * smoothstep(1.0, 0.85, yf);
        // The well's cut walls scatter the light: a bright rim.
        float wall = (m.b > 0.2 && m.b <= 0.8 && yf > 1.0 - u.slab.w) ? 1.0 : 0.0;
        // The stain is never quite even.
        float uneven = 0.72 + 0.36 * vnoise(uv * float2(9.0, 5.0) + 1.0) + 0.12 * vnoise(uv * float2(60.0, 33.0));
        float3 stain = u.stain.xyz * u.stain.w, band = u.band.xyz * u.band.w;
        Medium md;
        md.emit = (stain * uneven * (1.0 + 6.0 * wall) + band * m.r * prof) * u.look.z;
        md.absorb = u.sigma.xyz + float3(0.6, 0.9, 0.15) * 260.0 * m.g;
        return md;
    }

    kernel void gelTrace(texture2d<float, access::read> prev [[texture(0)]],
                         texture2d<float, access::write> next [[texture(1)]],
                         texture2d<float, access::sample> bands [[texture(2)]],
                         constant Uniforms &u [[buffer(0)]],
                         uint2 gid [[thread_position_in_grid]]) {
        uint W = u.counts.z, H = u.counts.w;
        if (gid.x >= W || gid.y >= H) return;
        uint seed = hashu(gid.x * 1973u + gid.y * 9277u + u.counts.x * 26699u + 1u);

        // The camera ray: through a random point in the pixel and a thin lens.
        float2 px = (float2(gid) + float2(rnd(seed), rnd(seed))) / float2(W, H);
        float2 ndc = float2(px.x * 2.0 - 1.0, 1.0 - px.y * 2.0);
        float tanH = u.lens.x, aspect = u.lens.y;
        float3 dir = normalize(u.camF.xyz + u.camR.xyz * (ndc.x * tanH * aspect) + u.camU.xyz * (ndc.y * tanH));
        float3 o = u.camPos.xyz;
        {
            float r = sqrt(rnd(seed)) * u.lens.z, a = rnd(seed) * 6.2831853;
            float3 focus = o + dir * (u.lens.w / dot(dir, u.camF.xyz));
            o += u.camR.xyz * (r * cos(a)) + u.camU.xyz * (r * sin(a));
            dir = normalize(focus - o);
        }

        float hw = u.slab.x * 0.5, hd = u.slab.y * 0.5;
        float3 L = float3(0.0), T = float3(1.0);
        float3 p = o, d = dir;
        bool inside = false;
        int scatters = 0;
        for (int bounce = 0; bounce < 12; bounce++) {
            if (!inside) {
                float tHit;
                if (!enterSlab(p, d, u, bands, tHit)) { L += T * outside(p, d, u); break; }
                // Into the gel, or off its surface.
                float3 x = p + d * tHit;
                float3 ng = slabNormal(x, u, bands);
                // The top's cut edges scatter the trapped light: a thin bright lip.
                if (ng.y > 0.5) {
                    float ex = min(hw - abs(x.x), hd - abs(x.z));
                    float lip = smoothstep(0.0012, 0.0, ex);
                    L += T * u.lip.xyz * u.lip.w * lip * u.emitter.w;
                }
                float3 n = ripple(x, ng);
                float cosi = max(0.01, -dot(d, n));
                float eta = 1.0 / IOR;
                float sin2t = eta * eta * (1.0 - cosi * cosi);
                float cost = sqrt(max(0.0, 1.0 - sin2t));
                float F = fresnel(cosi, cost, eta);
                if (rnd(seed) < F) { d = reflect(d, n); p = x + ng * EPS; continue; }
                d = normalize(eta * d + (eta * cosi - cost) * n);
                p = x - ng * EPS;
                inside = true;
                continue;
            }
            // Through the gel, gathering the stain's light step by step until
            // the ray leaves the block or is scattered off course.
            float t0, t1;
            if (!slabBox(p - d * 0.001, d, u, t0, t1)) break;
            t1 = max(t1 - 0.001, 0.0005);
            int steps = clamp(int(t1 / 0.0005) + 1, 3, 64);
            float ds = t1 / float(steps);
            float ps = 1.0 - exp(-u.sigma.w * ds);
            bool scattered = false, left = false;
            float tPrev = 0.0, tExit = 0.0;
            for (int i = 0; i < steps; i++) {
                float tq = (float(i) + 1.0) * ds;
                float3 q = p + d * tq;
                if (slabSDF(q, u, bands) > 0.0) {
                    // Crossed a face: find it, and take the gel up to it.
                    float a = tPrev, b = tq;
                    for (int j = 0; j < 5; j++) {
                        float m = 0.5 * (a + b);
                        if (slabSDF(p + d * m, u, bands) > 0.0) b = m; else a = m;
                    }
                    float dl = a - tPrev;
                    Medium m = medium(p + d * (tPrev + 0.5 * dl), u, bands);
                    L += T * m.emit * dl;
                    T *= exp(-m.absorb * dl);
                    tExit = 0.5 * (a + b);
                    left = true;
                    break;
                }
                Medium m = medium(p + d * (tq - 0.5 * ds), u, bands);
                L += T * m.emit * ds;
                T *= exp(-m.absorb * ds);
                if (scatters < 4 && rnd(seed) < ps) {
                    d = sampleHG(d, u.look.w, seed);
                    p = q;
                    scatters++;
                    scattered = true;
                    break;
                }
                tPrev = tq;
            }
            if (scattered) continue;
            if (!left) break;
            // At the face: out, or back in off its inside.
            float3 x = p + d * tExit;
            float3 ng = slabNormal(x, u, bands);
            float3 n = ripple(x, ng);
            float cosi = max(0.01, dot(d, n));
            float eta = IOR;
            float sin2t = eta * eta * (1.0 - cosi * cosi);
            if (sin2t >= 1.0) { d = reflect(d, n); p = x - ng * EPS; continue; }
            float cost = sqrt(1.0 - sin2t);
            float F = fresnel(cosi, cost, eta);
            if (rnd(seed) < F) { d = reflect(d, n); p = x - ng * EPS; continue; }
            d = normalize(eta * d - (eta * cosi - cost) * n);
            p = x + ng * EPS;
            inside = false;
            // The bottom face lies on the plate.
            if (ng.y < -0.5) { L += T * plate(x.xz, u); break; }
        }
        L = min(L, float3(6.0));
        float4 acc = (u.counts.y == 0u) ? float4(0.0) : prev.read(gid);
        next.write(acc + float4(L, 1.0), gid);
    }

    // The samples so far, averaged and developed as a phone would: a little
    // bloom off the bright parts, darker corners, grain, a filmic curve.
    kernel void gelResolve(texture2d<float, access::read> accum [[texture(0)]],
                           texture2d<float, access::write> out [[texture(1)]],
                           constant Uniforms &u [[buffer(0)]],
                           uint2 gid [[thread_position_in_grid]]) {
        uint W = u.counts.z, H = u.counts.w;
        if (gid.x >= W || gid.y >= H) return;
        float n = max(1.0, float(u.counts.y));
        float3 c = accum.read(gid).rgb / n;
        // With few samples yet, blur the grain a little: a moving picture is soft anyway.
        float soft = clamp(1.0 - n / 10.0, 0.0, 0.75);
        if (soft > 0.0) {
            float3 avg = float3(0.0);
            for (int j = -1; j <= 1; j++) {
                for (int i = -1; i <= 1; i++) {
                    int2 q = clamp(int2(gid) + int2(i, j), int2(0), int2(int(W) - 1, int(H) - 1));
                    avg += accum.read(uint2(q)).rgb / n;
                }
            }
            c = mix(c, avg / 9.0, soft);
        }
        float3 bloom = float3(0.0);
        float wsum = 0.0;
        float step = max(1.0, float(W) / 420.0) * 2.0;
        for (int j = -3; j <= 3; j++) {
            for (int i = -3; i <= 3; i++) {
                int2 q = int2(gid) + int2(float2(i, j) * step);
                q = clamp(q, int2(0), int2(int(W) - 1, int(H) - 1));
                float w = exp(-0.35 * float(i * i + j * j));
                float3 s = accum.read(uint2(q)).rgb / n;
                bloom += max(s - float3(1.1), float3(0.0)) * w;
                wsum += w;
            }
        }
        c = (c + bloom / wsum * 0.25) * u.look.x;
        float2 uv = float2(gid) / float2(W, H) - 0.5;
        c *= 1.0 - 0.6 * dot(uv, uv);
        // Shot noise: more where there's more light, none in the black.
        float g = (hashf(float2(gid) + float2(float(u.counts.x % 977u), 0.0)) - 0.5) * u.look.y;
        c += g * 2.5 * sqrt(max(c, float3(0.0)));
        // A black and white camera sees only how much light.
        if (u.plate.w > 0.5) c = float3(dot(c, float3(0.2126, 0.7152, 0.0722)));
        // A gentle curve that keeps the colours saturated, with a soft shoulder.
        float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
        c *= 1.0 / (1.0 + 0.25 * lum);
        c = select(0.85 + 0.15 * (1.0 - exp(-(c - 0.85) / 0.15)), c, c < 0.85);
        c = pow(clamp(c, float3(0.0), float3(1.0)), float3(1.0 / 2.2));
        out.write(float4(c, 1.0), gid);
    }
    """
}
