cmake_minimum_required(VERSION 3.16)
project(OpenClone VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Frontend: GTK4 + libadwaita on Linux, AppKit on macOS. Override with
# -DOPENCLONE_FRONTEND=gtk|appkit|none.
if(APPLE)
    set(_default_frontend appkit)
else()
    set(_default_frontend gtk)
endif()
set(OPENCLONE_FRONTEND ${_default_frontend} CACHE STRING "gtk, appkit or none")

# ---- core: plain C++17, no dependencies ---------------------------------------
add_library(openclone_core STATIC
    src/core/sequence.cpp
    src/core/edit.cpp
    src/core/digest.cpp
    src/core/primercheck.cpp
    src/core/cloning.cpp
    src/core/abif.cpp
    src/core/align.cpp
    src/core/genbank.cpp
    src/core/fasta.cpp
    src/core/xml.cpp
    src/core/snapgene.cpp
    src/core/enzymes.cpp
    src/core/tm.cpp
    src/core/seqio.cpp)
target_include_directories(openclone_core PUBLIC src)

# ---- view: layout of maps and the sequence view, still plain C++ -------------
add_library(openclone_view STATIC
    src/view/scene.cpp
    src/view/style.cpp
    src/view/maplayout.cpp
    src/view/seqlayout.cpp
    src/view/gellayout.cpp
    src/view/document.cpp)
target_link_libraries(openclone_view PUBLIC openclone_core)

# ---- C API: what the Swift/AppKit app calls --------------------------------------
add_library(openclone_capi STATIC src/capi/openclone_capi.cpp)
target_include_directories(openclone_capi PUBLIC src/capi/include)
target_link_libraries(openclone_capi PUBLIC openclone_view)

# ---- tests ---------------------------------------------------------------------
enable_testing()
add_executable(core_tests tests/test_core.cpp)
target_link_libraries(core_tests PRIVATE openclone_core)
add_test(NAME core_tests COMMAND core_tests ${CMAKE_CURRENT_SOURCE_DIR}/samples)

add_executable(view_tests tests/test_view.cpp)
target_link_libraries(view_tests PRIVATE openclone_view)
add_test(NAME view_tests COMMAND view_tests ${CMAKE_CURRENT_SOURCE_DIR}/samples)

add_executable(capi_tests tests/test_capi.c)
target_link_libraries(capi_tests PRIVATE openclone_capi)
set_target_properties(capi_tests PROPERTIES LINKER_LANGUAGE CXX C_STANDARD 99)
add_test(NAME capi_tests COMMAND capi_tests ${CMAKE_CURRENT_SOURCE_DIR}/samples)

# ---- frontends -----------------------------------------------------------------
if(OPENCLONE_FRONTEND STREQUAL "gtk")
    add_subdirectory(src/gtk)
elseif(OPENCLONE_FRONTEND STREQUAL "appkit")
    # The Swift/AppKit app is built with SwiftPM (Package.swift at the repo
    # root); see README. CMake still builds and tests the core here.
    message(STATUS "macOS app: run scripts/build_macos_app.sh")
endif()
