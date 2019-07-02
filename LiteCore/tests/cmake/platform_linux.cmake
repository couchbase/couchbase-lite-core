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
    target_include_directories(
        CppTests PRIVATE
        ${TOP}vendor/mbedtls/include
        ${TOP}LiteCore/Unix
    )

    target_link_libraries(
        CppTests PRIVATE
        ${LIBCXX_LIB}
        ${LIBCXXABI_LIB}
        ${ICU4C_COMMON}
        ${ICU4C_I18N}
        z
        pthread
        dl
    )

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^armv[67]")
        target_link_libraries(
            CppTests PRIVATE
            atomic
        )
    endif()
endfunction()
