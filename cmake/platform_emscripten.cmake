include("${CMAKE_CURRENT_LIST_DIR}/platform_unix.cmake")

macro(check_threading)
    message(STATUS "No threading checks needed for Emscripten")
endmacro()

set(EMSCRIPTEN_COMPILE_FLAGS "-pthread" "-fwasm-exceptions" "-Os")
set(EMSCRIPTEN_LINK_FLAGS    "-pthread" "-fwasm-exceptions" "-lembind")

function(setup_globals)
    setup_globals_unix()

    #if(NOT DISABLE_LTO_BUILD)
        set(EMSCRIPTEN_COMPILE_FLAGS ${EMSCRIPTEN_COMPILE_FLAGS} "-flto" PARENT_SCOPE)
        set(EMSCRIPTEN_LINK_FLAGS    ${EMSCRIPTEN_LINK_FLAGS}    "-flto" PARENT_SCOPE)
    #endif()
endfunction()

function(set_litecore_source)
    set(oneValueArgs RESULT)
    cmake_parse_arguments(EMSCRIPTEN_SSS "" ${oneValueArgs} "" ${ARGN})
    if(NOT DEFINED EMSCRIPTEN_SSS_RESULT)
        message(FATAL_ERROR set_source_files_base needs to be called with RESULT)
    endif()

    set_litecore_source_base(RESULT BASE_LITECORE_FILES)
    set(
        ${EMSCRIPTEN_SSS_RESULT}
        ${BASE_LITECORE_FILES}
        LiteCore/Storage/UnicodeCollator_JS.cc
        LiteCore/Support/StringUtil_JS.cc
        PARENT_SCOPE
    )
endfunction()

function(setup_litecore_build)
    setup_litecore_build_unix()

    target_include_directories(
        LiteCoreStatic PRIVATE
        LiteCore/Unix
    )

    message(STATUS "Emscripten flags are ${EMSCRIPTEN_COMPILE_FLAGS}")
    foreach(platform LiteCoreObjects LiteCoreUnitTesting BLIPObjects CouchbaseSqlite3 SQLite3_UnicodeSN)
        target_compile_options(
            ${platform} PRIVATE
            ${EMSCRIPTEN_COMPILE_FLAGS}
        )
    endforeach()

    target_link_libraries(
        LiteCoreStatic INTERFACE
        zlibstatic
    )
endfunction()

function(setup_rest_build)
    setup_rest_build_unix()
endfunction()
