include("${CMAKE_CURRENT_LIST_DIR}/platform_base.cmake")

function(set_source_files)
    set(oneValueArgs RESULT)
    cmake_parse_arguments(LINUX_SSS "" "${oneValueArgs}" "" ${ARGN})
    if(NOT DEFINED LINUX_SSS_RESULT)
        message(FATAL_ERROR "set_source_files_base needs to be called with RESULT")
    endif()

    set_source_files_base(RESULT BASE_SRC_FILES)
    set(
        ${LINUX_SSS_RESULT}
        ${BASE_SRC_FILES}
        PARENT_SCOPE
    )
endfunction()

function(setup_build)
    target_compile_definitions(
        CppTests PRIVATE
        -DLITECORE_USES_ICU=1
    )
    
    target_include_directories(
        CppTests PRIVATE
        ${TOP}vendor/mbedtls/include
        ${TOP}LiteCore/Unix
    )

    target_link_libraries(
        CppTests PRIVATE
        ${LIBCXX_LIB}
        ${LIBCXXABI_LIB}
        ${ICU_LIBS}
        ${ZLIB_LIB}
        pthread
        dl
    )

    if(NOT DISABLE_LTO_BUILD)
        # When clang enables LTO, it compiles bitcode instead of machine code.  This means
        # that if the final product statically linking in LTO code is not also LTO-enabled
        # the linker will fail because it thinks bitcode is bad machine code
        set_property(TARGET CppTests PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endfunction()
