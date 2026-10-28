find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.12)
pkg_check_modules(ADW REQUIRED IMPORTED_TARGET libadwaita-1>=1.5)
pkg_check_modules(PANGOCAIRO REQUIRED IMPORTED_TARGET pangocairo cairo)

# cairo/Pango drawing of the shared scenes (also used for map export).
add_library(restricto_cairo STATIC ${PROJECT_SOURCE_DIR}/src/view/cairo_render.cpp)
target_link_libraries(restricto_cairo PUBLIC restricto_view PkgConfig::PANGOCAIRO)

add_executable(restricto
    main.cpp
    window.cpp
    dialogs.cpp
    digestdialog.cpp
    mapview.cpp
    seqview.cpp)
target_include_directories(restricto PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(restricto PRIVATE restricto_cairo PkgConfig::GTK4 PkgConfig::ADW)
# GLib's C macros trip -Wpedantic in C++.
target_compile_options(restricto PRIVATE -Wno-pedantic -Wno-missing-field-initializers)

include(GNUInstallDirs)
install(TARGETS restricto RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${PROJECT_SOURCE_DIR}/packaging/linux/io.github.mrgarbett.Restricto.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${PROJECT_SOURCE_DIR}/packaging/linux/io.github.mrgarbett.Restricto.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/mime/packages)
install(FILES ${PROJECT_SOURCE_DIR}/packaging/icons/io.github.mrgarbett.Restricto.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES ${PROJECT_SOURCE_DIR}/packaging/linux/io.github.mrgarbett.Restricto.metainfo.xml
        DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)

# Headless map render of the demo plasmid as a smoke test (no display needed).
add_test(NAME render_map_smoke
         COMMAND restricto --render-map ${CMAKE_BINARY_DIR}/demo-map.png --enzymes all
                 ${PROJECT_SOURCE_DIR}/samples/pOC-demo.dna)
