add_executable(EETool
    main.cpp
)

target_include_directories(EETool
    PRIVATE
        ${CMAKE_SOURCE_DIR}/ExportEngine
        ${VCPKG_DIR}/include          # spdlog
)

target_compile_definitions(EETool PRIVATE
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(EETool
    PRIVATE
        ExportEngine
        spdlog_vcpkg   # for SPDLOG_COMPILED_LIB defines + fmt import
)

install(TARGETS EETool
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
