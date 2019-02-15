include("${CMAKE_CURRENT_LIST_DIR}/platform_linux.cmake")

function(setup_globals)
    setup_globals_linux()

    # See: https://github.com/android-ndk/ndk/issues/477
    # The issue is also applicable for other areas like fseeko
    add_definitions(-D_FILE_OFFSET_BITS=32)

    # See: https://github.com/android-ndk/ndk/issues/289
    # Work around an NDK issue that links things like exception handlers in the incorrect order
    set(LITECORE_SHARED_LINKER_FLAGS "${LITECORE_SHARED_LINKER_FLAGS} -Wl,--exclude-libs,libgcc.a" CACHE INTERNAL "")
endfunction()

function(set_litecore_source)
    set(oneValueArgs RESULT)
    cmake_parse_arguments(ANDROID_SSS "" ${oneValueArgs} "" ${ARGN})
    if(NOT DEFINED ANDROID_SSS_RESULT)
        message(FATAL_ERROR set_source_files_base needs to be called with RESULT)
    endif()

    set_litecore_source_linux(RESULT BASE_LITECORE_FILES)
    set(
        ${ANDROID_SSS_RESULT}
        ${BASE_LITECORE_FILES}
        LiteCore/Android/unicode/ndk_icu.c
        PARENT_SCOPE
    )
endfunction()

function(set_support_source)
    set(oneValueArgs RESULT)
    cmake_parse_arguments(ANDROID_SSS "" ${oneValueArgs} "" ${ARGN})
    if(NOT DEFINED ANDROID_SSS_RESULT)
        message(FATAL_ERROR set_source_files_base needs to be called with RESULT)
    endif()

    set_support_source_linux(RESULT BASE_SUPPORT_FILES)
    set(
        ${ANDROID_SSS_RESULT}
        ${BASE_SUPPORT_FILES}
        LiteCore/Support/Error_android.cc
        PARENT_SCOPE
    )
endfunction()

function(setup_litecore_build)
    setup_litecore_build_linux()

    
    target_compile_definitions(
        LiteCoreStatic PRIVATE
        -DLITECORE_USES_ICU=1
    )

    target_include_directories(
        LiteCoreStatic PRIVATE
        LiteCore/Android
    )

    target_link_libraries(
        LiteCore PRIVATE 
        zlibstatic
    )

    target_link_libraries(
        LiteCore PUBLIC
        log
        atomic
    )
endfunction()

function(setup_rest_build)
    # No-op
endfunction()