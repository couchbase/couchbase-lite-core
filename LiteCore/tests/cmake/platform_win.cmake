function(setup_build)
    set(BIN_TOP "${PROJECT_BINARY_DIR}/../..")
    target_include_directories(
        CppTests PRIVATE
        ${TOP}MSVC
    )
endfunction()