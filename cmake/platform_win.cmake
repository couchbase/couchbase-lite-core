include(${CMAKE_CURRENT_LIST_DIR}/platform_base.cmake)

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
        PARENT_SCOPE
    )
endfunction()

function(set_support_source)
    set(oneValueArgs RESULT)
    cmake_parse_arguments(WIN_SSS "" ${oneValueArgs} "" ${ARGN})
    if(NOT DEFINED WIN_SSS_RESULT)
        message(FATAL_ERROR set_source_files_base needs to be called with RESULT)
    endif()

    set_support_source_base(RESULT BASE_SUPPORT_FILES)
    set(
        ${WIN_SSS_RESULT}
        ${BASE_SUPPORT_FILES}
        MSVC/asprintf.c
        vendor/fleece/MSVC/memmem.cc
        MSVC/mkstemp.cc
        MSVC/mkdtemp.cc
        MSVC/strlcat.c
        MSVC/vasprintf-msvc.c
        MSVC/arc4random.cc
        MSVC/strptime.cc
        LiteCore/Support/StringUtil_winapi.cc
        LiteCore/Support/Error_windows.cc
        PARENT_SCOPE
    )
endfunction()

function(setup_globals)
    set(CMAKE_C_FLAGS_MINSIZEREL "/MD /O1 /Ob1 /DNDEBUG /Zi /GL" CACHE INTERNAL "")
    set(CMAKE_CXX_FLAGS_MINSIZEREL "/MD /O1 /Ob1 /DNDEBUG /Zi /GL" CACHE INTERNAL "")
    set(CMAKE_SHARED_LINKER_FLAGS_MINSIZEREL "/INCREMENTAL:NO /LTCG:incremental /debug" CACHE INTERNAL "")
    set(CMAKE_STATIC_LINKER_FLAGS_MINSIZEREL "/LTCG:incremental" CACHE INTERNAL "")
    
    # Compile string literals as UTF-8,
    # Enable exception handling for C++ but disable for extern C
    # Disable the following warnings:
    #   4068 (unrecognized pragma)
    #   4244 (converting float to integer)
    #   4018 (signed / unsigned mismatch)
    #   4819 (character that cannot be represented in current code page)
    #   4800 (value forced to bool)
    # Disable warning about "insecure" C runtime functions (strcpy vs strcpy_s)
    string(
        CONCAT LITECORE_CXX_FLAGS 
        "/utf-8 "
        "/EHsc "
        "/wd4068 "
        "/wd4244 "
        "/wd4018 "
        "/wd4819 "
        "/wd4800 "
        "-D_CRT_SECURE_NO_WARNINGS=1"
    )
    set(LITECORE_CXX_FLAGS ${LITECORE_CXX_FLAGS} CACHE INTERNAL "")
    
    string(
        CONCAT LITECORE_C_FLAGS
        "/utf-8 "
        "/wd4068 "
        "/wd4244 "
        "/wd4018 "
        "/wd4819 "
        "/wd4800 "
        "-D_CRT_SECURE_NO_WARNINGS=1" 
    )
    set(LITECORE_C_FLAGS ${LITECORE_C_FLAGS} CACHE INTERNAL "")
    
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
        LiteCoreStatic PRIVATE 
        -DUNICODE               # Use wide string variants for Win32 calls
        -D_UNICODE              # Ditto
        -D_USE_MATH_DEFINES     # Define math constants like PI
        -DLITECORE_EXPORTS      # Export functions marked CBL_CORE_API, etc
        -DWIN32                 # Identify as WIN32
    )

    target_include_directories(LiteCoreStatic PRIVATE MSVC)
    target_include_directories(LiteCoreStatic PRIVATE vendor/fleece/MSVC)

    # Set the exported symbols for LiteCore
    set_target_properties(
        LiteCore PROPERTIES LINK_FLAGS
        "/def:${PROJECT_SOURCE_DIR}/C/c4.def"
    )

    target_include_directories(
        LiteCore PRIVATE
        vendor/fleece/API
        vendor/fleece/Fleece/Support
    )

    # Link with subproject libz and Windows sockets lib
    target_link_libraries(
        LiteCore PRIVATE 
        zlibstatic 
        Ws2_32
    )
endfunction()

function(setup_support_build)
    target_include_directories(Support PRIVATE MSVC)
    target_include_directories(Support PRIVATE vendor/fleece/MSVC)
endfunction()

function(setup_rest_build)
    target_include_directories(LiteCoreREST_Static PRIVATE ../MSVC)
endfunction()