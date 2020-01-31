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
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        # Optimizer bug...causes infinite loops on basic for loops
        add_definitions(-fno-aggressive-loop-optimizations)
    endif()

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

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^armv[67]")
        target_link_libraries(
            CppTests PRIVATE
            atomic
        )
    endif()

    if(NOT DISABLE_LTO_BUILD AND
       NOT ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug" OR "${CMAKE_BUILD_TYPE}" STREQUAL ""))
        # When clang enables LTO, it compiles bitcode instead of machine code.  This means
        # that if the final product statically linking in LTO code is not also LTO-enabled
        # the linker will fail because it thinks bitcode is bad machine code.  Furthermore, 
        # there is a bug in the LLVM plugin for LTO in 3.9.1 that causes invalid linker flags
        # when combined with -Oz optimization
        #
        # Furthermore, who knows what goes on in the mind of GNU...so just forget about it
        if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION GREATER "3.9.1")    
            set_property(TARGET CppTests PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        else()
            message("Disabling LTO for CppTests to work around LLVM 3.9.1 issue")
        endif()
    endif()
endfunction()
