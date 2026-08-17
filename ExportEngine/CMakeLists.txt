add_library(ExportEngine SHARED
    # Public API
    Json2Tiff.cpp

    # Core
    Core/JsonSceneParser.cpp
    Core/SceneRenderer.cpp
    Core/RenderContext.cpp
    Core/IccProfile.cpp
    Core/ProgressManager.cpp
    Core/Version.cpp
    Core/Log.cpp
    
    # Sink
    Sink/TiffSink.cpp
    Sink/TiffWriter.cpp

    # Sampling
    Sampling/NearestResampler.cpp

    # Color
    Color/ColorConverter.cpp
    Color/ColorTransform.cpp

    # Render
    Render/RectRenderer.cpp
    Render/CircleRenderer.cpp
    Render/PathRenderer.cpp
    Render/ImageRenderer.cpp
    Render/TextureSource.cpp
    Render/TextRenderer.cpp

    # Font
    Font/FontEngine.cpp
)

# Platform-specific font lookup
if(WIN32)
    target_sources(ExportEngine PRIVATE Font/FontEngine_win.cpp)
elseif(APPLE)
    target_sources(ExportEngine PRIVATE Font/FontEngine_mac.mm)
else()
    target_sources(ExportEngine PRIVATE Font/FontEngine_linux.cpp)
endif()

target_include_directories(ExportEngine
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Core>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Color>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Sink>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Sampling>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Render
        ${CMAKE_CURRENT_SOURCE_DIR}/Font
)

target_compile_definitions(ExportEngine PRIVATE
    EXPORTENGINE_EXPORTS
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    _USE_MATH_DEFINES
)

if(WIN32)
    set_target_properties(ExportEngine PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# Reproducible build timestamp
if(NOT DEFINED BUILD_TIMESTAMP)
    string(TIMESTAMP BUILD_TIMESTAMP "%Y-%m-%d %H:%M:%S")
endif()
target_compile_definitions(ExportEngine PRIVATE "BUILD_TIMESTAMP=\"${BUILD_TIMESTAMP}\"")

target_link_libraries(ExportEngine
    PRIVATE
        tiff
        lcms2
        freetype
        cairo
        spdlog_vcpkg
        atcodebase_headers
)

if(APPLE)
    find_library(COREFOUNDATION CoreFoundation)
    find_library(CORETEXT CoreText)
    target_link_libraries(ExportEngine PRIVATE
        ${COREFOUNDATION} ${CORETEXT}
        PkgConfig::vips
    )
elseif(WIN32)
    target_link_libraries(ExportEngine PRIVATE vips)
endif()

# ── Copy vcpkg DLLs alongside the built ExportEngine.dll (Windows) ────
if(WIN32)
    # Direct dependencies
    add_vcpkg_dll_copy(ExportEngine freetype  freetyped)
    add_vcpkg_dll_copy(ExportEngine tiff      tiffd)
    add_vcpkg_dll_copy(ExportEngine fmt       fmtd)
    add_vcpkg_dll_copy(ExportEngine spdlog    spdlogd)
    add_vcpkg_dll_copy_samename(ExportEngine lcms2-2)
    add_vcpkg_dll_copy_samename(ExportEngine cairo-2)
    add_vcpkg_dll_copy_samename(ExportEngine vips-42)

    # vips / glib runtime chain (transitive)
    add_vcpkg_dll_copy_samename(ExportEngine glib-2.0-0)
    add_vcpkg_dll_copy_samename(ExportEngine gobject-2.0-0)
    add_vcpkg_dll_copy_samename(ExportEngine gio-2.0-0)
    add_vcpkg_dll_copy_samename(ExportEngine gmodule-2.0-0)
    add_vcpkg_dll_copy(ExportEngine pcre2-8 pcre2-8d)
    add_vcpkg_dll_copy_samename(ExportEngine ffi-8)
    add_vcpkg_dll_copy_samename(ExportEngine iconv-2)
    add_vcpkg_dll_copy_samename(ExportEngine intl-8)
    add_vcpkg_dll_copy_samename(ExportEngine jpeg62)
    add_vcpkg_dll_copy(ExportEngine libpng16 libpng16d)
    add_vcpkg_dll_copy(ExportEngine libexpat libexpatd)
    add_vcpkg_dll_copy_samename(ExportEngine liblzma)
    add_vcpkg_dll_copy_samename(ExportEngine brotlicommon)
    add_vcpkg_dll_copy_samename(ExportEngine brotlidec)
    add_vcpkg_dll_copy(ExportEngine bz2 bz2d)
    add_vcpkg_dll_copy(ExportEngine z zd)

    # vips → pango chain (text rendering; vips-42.dll imports pango/pangocairo)
    add_vcpkg_dll_copy_samename(ExportEngine pango-1.0-0)
    add_vcpkg_dll_copy_samename(ExportEngine pangocairo-1.0-0)
    add_vcpkg_dll_copy_samename(ExportEngine pangoft2-1.0-0)
    add_vcpkg_dll_copy_samename(ExportEngine pangowin32-1.0-0)
    add_vcpkg_dll_copy_samename(ExportEngine harfbuzz)
    add_vcpkg_dll_copy_samename(ExportEngine fribidi-0)

    # Cairo chain (transitive)
    add_vcpkg_dll_copy_samename(ExportEngine pixman-1-0)
    add_vcpkg_dll_copy_samename(ExportEngine fontconfig-1)
endif()

# ── Install ────────────────────────────────────────────────────────────
install(TARGETS ExportEngine
    EXPORT ExportEngineTargets
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# Public API headers installed to include/
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/Json2Tiff.h
    ${CMAKE_CURRENT_SOURCE_DIR}/ExportEngine.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Version.h
    ${CMAKE_CURRENT_SOURCE_DIR}/Log.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT ExportEngineTargets
    FILE ExportEngineTargets.cmake
    NAMESPACE ExportEngine::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ExportEngine
)
