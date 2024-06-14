function(setup_build)
    target_sources(
        C4Tests PRIVATE
	${TOP}LiteCore/Unix/strlcat.c
    )

    target_link_libraries(
        C4Tests PRIVATE
    )

    target_compile_options(
        C4Tests PRIVATE
        ${EMSCRIPTEN_COMPILE_FLAGS}
    )

    target_link_options(
        C4Tests PRIVATE
        ${EMSCRIPTEN_LINK_FLAGS}
        "SHELL:-s EXIT_RUNTIME=1"
        "SHELL:-s PTHREAD_POOL_SIZE=24"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s WASM_BIGINT=1"
        "-lnodefs.js"
        "-lnoderawfs.js"
    )

    target_include_directories(
        C4Tests PRIVATE
        ${TOP}LiteCore/Unix
    )
endfunction()
