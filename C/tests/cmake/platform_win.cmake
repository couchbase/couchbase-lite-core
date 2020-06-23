function(setup_build)
    set(BIN_TOP "${PROJECT_BINARY_DIR}/../..")
    set(FilesToCopy ${BIN_TOP}/\$\(Configuration\)/LiteCore)

    target_sources(
        C4Tests PRIVATE
        ${TOP}Crypto/mbedUtils.cc
        ${TOP}LiteCore/Support/Error_windows.cc
        ${TOP}LiteCore/Support/PlatformIO.cc
        ${TOP}MSVC/vasprintf-msvc.c
        ${TOP}MSVC/asprintf.c
        ${TOP}MSVC/arc4random.cc
        ${TOP}MSVC/mkdtemp.cc
        ${TOP}MSVC/mkstemp.cc
        ${TOP}MSVC/strlcat.c
        ${TOP}MSVC/asprintf.c
        ${TOP}vendor/fleece/Fleece/Support/slice.cc
        ${TOP}vendor/fleece/MSVC/memmem.cc
    )

    target_compile_definitions(
        C4Tests PRIVATE
        -DNOMINMAX   # Disable min/max macros (they interfere with std::min and max)
    )

    target_include_directories(
        C4Tests PRIVATE
        ${TOP}MSVC
        ${TOP}vendor/fleece/MSVC
    )

    target_link_libraries(
        C4Tests PRIVATE
        mbedcrypto
        mbedx509
    )

    target_include_directories(
        LiteCoreWebSocket PRIVATE
        ${TOP}MSVC
    )
    
    add_custom_command(
        TARGET C4Tests POST_BUILD
        COMMAND ${CMAKE_COMMAND}
        -DFilesToCopy="${FilesToCopy}"
        -DDestinationDirectory=${PROJECT_BINARY_DIR}/\$\(Configuration\)
        -P ${TOP}MSVC/copy_artifacts.cmake
    )
endfunction()