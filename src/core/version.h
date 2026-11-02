// SPDX-License-Identifier: GPL-3.0-or-later
// The one place the version is set. CMakeLists.txt, the RPM spec, the
// AppStream metainfo and the macOS Info.plist carry the numeric part.
#pragma once

// The commit being built, as "+<short hash>": version_hash.h is written by
// CMake at configure time and by scripts/build_macos_app.sh (not in git).
#if __has_include("version_hash.h")
#include "version_hash.h"
#endif
#ifndef RESTRICTO_GIT_HASH
#define RESTRICTO_GIT_HASH "unknown"
#endif

#define RESTRICTO_VERSION_BASE "0.1.0-alpha"
#define RESTRICTO_VERSION RESTRICTO_VERSION_BASE "+" RESTRICTO_GIT_HASH
