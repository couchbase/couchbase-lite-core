function(setup_build)
    set(BIN_TOP "${PROJECT_BINARY_DIR}/../..")
    target_include_directories(
        CppTests PRIVATE
        ${TOP}MSVC
    )

    target_compile_definitions(
        CppTests PRIVATE
        -DLITECORE_USES_ICU=1
    )
endfunction()