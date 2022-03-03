function(setup_build)
    target_include_directories(
        CppTests PRIVATE
        ${TOP}vendor/mbedtls/include
        ${TOP}LiteCore/Unix
    )

    target_link_options(
        CppTests PRIVATE
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
endfunction()
