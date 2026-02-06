include(${CMAKE_CURRENT_LIST_DIR}/platform_base.cmake)

macro(check_threading)
    message(STATUS "No threading checks needed for Windows")
endmacro()

function(set_litecore_source)
    set(oneValueArgs RESULT)
    cmake_parse_arguments(WIN_SSS "" ${oneValueArgs} "" ${ARGN})
    if(NOT DEFINED WIN_SSS_RESULT)
        message(FATAL_ERROR set_source_files_base needs to be called with RESULT)
    endif()

    set_litecore_source_base(RESULT BASE_LITECORE_FILES)
    set(
        ${WIN_SSS_RESULT}
        ${BASE_LITECORE_FILES}
        LiteCore/Storage/UnicodeCollator_winapi.cc
        MSVC/mkstemp.cc
        MSVC/mkdtemp.cc
        MSVC/strlcat.c
        MSVC/mbedThreading.cc
        LiteCore/Support/StringUtil_winapi.cc
        Crypto/PublicKey+Windows.cc
        PARENT_SCOPE
    )
endfunction()

function(setup_globals)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    message(STATUS "Configuring mbedTLS for Windows threading model")
    set(_mbedtls_config_py "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../vendor/mbedtls/scripts/config.py")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${_mbedtls_config_py}" unset MBEDTLS_THREADING_PTHREAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.."
    )
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${_mbedtls_config_py}" set MBEDTLS_THREADING_ALT
        WORKING_DIRECTORY "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.."
    )
    file(COPY ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../MSVC/threading_alt.h
         DESTINATION ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../vendor/mbedtls/include/mbedtls)

    add_compile_options(/MP)
    add_link_options(/CGTHREADS:8)
    set(CMAKE_C_FLAGS_MINSIZEREL "/MD /O1 /Ob1 /DNDEBUG /Zi /GL /MP" CACHE INTERNAL "")
    set(CMAKE_CXX_FLAGS_MINSIZEREL "/MD /O1 /Ob1 /DNDEBUG /Zi /GL /MP" CACHE INTERNAL "")
    set(CMAKE_SHARED_LINKER_FLAGS_MINSIZEREL "/INCREMENTAL:NO /LTCG:incremental /debug" CACHE INTERNAL "")
    set(CMAKE_EXE_LINKER_FLAGS_MINSIZEREL "/INCREMENTAL:NO /LTCG:incremental /debug" CACHE INTERNAL "")
    set(CMAKE_STATIC_LINKER_FLAGS_MINSIZEREL "/LTCG:incremental" CACHE INTERNAL "")

    # Disable the following warnings:
    #   4099 (library linked without debug info)
    #   4221 (object file with no new public symbols)
    string(
        CONCAT LITECORE_SHARED_LINKER_FLAGS
        "/ignore:4099 "
        "/ignore:4221"
    )
    set(LITECORE_SHARED_LINKER_FLAGS ${LITECORE_SHARED_LINKER_FLAGS} CACHE INTERNAL "")

    set(
        LITECORE_STATIC_LINKER_FLAGS
        "/ignore:4221"
        CACHE INTERNAL ""
    )
endfunction()

function(setup_litecore_build_win)
    foreach(liteCoreVariant LiteCoreObjects LiteCoreUnitTesting)
        target_compile_definitions(
            ${liteCoreVariant} PRIVATE
            -DUNICODE               # Use wide string variants for Win32 calls
            -D_UNICODE              # Ditto
            -D_USE_MATH_DEFINES     # Define math constants like PI
            -DWIN32                 # Identify as WIN32
            -DNOMINMAX              # Disable min/max macros (they interfere with std::min and max)
            PUBLIC
            -DLITECORE_EXPORTS      # Export functions marked CBL_CORE_API, etc
            -DPERSISTENT_PRIVATE_KEY_AVAILABLE
        )

        target_include_directories(
            ${liteCoreVariant} PUBLIC
            MSVC
            vendor/fleece/MSVC
        )
    endforeach()

    if(LITECORE_BUILD_SHARED)
        target_include_directories(
            LiteCore PRIVATE
            vendor/fleece/API
            vendor/fleece/Fleece/Support
        )
    endif()

    # Link with subproject libz and Windows sockets lib
    foreach(liteCoreVariant LiteCoreObjects LiteCoreUnitTesting)
        target_link_libraries(
           ${liteCoreVariant} INTERFACE
           zlibstatic
           Ws2_32
           ncrypt
           crypt32
        )
    endforeach()

    # Compile string literals as UTF-8,
    # Enable exception handling for C++ but disable for extern C
    # Disable the following warnings:
    #   4068 (unrecognized pragma)
    #   4244 (converting float to integer)
    #   4018 (signed / unsigned mismatch)
    #   4819 (character that cannot be represented in current code page)
    #   4800 (value forced to bool)
    #   5105 ("macro expansion producing 'defined' has undefined behavior")
    # Disable warning about "insecure" C runtime functions (strcpy vs strcpy_s)
    foreach(target ${LITECORE_TARGETS})
        target_compile_options(
            ${target} PRIVATE
            "/utf-8"
            "/wd4068;/wd4244;/wd4018;/wd4819;/wd4800;/wd5105"
            "-D_CRT_SECURE_NO_WARNINGS=1"
            "/guard:cf"
            "$<$<COMPILE_LANGUAGE:CXX>:/EHsc>"
        )
    endforeach()

    # Suppress zlib errors
    target_compile_options(
        zlibstatic PRIVATE
        "/wd4267" # loss of data due to size narrowing
    )

    # Windows always has socklen_t
    target_compile_definitions(
        mbedtls PRIVATE
        _SOCKLEN_T_DECLARED
    )

    target_compile_definitions(
        LiteCoreWebSocket PRIVATE
        -DNOMINMAX              # Disable min/max macros (they interfere with std::min and max)
    )

    target_include_directories(LiteCoreWebSocket PRIVATE MSVC)

    if(LITECORE_BUILD_SHARED)
        install(FILES $<TARGET_PDB_FILE:LiteCore> DESTINATION bin OPTIONAL)
    endif()
endfunction()

function(setup_rest_build)
    target_include_directories(LiteCoreREST_Objects PUBLIC ../MSVC)
endfunction()
