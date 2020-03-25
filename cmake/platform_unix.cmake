include(${CMAKE_CURRENT_LIST_DIR}/platform_base.cmake)

function(setup_globals_unix)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(CMAKE_C_FLAGS_MINSIZEREL "-Os -DNDEBUG -g" CACHE INTERNAL "")
        set(CMAKE_CXX_FLAGS_MINSIZEREL "-Os -DNDEBUG -g" CACHE INTERNAL "")
    else()
        set(CMAKE_C_FLAGS_MINSIZEREL "-Oz -DNDEBUG -g" CACHE INTERNAL "")
        set(CMAKE_CXX_FLAGS_MINSIZEREL "-Oz -DNDEBUG -g" CACHE INTERNAL "")
    endif()
endfunction()

function(setup_litecore_build_unix)
    FILE(GLOB C_SRC LIST_DIRECTORIES FALSE "C/*.cc")
    set_source_files_properties(${C_SRC} PROPERTIES COMPILE_FLAGS -Wno-return-type-c-linkage)

    # Enable Link-Time Optimization, AKA Inter-Procedure Optimization
    if(NOT ANDROID AND NOT DISABLE_LTO_BUILD AND
       NOT ("${CMAKE_BUILD_TYPE}" STREQUAL "Debug" OR "${CMAKE_BUILD_TYPE}" STREQUAL ""))
        include(CheckIPOSupported)
        check_ipo_supported(RESULT LTOAvailable)
    endif()
    if(LTOAvailable)
        message("Link-time optimization enabled")
        if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            # GNU LTO can't seem to handle any of this...at least not with 7.4.  Unexplained
            # linker errors occur.
            set_property(TARGET LiteCoreStatic PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
            set_property(TARGET FleeceStatic       PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
            set_property(TARGET BLIPStatic       PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
            set_property(TARGET Support       PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        endif()

        set_property(TARGET LiteCore       PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()

    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^armv[67]")
        # C/C++ atomic operations on ARM6/7 emit calls to functions in libatomic
        target_link_libraries(
            LiteCore PRIVATE
            atomic
        )
    endif()

    set(LITECORE_WARNINGS
        -Wincompatible-pointer-types
        -Wnon-virtual-dtor
        -Woverloaded-virtual
        -Wmissing-braces
        -Wparentheses
        -Wswitch
        -Wunused-function
        -Wunused-label
        -Wno-unused-parameter
        -Wunused-variable
        -Wunused-value
        -Wuninitialized
        -Wunknown-pragmas
        -Wshadow
        -Wint-conversion
        -Wfloat-conversion
        -Wstrict-prototypes
        -Weffc++
        -Wmemset-transposed-args
        -Werror
    )

    target_compile_options(LiteCoreStatic PRIVATE ${LITECORE_WARNINGS})
    target_compile_options(BLIPStatic PRIVATE ${LITECORE_WARNINGS})
    target_compile_options(FleeceStatic PRIVATE ${LITECORE_WARNINGS})
endfunction()

function(setup_rest_build_unix)
endfunction()
