// swift-tools-version:5.9
// The macOS app: Swift + AppKit over the shared C++ core (through its C API).
// Build with `swift build -c release` or open this file in Xcode; wrap it into
// Restricto.app with scripts/build_macos_app.sh.
import PackageDescription

let package = Package(
    name: "Restricto",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "Restricto", targets: ["Restricto"]),
        // The Quick Look preview extension; build_macos_app.sh puts it in the app's PlugIns.
        .executable(name: "RestrictoQuickLook", targets: ["RestrictoQuickLook"]),
    ],
    targets: [
        // C++17 core + layout + C API. No Qt, no GTK, no cairo.
        .target(
            name: "RestrictoCore",
            path: "src",
            exclude: ["gtk", "view/cairo_render.cpp", "view/cairo_render.h"],
            sources: ["core", "view", "capi/restricto_capi.cpp"],
            publicHeadersPath: "capi/include",
            cxxSettings: [.headerSearchPath(".")]
        ),
        // Scene drawing shared by the app and the Quick Look extension.
        .target(
            name: "RestrictoDrawing",
            dependencies: ["RestrictoCore"],
            path: "macos/RestrictoDrawing"
        ),
        .executableTarget(
            name: "Restricto",
            dependencies: ["RestrictoCore", "RestrictoDrawing"],
            path: "macos/Restricto"
        ),
        .executableTarget(
            name: "RestrictoQuickLook",
            dependencies: ["RestrictoCore", "RestrictoDrawing"],
            path: "macos/QuickLook",
            linkerSettings: [.linkedFramework("QuickLookUI")]
        ),
    ],
    cxxLanguageStandard: .cxx17
)
