// SPDX-License-Identifier: GPL-3.0-or-later
// An app extension's entry point is Foundation's NSExtensionMain, which then
// loads the principal class named in the extension's Info.plist
// (packaging/macos/QuickLook-Info.plist). It has no Swift declaration, so
// look it up at run time.
import Foundation

typealias ExtensionMain = @convention(c) (Int32, UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>) -> Int32
guard let symbol = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "NSExtensionMain") else {
    FileHandle.standardError.write("RestrictoQuickLook: NSExtensionMain not found\n".data(using: .utf8)!)
    exit(1)
}
exit(unsafeBitCast(symbol, to: ExtensionMain.self)(CommandLine.argc, CommandLine.unsafeArgv))
