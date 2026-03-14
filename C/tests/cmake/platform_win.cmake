function(setup_build)
    set(BIN_TOP "${PROJECT_BINARY_DIR}/../..")
    if(NOT LITECORE_PREBUILT_LIB STREQUAL "")
        cmake_path(REMOVE_EXTENSION LITECORE_PREBUILT_LIB OUTPUT_VARIABLE FilesToCopy)
    else()
        set(FilesToCopy ${BIN_TOP}/\$\(Configuration\)/LiteCore)
    endif()

    target_sources(
        C4Tests PRIVATE
        ${TOP}Crypto/mbedUtils.cc
        ${TOP}LiteCore/Support/PlatformIO.cc
        ${TOP}MSVC/vasprintf-msvc.c
        ${TOP}MSVC/asprintf.c
        ${TOP}MSVC/mkdtemp.cc
        ${TOP}MSVC/mkstemp.cc
        ${TOP}MSVC/strlcat.c
        ${TOP}MSVC/asprintf.c
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
endfunction()
