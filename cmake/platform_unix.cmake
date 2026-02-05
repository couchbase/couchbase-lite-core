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
        set_property(TARGET LiteCoreObjects LiteCoreUnitTesting PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        set_property(TARGET FleeceStatic       PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        if(LITECORE_BUILD_SHARED)
            set_property(TARGET LiteCore       PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
        endif()
    endif()

    if(LITECORE_BUILD_SHARED AND CMAKE_SYSTEM_PROCESSOR MATCHES "^armv[67]")
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
        if (LITECORE_BUILD_SHARED)
            target_link_options(LiteCore PRIVATE -fsanitize=address -fsanitize=undefined)
        endif()
        if (LITECORE_BUILD_TESTS)
            target_link_options(CppTests PRIVATE -fsanitize=address -fsanitize=undefined)
            target_link_options(C4Tests  PRIVATE -fsanitize=address -fsanitize=undefined)
        endif()
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

    if(LITECORE_WARNINGS_HARDCORE)
        if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            set(LITECORE_CXX_WARNINGS
                -Werror
                -Weverything            # "WARN ALL THE THINGS!!!"
                -Wformat=2
                # Disabled C++ warnings:
                -Wno-cast-qual  # TODO: "cast drops const qualifier"
                -Wno-nullable-to-nonnull-conversion # TODO: "implicit conversion from nullable pointer to non-nullable pointer type"
                -Wno-sign-compare # TODO "comparison of integers of different signs"
                -Wno-sign-conversion # TODO "implicit conversion changes signedness"
                -Wno-switch-enum # TODO: "enumeration values not explicitly handled in switch"
                -Wno-alloca
                -Wno-atomic-implicit-seq-cst # "implicit use of sequentially-consistent atomic may incur stronger memory barriers than necessary"
                -Wno-c99-extensions
                -Wno-c++98-compat
                -Wno-c++98-compat-pedantic
                -Wno-cast-align # "cast from X* to Y* increases required alignment"
                -Wno-cast-function-type # "cast from X* to Y* converts to incompatible function type"
                -Wno-covered-switch-default # "default label in switch which covers all enumeration values"
                -Wno-ctad-maybe-unsupported # "'...' may not intend to support class template argument deduction"
                -Wno-date-time # "expansion of date or time macro is not reproducible"
                -Wno-deprecated-copy-with-user-provided-dtor # "definition of implicit copy constructor is deprecated because it has a user-provided destructor"
                -Wno-documentation-pedantic
                -Wno-direct-ivar-access # Obj-C: "instance variable is being directly accessed"
                -Wno-exit-time-destructors # "declaration requires an exit-time destructor"
                -Wno-extra-semi # "extra ';' after member function definition"
                -Wno-float-equal
                -Wno-format-pedantic # "format specifies type 'void *' but the argument has type 'C4Document *'"
                -Wno-global-constructors
                -Wno-gnu-anonymous-struct # "anonymous structs are a GNU extension"
                -Wno-gnu-zero-variadic-macro-arguments # "token pasting of ',' and __VA_ARGS__ is a GNU extension"
                -Wno-inconsistent-missing-destructor-override # "'~Foo' overrides a destructor but is not marked 'override'"
                -Wno-missing-designated-field-initializers # "missing field 'x' initializer"
                -Wno-missing-field-initializers # "missing field 'x' initializer"
                -Wno-missing-noreturn # "function could be declared with attribute 'noreturn'"
                -Wno-nested-anon-types # "anonymous types declared in an anonymous union are an extension"
                -Wno-nullability-extension
                -Wno-old-style-cast
                -Wno-padded
                -Wno-reserved-identifier
                -Wno-reserved-macro-identifier
                -Wno-shadow-field # "parameter shadows member inherited from type"
                -Wno-shadow-uncaptured-local # "declaration [of a lambda parameter] shadows a local variable"
                -Wno-suggest-destructor-override # "'~Foo' overrides a destructor but is not marked 'override'"
                -Wno-switch-default # "'switch' missing 'default' label"
                -Wno-undef      # `#if X` where X isn't defined
                -Wno-unknown-warning-option   # So Clang 17 warning flags listed here don't break Clang 16
                -Wno-unsafe-buffer-usage-in-container # "the two-parameter std::span construction is unsafe"
                -Wno-unused-macros
                -Wno-unused-exception-parameter
                -Wno-unused-parameter # Unused fn parameter
                -Wno-weak-vtables # "Class has no out-of-line virtual method definitions; its vtable will be emitted in every translation unit"
                -Wno-zero-as-null-pointer-constant # Using 0 instead of nullptr. Some of our dependencies do this in headers.
                -Wno-documentation-deprecated-sync # "declaration is marked with '\deprecated' command but does not have a deprecation attribute" --mbedTLS does this
            )
        endif()
    endif()

    foreach(target  LiteCoreObjects LiteCoreUnitTesting BLIPObjects LiteCoreWebSocket LiteCoreREST_Objects)
        target_compile_options(${target} PRIVATE
            ${LITECORE_COMPILE_OPTIONS}
            ${LITECORE_WARNINGS} 
            $<$<COMPILE_LANGUAGE:CXX>:${LITECORE_CXX_WARNINGS}>
            $<$<COMPILE_LANGUAGE:C>:${LITECORE_C_WARNINGS}>
        )
    endforeach()
    if (BUILD_ENTERPRISE)
        set(targets LiteCoreListener_Objects)
        if (ANDROID OR APPLE)
            list(APPEND targets LiteCoreP2P)
        endif()
        foreach(target ${targets})
            target_compile_options(${target} PRIVATE
                    ${LITECORE_COMPILE_OPTIONS}
                    ${LITECORE_WARNINGS}
                    $<$<COMPILE_LANGUAGE:CXX>:${LITECORE_CXX_WARNINGS}>
                    $<$<COMPILE_LANGUAGE:C>:${LITECORE_C_WARNINGS}>
            )
        endforeach ()
    endif ()

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
