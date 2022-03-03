include("${CMAKE_CURRENT_LIST_DIR}/platform_unix.cmake")

macro(check_threading)
    message(STATUS "No threading checks needed for Emscripten")
endmacro()

function(setup_globals)
    setup_globals_unix()

    set(EMSCRIPTEN_COMPILE_FLAGS "-pthread -fwasm-exceptions")

    set(LITECORE_C_FLAGS ${EMSCRIPTEN_COMPILE_FLAGS} CACHE INTERNAL "")
    set(LITECORE_CXX_FLAGS ${EMSCRIPTEN_COMPILE_FLAGS} CACHE INTERNAL "")
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

    target_link_libraries(
        LiteCoreStatic INTERFACE
        zlibstatic
    )
endfunction()

function(setup_rest_build)
    setup_rest_build_unix()
endfunction()
