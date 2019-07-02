include(${CMAKE_CURRENT_LIST_DIR}/platform_base.cmake)

function(setup_globals_unix)
    set(CMAKE_C_FLAGS_MINSIZEREL "-Oz -DNDEBUG -g" CACHE INTERNAL "")
    set(CMAKE_CXX_FLAGS_MINSIZEREL "-Oz -DNDEBUG -g" CACHE INTERNAL "")
endfunction()

function(setup_litecore_build_unix)
    FILE(GLOB C_SRC LIST_DIRECTORIES FALSE "C/*.cc")
    set_source_files_properties(${C_SRC} PROPERTIES COMPILE_FLAGS -Wno-return-type-c-linkage)

    # Enable Link-Time Optimization, AKA Inter-Procedure Optimization
    if(NOT ANDROID AND NOT ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug"))
        include(CheckIPOSupported)
        check_ipo_supported(RESULT LTOAvailable)
    endif()
    if(LTOAvailable AND (NOT DISABLE_LTO_BUILD))
        message("Link-time optimization enabled")
        set_property(TARGET LiteCoreStatic PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET LiteCore       PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET FleeceStatic       PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET BLIPStatic       PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET Support       PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^armv[67]")
        # C/C++ atomic operations on ARM6/7 emit calls to functions in libatomic
        target_link_libraries(
            LiteCore PRIVATE
            atomic
        )
    endif()

    if(CMAKE_COMPILER_IS_GNUCC OR CMAKE_COMPILER_IS_GNUCXX)
        # List of symbols to export from LiteCore:
        target_link_libraries(
            LiteCore PRIVATE
            "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/C/c4exports.gnu"
        )
    endif()
endfunction()
