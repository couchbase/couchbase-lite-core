include(${CMAKE_CURRENT_LIST_DIR}/platform_base.cmake)
include(CheckTypeSize)

macro(check_threading_unix)
    set(THREADS_PREFER_PTHREAD_FLAG ON) 
    find_package(Threads)
endmacro()

function(setup_globals_unix)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
        set(CMAKE_C_FLAGS_MINSIZEREL "-Oz -DNDEBUG -g" CACHE INTERNAL "")
        set(CMAKE_CXX_FLAGS_MINSIZEREL "-Oz -DNDEBUG -g" CACHE INTERNAL "")
    else()
        set(CMAKE_C_FLAGS_MINSIZEREL "-Os -DNDEBUG -g" CACHE INTERNAL "")
        set(CMAKE_CXX_FLAGS_MINSIZEREL "-Os -DNDEBUG -g" CACHE INTERNAL "")
    endif()
endfunction()

function(setup_litecore_build_unix)
    setup_litecore_build_base()

    FILE(GLOB C_SRC LIST_DIRECTORIES FALSE "C/*.cc")
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        set_source_files_properties(${C_SRC} PROPERTIES COMPILE_FLAGS -Wno-return-type-c-linkage)
    endif()

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
            set_property(TARGET LiteCoreObjects LiteCoreUnitTesting PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
            set_property(TARGET FleeceStatic       PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
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


    if (LITECORE_SANITIZE AND NOT CODE_COVERAGE_ENABLED AND (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "GNU"))
        set(LITECORE_SAN_FLAGS "-fstack-protector -fsanitize=address -fsanitize-address-use-after-scope -fsanitize=undefined -fno-sanitize-recover=all")
        # "-fno-sanitize-recover=all" : Always exit after UBSan warning
        # Note: _FORTIFY_SOURCE is incompatible with ASan; defining it will cause build errors
        if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            set(LITECORE_SAN_FLAGS "${LITECORE_SAN_FLAGS} -fsanitize=nullability -fsanitize-address-use-after-return=always")
        endif ()
        # Enable sanitizers for ALL targets. It's especially important to set them for all C++
        # targets, because otherwise the container overflow check can produce false positives:
        # https://github.com/google/sanitizers/wiki/AddressSanitizerContainerOverflow#false-positives
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${LITECORE_SAN_FLAGS}" CACHE INTERNAL "")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${LITECORE_SAN_FLAGS}" CACHE INTERNAL "")

        # The linker also needs to be told, so it can link the appropriate sanitizer runtime libs:
        foreach(target LiteCore CppTests C4Tests)
            target_link_options(${target} PRIVATE
                -fsanitize=address
                -fsanitize=undefined
            )
        endforeach ()
    else()
        set(LITECORE_COMPILE_OPTIONS
            -fstack-protector
            -D_FORTIFY_SOURCE=2
        )
    endif()


    set(LITECORE_WARNINGS
        -Werror=missing-braces
        -Werror=parentheses
        -Werror=switch
        -Werror=unused-function
        -Werror=unused-label
        -Werror=unused-variable
        -Werror=unused-value
        -Werror=uninitialized
        -Werror=float-conversion
        #-Wformat=2
        #-Wshadow
        #-Weffc++
    )

    set(LITECORE_CXX_WARNINGS
        -Wnon-virtual-dtor
        #-Werror=overloaded-virtual
        -Wno-psabi
        -Wno-odr
    )

    set(LITECORE_C_WARNINGS
        -Werror=incompatible-pointer-types
        -Werror=int-conversion
        -Werror=strict-prototypes
    )

    foreach(target  LiteCoreObjects LiteCoreUnitTesting BLIPObjects LiteCoreWebSocket LiteCoreREST_Objects)
        target_compile_options(${target} PRIVATE
            ${LITECORE_COMPILE_OPTIONS}
            ${LITECORE_WARNINGS} 
            $<$<COMPILE_LANGUAGE:CXX>:${LITECORE_CXX_WARNINGS}>
            $<$<COMPILE_LANGUAGE:C>:${LITECORE_C_WARNINGS}>
        )
    endforeach()

    set(CMAKE_EXTRA_INCLUDE_FILES "sys/socket.h")
    check_type_size(socklen_t SOCKLEN_T)
    if(${HAVE_SOCKLEN_T})
        # mbedtls fails to detect this accurately
        target_compile_definitions(
            mbedtls PRIVATE
            _SOCKLEN_T_DECLARED
        )
    endif()
endfunction()

function(setup_rest_build_unix)
endfunction()
