cmake_minimum_required(VERSION 3.16)
project(ExportEngine VERSION 1.0.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Install support ───────────────────────────────────────────────────
include(GNUInstallDirs)
set(CMAKE_INSTALL_PREFIX "${CMAKE_SOURCE_DIR}/install" CACHE PATH "Install prefix" FORCE)

if(MSVC)
    add_compile_options(/utf-8 /wd4819)
endif()

# ── Output directories ──────────────────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/$<CONFIG>)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY  ${CMAKE_SOURCE_DIR}/bin/$<CONFIG>)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY  ${CMAKE_SOURCE_DIR}/bin/$<CONFIG>)

# ═══════════════════════════════════════════════════════════════════════
#  Third-party dependencies
# ═══════════════════════════════════════════════════════════════════════

if(APPLE)
    # ── macOS: find dependencies from Homebrew ────────────────────────
    execute_process(
        COMMAND brew --prefix
        OUTPUT_VARIABLE BREW_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT BREW_PREFIX)
        set(BREW_PREFIX "/opt/homebrew")
    endif()
    message(STATUS "Homebrew prefix: ${BREW_PREFIX}")
    list(APPEND CMAKE_PREFIX_PATH "${BREW_PREFIX}")

    # For sub-project include-path compatibility (e.g. ${VCPKG_DIR}/include)
    set(VCPKG_DIR "${BREW_PREFIX}")

    # freetype
    find_package(Freetype REQUIRED)
    add_library(freetype ALIAS Freetype::Freetype)

    # lcms2 (no CMake config provided; use find_library/find_path)
    find_library(LCMS2_LIBRARY lcms2 PATHS "${BREW_PREFIX}/lib" REQUIRED)
    find_path(LCMS2_INCLUDE_DIR lcms2.h PATHS "${BREW_PREFIX}/include" REQUIRED)
    add_library(lcms2 SHARED IMPORTED)
    set_target_properties(lcms2 PROPERTIES
        IMPORTED_LOCATION "${LCMS2_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LCMS2_INCLUDE_DIR}"
    )

    # TIFF
    find_package(TIFF REQUIRED)
    add_library(tiff ALIAS TIFF::TIFF)

    # fmt
    find_package(fmt REQUIRED)
    add_library(fmt_vcpkg ALIAS fmt::fmt)

    # spdlog
    find_package(spdlog REQUIRED)
    add_library(spdlog_vcpkg ALIAS spdlog::spdlog)

    # libvips — universal image reading, color conversion, resampling
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(vips REQUIRED IMPORTED_TARGET vips)

    # Cairo — vector graphics rendering (replaces AGG)
    find_library(CAIRO_LIBRARY cairo PATHS "${BREW_PREFIX}/lib" REQUIRED)
    find_path(CAIRO_INCLUDE_DIR cairo.h PATHS "${BREW_PREFIX}/include/cairo" "${BREW_PREFIX}/include" REQUIRED)
    add_library(cairo SHARED IMPORTED)
    set_target_properties(cairo PROPERTIES
        IMPORTED_LOCATION "${CAIRO_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${CAIRO_INCLUDE_DIR};${BREW_PREFIX}/include"
    )

elseif(WIN32)
    # ── Windows: vcpkg IMPORTED shared libraries ──────────────────────
    set(VCPKG_DIR "D:/WorkSpace/vcpkg/installed/x64-windows")

    # freetype — debug: freetyped.dll / freetyped.lib
    add_library(freetype SHARED IMPORTED)
    set_target_properties(freetype PROPERTIES
        IMPORTED_LOCATION_DEBUG   "${VCPKG_DIR}/debug/bin/freetyped.dll"
        IMPORTED_IMPLIB_DEBUG     "${VCPKG_DIR}/debug/lib/freetyped.lib"
        IMPORTED_LOCATION_RELEASE "${VCPKG_DIR}/bin/freetype.dll"
        IMPORTED_IMPLIB_RELEASE   "${VCPKG_DIR}/lib/freetype.lib"
    )
    target_include_directories(freetype INTERFACE "${VCPKG_DIR}/include")

    # lcms2 — same DLL / .lib name for both configs, but different dirs
    add_library(lcms2 SHARED IMPORTED)
    set_target_properties(lcms2 PROPERTIES
        IMPORTED_LOCATION_DEBUG   "${VCPKG_DIR}/debug/bin/lcms2-2.dll"
        IMPORTED_IMPLIB_DEBUG     "${VCPKG_DIR}/debug/lib/lcms2.lib"
        IMPORTED_LOCATION_RELEASE "${VCPKG_DIR}/bin/lcms2-2.dll"
        IMPORTED_IMPLIB_RELEASE   "${VCPKG_DIR}/lib/lcms2.lib"
    )
    target_include_directories(lcms2 INTERFACE "${VCPKG_DIR}/include")

    # TIFF — debug: tiffd.dll / tiffd.lib
    add_library(tiff SHARED IMPORTED)
    set_target_properties(tiff PROPERTIES
        IMPORTED_LOCATION_DEBUG   "${VCPKG_DIR}/debug/bin/tiffd.dll"
        IMPORTED_IMPLIB_DEBUG     "${VCPKG_DIR}/debug/lib/tiffd.lib"
        IMPORTED_LOCATION_RELEASE "${VCPKG_DIR}/bin/tiff.dll"
        IMPORTED_IMPLIB_RELEASE   "${VCPKG_DIR}/lib/tiff.lib"
    )
    target_include_directories(tiff INTERFACE "${VCPKG_DIR}/include")

    # fmt — required by spdlog in compiled-lib mode
    add_library(fmt_vcpkg SHARED IMPORTED)
    set_target_properties(fmt_vcpkg PROPERTIES
        IMPORTED_LOCATION_DEBUG   "${VCPKG_DIR}/debug/bin/fmtd.dll"
        IMPORTED_IMPLIB_DEBUG     "${VCPKG_DIR}/debug/lib/fmtd.lib"
        IMPORTED_LOCATION_RELEASE "${VCPKG_DIR}/bin/fmt.dll"
        IMPORTED_IMPLIB_RELEASE   "${VCPKG_DIR}/lib/fmt.lib"
    )
    target_include_directories(fmt_vcpkg INTERFACE "${VCPKG_DIR}/include")

    # spdlog — compiled shared lib, not header-only
    add_library(spdlog_vcpkg SHARED IMPORTED)
    set_target_properties(spdlog_vcpkg PROPERTIES
        IMPORTED_LOCATION_DEBUG   "${VCPKG_DIR}/debug/bin/spdlogd.dll"
        IMPORTED_IMPLIB_DEBUG     "${VCPKG_DIR}/debug/lib/spdlogd.lib"
        IMPORTED_LOCATION_RELEASE "${VCPKG_DIR}/bin/spdlog.dll"
        IMPORTED_IMPLIB_RELEASE   "${VCPKG_DIR}/lib/spdlog.lib"
    )
    target_include_directories(spdlog_vcpkg INTERFACE "${VCPKG_DIR}/include")
    target_compile_definitions(spdlog_vcpkg INTERFACE
        SPDLOG_SHARED_LIB SPDLOG_COMPILED_LIB SPDLOG_FMT_EXTERNAL)
    target_link_libraries(spdlog_vcpkg INTERFACE fmt_vcpkg)

    # libvips — universal image reading, color conversion, resampling
    # vips requires glib/gobject/gio which are transitively provided
    find_library(GLIB2_LIB     glib-2.0    PATHS "${VCPKG_DIR}/lib" NO_DEFAULT_PATH)
    find_library(GOBJECT2_LIB  gobject-2.0 PATHS "${VCPKG_DIR}/lib" NO_DEFAULT_PATH)
    add_library(vips SHARED IMPORTED)
    set_target_properties(vips PROPERTIES
        IMPORTED_LOCATION_DEBUG   "${VCPKG_DIR}/debug/bin/vips-42.dll"
        IMPORTED_IMPLIB_DEBUG     "${VCPKG_DIR}/debug/lib/vips.lib"
        IMPORTED_LOCATION_RELEASE "${VCPKG_DIR}/bin/vips-42.dll"
        IMPORTED_IMPLIB_RELEASE   "${VCPKG_DIR}/lib/vips.lib"
    )
    target_include_directories(vips INTERFACE
        "${VCPKG_DIR}/include"
        "${VCPKG_DIR}/include/glib-2.0"
        "${VCPKG_DIR}/lib/glib-2.0/include"
    )
    target_link_libraries(vips INTERFACE ${GLIB2_LIB} ${GOBJECT2_LIB})

    # Cairo — vector graphics rendering (replaces AGG)
    find_path(CAIRO_INCLUDE_DIR cairo.h
        PATHS "${VCPKG_DIR}/include/cairo" NO_DEFAULT_PATH REQUIRED)
    add_library(cairo SHARED IMPORTED)
    set_target_properties(cairo PROPERTIES
        IMPORTED_LOCATION_DEBUG   "${VCPKG_DIR}/debug/bin/cairo-2.dll"
        IMPORTED_IMPLIB_DEBUG     "${VCPKG_DIR}/debug/lib/cairo.lib"
        IMPORTED_LOCATION_RELEASE "${VCPKG_DIR}/bin/cairo-2.dll"
        IMPORTED_IMPLIB_RELEASE   "${VCPKG_DIR}/lib/cairo.lib"
    )
    target_include_directories(cairo INTERFACE
        ${CAIRO_INCLUDE_DIR}
        "${VCPKG_DIR}/include"              # fontconfig.h, ft2build.h, lcms2.h …
        "${VCPKG_DIR}/include/fontconfig"
        "${VCPKG_DIR}/include/pixman-1"
        "${VCPKG_DIR}/include/libpng16")
    # Cairo's transitive link-time deps
    find_library(PIXMAN_LIB     pixman-1   PATHS "${VCPKG_DIR}/lib" NO_DEFAULT_PATH)
    find_library(FONTCONFIG_LIB fontconfig PATHS "${VCPKG_DIR}/lib" NO_DEFAULT_PATH)
    find_library(LIBPNG_LIB     libpng16   PATHS "${VCPKG_DIR}/lib" NO_DEFAULT_PATH)
    find_library(Z_LIB          z          PATHS "${VCPKG_DIR}/lib" NO_DEFAULT_PATH)
    target_link_libraries(cairo INTERFACE
        ${PIXMAN_LIB} ${FONTCONFIG_LIB} ${LIBPNG_LIB} ${Z_LIB})

else()
    message(FATAL_ERROR "Unsupported platform. Use macOS (Homebrew) or Windows (vcpkg).")
endif()

# ═══════════════════════════════════════════════════════════════════════
#  ATCodebase / header-only libs
# ═══════════════════════════════════════════════════════════════════════
add_library(atcodebase_headers INTERFACE)
if(APPLE)
    # nlohmann-json from Homebrew
    target_include_directories(atcodebase_headers INTERFACE
        "${BREW_PREFIX}/include"                       # nlohmann/json.hpp
    )
else()
    # Windows: nlohmann/json.hpp from vcpkg
    target_include_directories(atcodebase_headers INTERFACE
        "${VCPKG_DIR}/include"          # nlohmann/json.hpp
    )
endif()

# ═══════════════════════════════════════════════════════════════════════
#  Post-build: copy vcpkg DLLs to output directory  (Windows only)
# ═══════════════════════════════════════════════════════════════════════
if(WIN32)
    function(add_vcpkg_dll_copy TARGET DLL_BASE DEBUG_DLL_BASE)
        add_custom_command(TARGET ${TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<IF:$<CONFIG:Debug>,${VCPKG_DIR}/debug/bin/${DEBUG_DLL_BASE}.dll,${VCPKG_DIR}/bin/${DLL_BASE}.dll>"
                "$<TARGET_FILE_DIR:${TARGET}>"
            COMMENT "Copying ${DLL_BASE}.dll → output"
        )
    endfunction()

    function(add_vcpkg_dll_copy_samename TARGET DLL_BASE)
        add_custom_command(TARGET ${TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<IF:$<CONFIG:Debug>,${VCPKG_DIR}/debug/bin/${DLL_BASE}.dll,${VCPKG_DIR}/bin/${DLL_BASE}.dll>"
                "$<TARGET_FILE_DIR:${TARGET}>"
            COMMENT "Copying ${DLL_BASE}.dll → output"
        )
    endfunction()
endif()

# ── Testing ─────────────────────────────────────────────────────────
enable_testing()

# ── CMake package config ─────────────────────────────────────────────
include(CMakePackageConfigHelpers)
configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/ExportEngineConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ExportEngineConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ExportEngine
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ExportEngineConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/ExportEngineConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/ExportEngineConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ExportEngine
)

# ── Sub-projects ────────────────────────────────────────────────────
add_subdirectory(ExportEngine)
add_subdirectory(App/EETool)
add_subdirectory(App/EEVerify)
