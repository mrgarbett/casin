// SPDX-License-Identifier: GPL-3.0-or-later
// EXPERIMENTAL: the gel photographed on a blue-light box, by path tracing on
// the GPU (Metal). Nothing here is painted to look like a gel: the scene is
// a block of agarose (a refracting, faintly scattering, stain-filled medium)
// lying on a glowing blue plate under a dark room with one lamp, and the
// picture is what a camera would see. Light enters the block from the plate,
// the stain fluoresces pink everywhere and orange where the DNA is, rays
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
    /// Only a drag that started on the gel turns it.
    private var grabbed = false
    override func mouseDown(with event: NSEvent) {
        grabbed = true
        if event.clickCount == 2 { onReset?() }
    }
    override func mouseUp(with event: NSEvent) { grabbed = false }
    override func mouseDragged(with event: NSEvent) {
        if grabbed { onDrag?(event.deltaX, event.deltaY) }
    }
    override func scrollWheel(with event: NSEvent) { onScroll?(event.scrollingDeltaY) }
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
    private let maxSamples: UInt32 = 256

    private static let homePitch: Float = 0.80
    private var yaw: Float = 0
    private var pitch: Float = homePitch
    private var zoom: Float = 1
    /// How far the camera stands back to take in the whole gel.
    private var fit: Float = 0.2
    private var lanes: [GelLaneModel] = []
    private var runLevel: Double = 1
    private var dyeRuns: [Double] = []
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

    private func fail(_ message: String) {
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
        // The stained gel drinks a little of the blue and less of the red;
        // 1% agarose scatters gently, mostly forward.
        u.sigma = SIMD4<Float>(10, 26, 7, 18)
        u.lens = SIMD4<Float>(tan(17.5 * .pi / 180), Float(w) / Float(h), 0.0012, distance)
        u.slab = SIMD4<Float>(width, depth, thick, 0.6)
        u.emitter = SIMD4<Float>(width / 2 + 0.13, depth / 2 + 0.11, 0.025, 1.0)
        u.look = SIMD4<Float>(1.15, 0.045, 46, 0.75)
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
    private static func loading(_ lane: Int) -> CGFloat {
        let wobble: [CGFloat] = [1, 0.92, 1.04, 0.96, 1.06, 0.94, 1.01, 0.95, 1.05, 0.98, 1.02]
        return wobble[lane % wobble.count]
    }

    /// A small, repeatable random sequence, so the gel looks the same each time.
    private struct Noise {
        var seed: UInt32
        mutating func next() -> CGFloat {
            seed = seed &* 1_664_525 &+ 1_013_904_223
            return CGFloat(seed >> 8) / CGFloat(1 << 24)
        }
    }

    /// Fills `path` blurred by `blur`: only the shadow of a copy far off to
    /// the side lands in the picture.
    private static func softFill(_ ctx: CGContext, _ path: CGPath, color: CGColor, blur: CGFloat) {
        let away: CGFloat = 100_000
        ctx.saveGState()
        ctx.setShadow(offset: CGSize(width: away, height: 0), blur: blur, color: color)
        ctx.translateBy(x: -away, y: 0)
        ctx.addPath(path)
        ctx.setFillColor(CGColor(gray: 0, alpha: 1))
        ctx.fillPath()
        ctx.restoreGState()
    }

    private static func rgb(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ a: CGFloat = 1) -> CGColor {
        CGColor(srgbRed: r, green: g, blue: b, alpha: a)
    }

    /// Where the DNA is, seen from above, as a picture the shaders sample:
    /// red = DNA (band and smear), green = loading dye, blue = well. Row 0
    /// is the wells' side (−z).
    private func makeBandMap() -> MTLTexture? {
        guard let device = metalView.device else { return nil }
        let w = 1024, h = max(128, min(2048, Int(CGFloat(w) * CGFloat(depth / width))))
        guard let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0,
                                  space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                  bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) else { return nil }
        ctx.translateBy(x: 0, y: CGFloat(h))
        ctx.scaleBy(x: 1, y: -1)
        ctx.setFillColor(CGColor(gray: 0, alpha: 1))
        ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
        let ppm = CGFloat(w) / CGFloat(width)
        let mm = ppm / 1000
        let laneW: Float = 0.010, gap: Float = 0.004
        let laneX = { (i: Int) -> Float in -self.width / 2 + 0.006 + gap + Float(i) * (laneW + gap) + laneW / 2 }
        let px = { (x: Float) -> CGFloat in CGFloat(x + self.width / 2) * ppm }
        let pz = { (z: Float) -> CGFloat in CGFloat(z + self.depth / 2) * ppm }
        let wellZ = -depth / 2 + 0.010
        let runTop = wellZ + 0.004, runBottom = depth / 2 - 0.008
        let runZ = { (p: Double) -> Float in runTop + Float(p) * (runBottom - runTop) }
        var random = Noise(seed: 11)
        let half = CGFloat(laneW) * ppm * 0.46

        for (i, lane) in lanes.enumerated() {
            let cx = px(laneX(i)), load = Self.loading(i)
            var lowest = -1.0
            for band in lane.bands {
                let p = band.run * runLevel
                guard p <= 1.03 else { continue }
                lowest = max(lowest, p)
                let k = min(1, CGFloat(band.intensity) * load)
                let y = pz(runZ(p))
                let th = (0.55 + 1.0 * k) * mm * (1 + 0.4 * CGFloat(p))
                let smile = 0.2 * mm * (1 + CGFloat(p))
                // No two bands quite the same: ends and thickness waver.
                let left = cx - half + (random.next() - 0.5) * 0.4 * mm
                let right = cx + half + (random.next() - 0.5) * 0.4 * mm
                let lean = (random.next() - 0.5) * 0.2 * th
                let top = y - th / 2, bottom = y + th / 2 * (0.9 + 0.2 * random.next())
                let path = CGMutablePath()
                path.move(to: CGPoint(x: left, y: top + lean))
                path.addQuadCurve(to: CGPoint(x: right, y: top - lean), control: CGPoint(x: cx, y: top + 2 * smile))
                path.addLine(to: CGPoint(x: right, y: bottom - lean))
                path.addQuadCurve(to: CGPoint(x: left, y: bottom + lean), control: CGPoint(x: cx, y: bottom + 2 * smile))
                path.closeSubpath()
                // DNA diffuses a little: a soft edge, and a faint wider fringe lower down.
                Self.softFill(ctx, path, color: Self.rgb(1, 0, 0, 0.3 * k), blur: (0.6 + 1.2 * CGFloat(p)) * mm)
                Self.softFill(ctx, path, color: Self.rgb(1, 0, 0, k), blur: 0.25 * mm)
            }
            if !lane.isLadder, lowest > 0 {
                let top = pz(wellZ) + 1.5 * mm, bottom = pz(runZ(lowest))
                let smear = CGRect(x: cx - half * 0.85, y: top, width: half * 1.7, height: max(0, bottom - top))
                Self.softFill(ctx, CGPath(rect: smear, transform: nil), color: Self.rgb(1, 0, 0, 0.05 * load),
                              blur: 0.9 * mm)
            }
            if !lane.isLadder {
                for dye in dyeRuns {
                    let p = dye * runLevel
                    guard p <= 1.02 else { continue }
                    let dw = half * (1.7 + 0.25 * random.next()), dh = (1.4 + 0.8 * random.next()) * mm
                    let r = CGRect(x: cx - dw / 2 + (random.next() - 0.5) * mm, y: pz(runZ(p)) - dh / 2, width: dw, height: dh)
                    Self.softFill(ctx, CGPath(ellipseIn: r, transform: nil), color: Self.rgb(0, 1, 0, 0.35 + 0.2 * random.next()),
                                  blur: 1.0 * mm)
                }
            }
            let well = CGRect(x: cx - half * 0.95, y: pz(wellZ) - 0.9 * mm, width: 1.9 * half, height: 1.8 * mm)
            ctx.setFillColor(Self.rgb(0, 0, 1))
            ctx.fill(well)
        }

        guard let data = ctx.data else { return nil }
        let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: w, height: h, mipmapped: false)
        desc.usage = [.shaderRead]
        guard let texture = device.makeTexture(descriptor: desc) else { return nil }
        texture.replace(region: MTLRegionMake2D(0, 0, w, h), mipmapLevel: 0, withBytes: data, bytesPerRow: ctx.bytesPerRow)
        return texture
    }

    // MARK: The shaders

    /// Compiled when the view is made, so a bad shader is a note on the
    /// view rather than a broken build.
    private static let shaders = """
    #include <metal_stdlib>
    using namespace metal;

    struct Uniforms {
        float4 camPos, camF, camR, camU, sigma, lens, slab, emitter, look;
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
    inline bool slabHit(float3 o, float3 d, float3 bmin, float3 bmax,
                        thread float &t0, thread float &t1, thread float3 &n0, thread float3 &n1) {
        float3 inv = 1.0 / d;
        float3 ta = (bmin - o) * inv, tb = (bmax - o) * inv;
        float3 tmin = min(ta, tb), tmax = max(ta, tb);
        t0 = max(max(tmin.x, tmin.y), tmin.z);
        t1 = min(min(tmax.x, tmax.y), tmax.z);
        if (t1 <= max(t0, 0.0)) return false;
        if (t0 == tmin.x) n0 = float3(-sign(d.x), 0, 0);
        else if (t0 == tmin.y) n0 = float3(0, -sign(d.y), 0);
        else n0 = float3(0, 0, -sign(d.z));
        if (t1 == tmax.x) n1 = float3(sign(d.x), 0, 0);
        else if (t1 == tmax.y) n1 = float3(0, sign(d.y), 0);
        else n1 = float3(0, 0, sign(d.z));
        return true;
    }

    // Reflectance of a dielectric; eta = n1 / n2.
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

    // The blue plate: a diffuser lit from below, brightest under the gel,
    // mottled, dusty, dark beyond its edges.
    inline float3 plate(float2 q, constant Uniforms &u) {
        float halfX = u.emitter.x, front = u.emitter.y, back = u.emitter.z;
        float zmin = -u.slab.y * 0.5 - back;
        float edge = smoothstep(0.0, 0.012, halfX - abs(q.x))
                   * smoothstep(0.0, 0.012, q.y - zmin) * smoothstep(0.0, 0.012, front - q.y);
        float3 dark = float3(0.004, 0.004, 0.02);
        if (edge <= 0.0) return dark;
        float2 rel = q / float2(halfX, front);
        float fall = mix(1.0, 0.3, smoothstep(0.15, 1.25, length(rel)));
        float mott = 0.82 + 0.36 * vnoise(q * 70.0 + 3.0);
        float sp = hashf(q * 2200.0);
        float speck = sp < 0.010 ? 0.3 : (sp < 0.016 ? 1.35 : 1.0);
        float3 blue = float3(0.09, 0.10, 1.0) * u.emitter.w;
        return mix(dark, blue * fall * mott * speck, edge);
    }

    // The dark room above, with one lamp behind and above the gel.
    inline float3 room(float3 p, float3 d) {
        float3 dark = float3(0.003, 0.003, 0.005);
        if (d.y <= 0.0001) return dark;
        float t = (0.30 - p.y) / d.y;
        float3 h = p + d * t;
        float lx = smoothstep(0.0, 0.03, 0.14 - abs(h.x));
        float lz = smoothstep(0.0, 0.03, 0.07 - abs(h.z + 0.24));
        return dark + float3(5.0, 4.6, 4.2) * lx * lz;
    }

    // What a ray sees once it's clear of the slab.
    inline float3 outside(float3 p, float3 d, constant Uniforms &u) {
        if (d.y < -0.0001) {
            float t = -p.y / d.y;
            float3 h = p + d * t;
            return plate(h.xz, u);
        }
        return room(p, d);
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
        // A well is a slot of buffer cut into the top: no stain there.
        float inWell = (m.b > 0.5 && yf > 1.0 - u.slab.w) ? 1.0 : 0.0;
        // The stain is never quite even.
        float uneven = 0.85 + 0.3 * vnoise(uv * float2(9.0, 5.0) + 1.0);
        float3 pink = float3(1.0, 0.36, 0.60), orange = float3(1.0, 0.50, 0.33);
        Medium md;
        md.emit = (pink * uneven + orange * m.r * 14.0 * prof) * u.look.z * (1.0 - inWell);
        md.absorb = u.sigma.xyz + float3(0.9, 0.6, 0.12) * 260.0 * m.g;
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

        float3 bmin = float3(-u.slab.x * 0.5, 0.0, -u.slab.y * 0.5);
        float3 bmax = float3(u.slab.x * 0.5, u.slab.z, u.slab.y * 0.5);
        float3 L = float3(0.0), T = float3(1.0);
        float3 p = o, d = dir;
        bool inside = false;
        int scatters = 0;
        for (int bounce = 0; bounce < 12; bounce++) {
            float t0, t1;
            float3 n0, n1;
            bool hit = slabHit(p, d, bmin, bmax, t0, t1, n0, n1);
            if (!inside) {
                if (!hit || t0 < 0.0) { L += T * outside(p, d, u); break; }
                // Into the gel, or off its surface.
                float3 x = p + d * t0;
                float cosi = -dot(d, n0);
                float eta = 1.0 / IOR;
                float sin2t = eta * eta * (1.0 - cosi * cosi);
                float cost = sqrt(max(0.0, 1.0 - sin2t));
                float F = fresnel(cosi, cost, eta);
                if (rnd(seed) < F) { d = reflect(d, n0); p = x + n0 * EPS; continue; }
                d = normalize(eta * d + (eta * cosi - cost) * n0);
                p = x - n0 * EPS;
                inside = true;
                continue;
            }
            if (!hit) break;
            // Through the gel to its far face, gathering the stain's light,
            // sometimes scattered off course.
            int steps = clamp(int(t1 / 0.0005) + 1, 3, 64);
            float ds = t1 / float(steps);
            float ps = 1.0 - exp(-u.sigma.w * ds);
            bool scattered = false;
            for (int i = 0; i < steps; i++) {
                float3 q = p + d * ((float(i) + 0.5) * ds);
                Medium m = medium(q, u, bands);
                L += T * m.emit * ds;
                T *= exp(-m.absorb * ds);
                if (scatters < 4 && rnd(seed) < ps) {
                    d = sampleHG(d, u.look.w, seed);
                    p = q;
                    scatters++;
                    scattered = true;
                    break;
                }
            }
            if (scattered) continue;
            // At the far face: out, or back in off the inside of the face.
            float3 x = p + d * t1;
            float cosi = dot(d, n1);
            float eta = IOR;
            float sin2t = eta * eta * (1.0 - cosi * cosi);
            if (sin2t >= 1.0) { d = reflect(d, n1); p = x - n1 * EPS; continue; }
            float cost = sqrt(1.0 - sin2t);
            float F = fresnel(cosi, cost, eta);
            if (rnd(seed) < F) { d = reflect(d, n1); p = x - n1 * EPS; continue; }
            d = normalize(eta * d - (eta * cosi - cost) * n1);
            p = x + n1 * EPS;
            inside = false;
            // The bottom face lies on the plate.
            if (n1.y < -0.5) { L += T * plate(x.xz, u); break; }
        }
        L = min(L, float3(40.0));
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
        float3 bloom = float3(0.0);
        float wsum = 0.0;
        float step = max(1.0, float(W) / 420.0) * 2.0;
        for (int j = -3; j <= 3; j++) {
            for (int i = -3; i <= 3; i++) {
                int2 q = int2(gid) + int2(float2(i, j) * step);
                q = clamp(q, int2(0), int2(int(W) - 1, int(H) - 1));
                float w = exp(-0.35 * float(i * i + j * j));
                float3 s = accum.read(uint2(q)).rgb / n;
                bloom += max(s - float3(0.9), float3(0.0)) * w;
                wsum += w;
            }
        }
        c = (c + bloom / wsum * 0.6) * u.look.x;
        float2 uv = float2(gid) / float2(W, H) - 0.5;
        c *= 1.0 - 0.6 * dot(uv, uv);
        float g = (hashf(float2(gid) + float2(float(u.counts.x % 977u), 0.0)) - 0.5) * u.look.y;
        c += g / (1.0 + c);
        c = (c * (2.51 * c + 0.03)) / (c * (2.43 * c + 0.59) + 0.14);
        c = pow(clamp(c, float3(0.0), float3(1.0)), float3(1.0 / 2.2));
        out.write(float4(c, 1.0), gid);
    }
    """
}
