add_executable(EEVerify
    main.cpp
    Validator.cpp
)

target_include_directories(EEVerify
    PRIVATE
        ${CMAKE_SOURCE_DIR}/ExportEngine/Core
        ${VCPKG_DIR}/include          # spdlog
)

target_compile_definitions(EEVerify PRIVATE
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(EEVerify
    PRIVATE
        ExportEngine
        atcodebase_headers            # nlohmann/json.hpp
)

# -- Test executable --------------------------------------------------------
# Compiles Validator.cpp directly -- no ExportEngine.dll dependency needed
add_executable(test_validator
    tests/test_validator.cpp
    Validator.cpp
)

target_include_directories(test_validator
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(test_validator PRIVATE atcodebase_headers)

target_compile_definitions(test_validator PRIVATE
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)

add_test(NAME EEVerifyTests COMMAND test_validator)

# -- Error-code / core-lib tests --------------------------------------------
# Links ExportEngine.dll (like EETool) — exercises the migrated interfaces.
add_executable(test_errors
    tests/test_errors.cpp
)

target_include_directories(test_errors
    PRIVATE
        ${CMAKE_SOURCE_DIR}/ExportEngine
        ${CMAKE_SOURCE_DIR}/ExportEngine/Core   # TiffWriter.h, ...
        ${CMAKE_SOURCE_DIR}/ExportEngine/Color  # ColorConverter.h
        ${CMAKE_SOURCE_DIR}/ExportEngine/Font    # FontEngine.h
        ${CMAKE_SOURCE_DIR}/ExportEngine/Render  # TextureSource.h
        ${CMAKE_SOURCE_DIR}/ExportEngine/Sink    # MemorySink.h
        ${VCPKG_DIR}/include          # spdlog
)

target_link_libraries(test_errors
    PRIVATE
        ExportEngine
        spdlog_vcpkg                  # Log.h
        vips                          # vips_init / vips_pngsave（fixture PNG）
        tiff                          # TIFFOpen 直读 XResolution/YResolution tag
)

target_compile_definitions(test_errors PRIVATE
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
)

add_test(NAME ErrorCodeTests COMMAND test_errors)

install(TARGETS EEVerify
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
