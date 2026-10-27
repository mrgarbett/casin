// swift-tools-version:5.9
// The macOS app: Swift + AppKit over the shared C++ core (through its C API).
// Build with `swift build -c release` or open this file in Xcode; wrap it into
// OpenClone.app with scripts/build_macos_app.sh.
import PackageDescription

let package = Package(
    name: "OpenClone",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "OpenClone", targets: ["OpenClone"]),
    ],
    targets: [
        // C++17 core + layout + C API. No Qt, no GTK, no cairo.
        .target(
            name: "OpenCloneCore",
            path: "src",
            exclude: ["gtk", "view/cairo_render.cpp", "view/cairo_render.h"],
            sources: ["core", "view", "capi/openclone_capi.cpp"],
            publicHeadersPath: "capi/include",
            cxxSettings: [.headerSearchPath(".")]
        ),
        .executableTarget(
            name: "OpenClone",
            dependencies: ["OpenCloneCore"],
            path: "macos/OpenClone"
        ),
    ],
    cxxLanguageStandard: .cxx17
)
