function(setup_build)
    target_include_directories(
        CppTests PRIVATE
        ${TOP}vendor/mbedtls/include
        ${TOP}LiteCore/Unix
    )

    target_compile_options(
        CppTests PRIVATE
        ${EMSCRIPTEN_COMPILE_FLAGS}
    )

    target_link_options(
        CppTests PRIVATE
        ${EMSCRIPTEN_LINK_FLAGS}
        "SHELL:-s EXIT_RUNTIME=1"
        "SHELL:-s PTHREAD_POOL_SIZE=24"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:-s WASM_BIGINT=1"
        "-lnodefs.js"
        "-lnoderawfs.js"
    )
endfunction()
