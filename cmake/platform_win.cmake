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
        LiteCore/Support/StringUtil_winapi.cc
        Crypto/PublicKey+Windows.cc
        PARENT_SCOPE
    )
endfunction()

function(setup_globals)
    set(CMAKE_C_FLAGS_MINSIZEREL "/MD /O1 /Ob1 /DNDEBUG /Zi /GL" CACHE INTERNAL "")
    set(CMAKE_CXX_FLAGS_MINSIZEREL "/MD /O1 /Ob1 /DNDEBUG /Zi /GL" CACHE INTERNAL "")
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
    target_compile_definitions(
        LiteCoreObjects PRIVATE
        -DUNICODE               # Use wide string variants for Win32 calls
        -D_UNICODE              # Ditto
        -D_USE_MATH_DEFINES     # Define math constants like PI
        -DWIN32                 # Identify as WIN32
        -DNOMINMAX              # Disable min/max macros (they interfere with std::min and max)
    )

    target_compile_definitions(
        LiteCoreObjects PUBLIC
        -DLITECORE_EXPORTS      # Export functions marked CBL_CORE_API, etc 
        -DPERSISTENT_PRIVATE_KEY_AVAILABLE
    )

    target_include_directories(
        LiteCoreObjects PRIVATE  
        MSVC
        vendor/fleece/MSVC
    )

    target_include_directories(
        LiteCore PRIVATE
        vendor/fleece/API
        vendor/fleece/Fleece/Support
    )

    # Link with subproject libz and Windows sockets lib
    target_link_libraries(
        LiteCoreObjects INTERFACE
        zlibstatic
        Ws2_32
        ncrypt
        crypt32
    )

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

    install(FILES $<TARGET_PDB_FILE:LiteCore> DESTINATION bin OPTIONAL)
endfunction()

function(setup_support_build)
    target_include_directories(Support PRIVATE MSVC)
    target_include_directories(Support PRIVATE vendor/fleece/MSVC)
endfunction()

function(setup_rest_build)
    target_include_directories(LiteCoreREST_Objects PUBLIC ../MSVC)
endfunction()
