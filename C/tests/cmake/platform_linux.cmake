function(setup_build)
    target_sources(
        C4Tests PRIVATE
        ${TOP}Crypto/mbedUtils.cc
        ${TOP}LiteCore/Unix/strlcat.c
    )

    target_link_libraries(
        C4Tests PRIVATE
        mbedcrypto
        mbedx509
        Threads::Threads
    )

    target_include_directories(
        C4Tests PRIVATE
        ${TOP}LiteCore/Unix
    )

    if(NOT LITECORE_PREBUILT_LIB STREQUAL "")
        # Very likely that if using prebuilt LiteCore that the versions of libstdc++ differ
        # so use a static libstdc++ to fill in the gaps
        target_link_libraries(
            C4Tests PRIVATE
            -static-libstdc++
        )
    endif()
endfunction()
