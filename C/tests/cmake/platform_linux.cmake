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
    )

    target_include_directories(
        C4Tests PRIVATE
        ${TOP}LiteCore/Unix
    )
endfunction()
