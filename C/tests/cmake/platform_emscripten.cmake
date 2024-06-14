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

    target_link_options(
        C4Tests PRIVATE
        "-pthread"
        "-fwasm-exceptions"
        "-lembind"
        "SHELL:-s EXIT_RUNTIME=1"
        "SHELL:-s PTHREAD_POOL_SIZE=24"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s DEMANGLE_SUPPORT=1"
        "SHELL:-s WASM_BIGINT=1"
        "-lnodefs.js"
        "-lnoderawfs.js"
    )

    target_include_directories(
        C4Tests PRIVATE
        ${TOP}LiteCore/Unix
    )
endfunction()
