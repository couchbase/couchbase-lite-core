include(${CMAKE_CURRENT_LIST_DIR}/platform_base.cmake)

function(setup_globals_unix)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set(CMAKE_C_FLAGS_MINSIZEREL "-Os -DNDEBUG -g" CACHE INTERNAL "")
        set(CMAKE_CXX_FLAGS_MINSIZEREL "-Os -DNDEBUG -g" CACHE INTERNAL "")
    elseif(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
        set(CMAKE_C_FLAGS_MINSIZEREL "-Oz -DNDEBUG -g" CACHE INTERNAL "")
        set(CMAKE_CXX_FLAGS_MINSIZEREL "-Oz -DNDEBUG -g" CACHE INTERNAL "")
    else()
        set(CMAKE_C_FLAGS_MINSIZEREL "-O3 -DNDEBUG -g" CACHE INTERNAL "")
        set(CMAKE_CXX_FLAGS_MINSIZEREL "-O3 -DNDEBUG -g" CACHE INTERNAL "")
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
        if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
            # GNU and Linux clang LTO can't seem to handle any of this...at least not with the versions I tried.  
            # Unexplained linker errors occur.
            set_property(TARGET LiteCoreStatic PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
            set_property(TARGET FleeceStatic       PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
            set_property(TARGET BLIPStatic       PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
            set_property(TARGET LiteCoreWebSocket PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
            set_property(TARGET LiteCoreREST_Static PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        endif()

        set_property(TARGET LiteCore       PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
endfunction()

function(setup_rest_build_unix)
    target_link_libraries(
        LiteCoreREST PRIVATE
        FleeceBase
        Support
        CivetWeb
    )
endfunction()
