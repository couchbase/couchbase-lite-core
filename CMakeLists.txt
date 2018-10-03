cmake_minimum_required (VERSION 3.1)
cmake_policy(VERSION 3.1)
project (LiteCore)

### BUILD SETTINGS:

set(COMPILE_FLAGS   "${COMPILE_FLAGS}   -Wall -Werror")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
option(CODE_COVERAGE_ENABLED "Set whether or not code coverage information should be generated" OFF)
option(BUILD_ENTERPRISE "Set whether or not to build enterprise edition" OFF)

add_definitions(-DSQLITE_OMIT_LOAD_EXTENSION        # This one's for SQLiteCpp
                -DUSE_WEBSOCKET                     # Enables CivetWeb WebSocket support
                -DNO_FILES                          # No file system support in CivetWeb (unneeded)
                -DNO_CGI                            # No CGI support in CivetWeb (unneeded)
                -DNO_CACHING                        # No file caching support in CivetWeb (unneeded)
                -DUSE_IPV6                          # Enable IPv6 support in CivetWeb
                -DCMAKE                             # Let the source know this is a CMAKE build
                -D__STDC_LIMIT_MACROS               # Ensure the presence of UINT64_MAX, etc
                -DNOMINMAX                          # Get rid of pesky Windows macros for min and max
                -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1
                -DSQLITE_LIKE_DOESNT_MATCH_BLOBS
                -DSQLITE_OMIT_SHARED_CACHE
                -DSQLITE_OMIT_DECLTYPE
                -DSQLITE_OMIT_DATETIME_FUNCS
                -DSQLITE_ENABLE_EXPLAIN_COMMENTS
                -DSQLITE_DISABLE_FTS3_UNICODE
                -DSQLITE_ENABLE_MEMORY_MANAGEMENT
                -DSQLITE_ENABLE_STAT4
                -DSQLITE_HAVE_ISNAN
                -DHAVE_LOCALTIME_R
                -DHAVE_USLEEP
                -DHAVE_UTIME
                -DSQLITE_OMIT_LOAD_EXTENSION
                -DSQLITE_ENABLE_FTS4
                -DSQLITE_ENABLE_FTS3_PARENTHESIS
                -DSQLITE_ENABLE_FTS3_TOKENIZER)

if(BUILD_ENTERPRISE)
    add_definitions(-DCOUCHBASE_ENTERPRISE      # Tells LiteCore it's an EE build
                    -DSQLITE_HAS_CODEC          # Enables SQLite encryption extension (SEE)
                    -DCCCRYPT256                # Enables AES256 in SEE
                    -DSQLITE_TEMP_STORE=2)      # Don't write temp files to disk (for security)
endif()

if(MSVC)
    if(WINDOWS_STORE)
        add_definitions(-DSQLITE_OS_WINRT
                        -DMBEDTLS_NO_PLATFORM_ENTROPY
                        -D_WIN32_WINNT=0x0602)      # Support back to Windows 8 for Windows Store
    else()
        add_definitions(-D_WIN32_WINNT=0x0601       # Support back to Windows 7 for Windows desktop
                        -DINCL_EXTRA_HTON_FUNCTIONS)# Make sure htonll is defined for WebSocketProtocol.hh
    endif()
elseif(ANDROID)
     # See: https://github.com/android-ndk/ndk/issues/477
     # The issue is also applicable for other areas like fseeko
     add_definitions(-DLITECORE_USES_ICU=1 -D_FILE_OFFSET_BITS=32)
endif()

set_property(DIRECTORY APPEND PROPERTY COMPILE_DEFINITIONS
    $<$<CONFIG:Debug>:DEBUG>
)

if(CODE_COVERAGE_ENABLED)
  if("${CMAKE_BUILD_TYPE}" STREQUAL "Debug" AND "${CMAKE_CXX_COMPILER_ID}" MATCHES "Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --coverage -O0")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --coverage -O0")
  elseif(NOT "${CMAKE_BUILD_TYPE}" STREQUAL "Debug")
    message(WARN " Code coverage not supported for non-debug builds")
  else()
    message(WARN " Code coverage only supported on Clang")
  endif()
endif()


if(MSVC)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /utf-8 /EHsc /wd4068 /wd4244 /wd4018 /wd4819 /wd4800 -D_CRT_SECURE_NO_WARNINGS=1")
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /utf-8 -D_CRT_SECURE_NO_WARNINGS=1")
endif()

if("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")
    add_definitions(-DLITECORE_USES_ICU=1)

    # Enable relative RPATHs for installed bits
    set (CMAKE_INSTALL_RPATH "\$ORIGIN")

    if(NOT "${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
        message(FATAL_ERROR "${CMAKE_CXX_COMPILER_ID} is not supported for building!")
    endif()
    find_library(LIBCXX_LIB c++)
    if (NOT LIBCXX_LIB)
        message(FATAL_ERROR "libc++ not found")
    endif()
    message("Found libc++ at ${LIBCXX_LIB}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")

    find_library(LIBCXXABI_LIB c++abi)
    if (NOT LIBCXXABI_LIB)
        message(FATAL_ERROR "libc++abi not found")
    endif()
    message("Found libc++abi at ${LIBCXXABI_LIB}")
    find_path(LIBCXX_INCLUDE c++/v1/string
        HINTS "${CMAKE_BINARY_DIR}/tlm/deps/libcxx.exploded"
        PATH_SUFFIXES include)
    if (NOT LIBCXX_INCLUDE)
        message(FATAL_ERROR "libc++ header files not found")
    endif()
    message("Using libc++ header files in ${LIBCXX_INCLUDE}")
    include_directories("${LIBCXX_INCLUDE}/c++/v1")
    if(NOT EXISTS "/usr/include/xlocale.h")
      include_directories("${LIBCXX_INCLUDE}/c++/v1/support/xlocale") # this fixed path is here to avoid compilation on Ubuntu 17.10 where xlocale.h is searched by some header(s) in libc++ as <xinclude.h> but not found from search path without this modification.  However, only do it if the original xlocale.h does not exist since this will get searched before /usr/include and override a valid file with an empty one.
    endif()
    include_directories("/usr/include/libcxxabi") # this fixed path is here to avoid Clang issue noted at http://lists.alioth.debian.org/pipermail/pkg-llvm-team/2015-September/005208.html
    find_library(ICU4C_COMMON icuuc)
    if (NOT ICU4C_COMMON)
        message(FATAL_ERROR "libicuuc not found")
    endif()
    message("Found libicuuc at ${ICU4C_COMMON}")
    find_library(ICU4C_I18N icui18n)
    if (NOT ICU4C_I18N)
        message(FATAL_ERROR "libicui18n not found")
    endif()
    message("Found libicui18n at ${ICU4C_I18N}")
    find_path(LIBICU_INCLUDE unicode/ucol.h
        HINTS "${CMAKE_BINARY_DIR}/tlm/deps/icu4c.exploded"
        PATH_SUFFIXES include)
    if (NOT LIBCXX_INCLUDE)
        message(FATAL_ERROR "libicu header files not found")
    endif()
    message("Using libicu header files in ${LIBICU_INCLUDE}")
    include_directories("${LIBICU_INCLUDE}")
endif()

if(WIN32 OR ANDROID)
    if(ANDROID)
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--exclude-libs,libgcc.a")
    endif()
endif()

### SUBMODULES:

include_directories("vendor/fleece/API"
                    "vendor/fleece/Fleece/Core"
                    "vendor/fleece/Fleece/Support"
                    "vendor/SQLiteCpp/include"
                    "vendor/SQLiteCpp/sqlite3"
                    "vendor/sqlite3-unicodesn"
                    "vendor/BLIP-Cpp/include/blip_cpp"
                    "vendor/BLIP-Cpp/src/util"
                    "vendor/civetweb/include"
                    "vendor/mbedtls/include")

if(WIN32)
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /ignore:4099 /ignore:4221")
  set(CMAKE_STATIC_LINKER_FLAGS "${CMAKE_STATIC_LINKER_FLAGS} /ignore:4221")
  include_directories("MSVC")
elseif(ANDROID)
  include_directories("LiteCore/Unix")
  include_directories("LiteCore/Android")
elseif(UNIX AND NOT APPLE)
  include_directories("LiteCore/Unix")
endif()

add_subdirectory(REST)

add_subdirectory(vendor/fleece              EXCLUDE_FROM_ALL)
add_subdirectory(vendor/sqlite3-unicodesn   EXCLUDE_FROM_ALL)
add_subdirectory(vendor/BLIP-Cpp            EXCLUDE_FROM_ALL)
add_subdirectory(vendor/mbedtls             EXCLUDE_FROM_ALL)

### MORE BUILD SETTINGS:

# Mac/iOS stuff
if (APPLE)
    # Specify that we want to use CommonCrypto:
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_CRYPTO_CC")

  elseif(UNIX AND NOT ANDROID)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_CRYPTO_MBEDTLS")
endif()

# Generate file repo_version.h containing Git repo information, and add it to #include path:
set(GENERATED_HEADERS_DIR "${CMAKE_BINARY_DIR}/generated_headers")
file(MAKE_DIRECTORY "${GENERATED_HEADERS_DIR}")
if (UNIX)
    execute_process(COMMAND /bin/bash "${PROJECT_SOURCE_DIR}/build_cmake/scripts/get_repo_version.sh"
                                      "${GENERATED_HEADERS_DIR}/repo_version.h")
else()
    execute_process(COMMAND powershell "${PROJECT_SOURCE_DIR}/build_cmake/scripts/get_repo_version.ps1"
                                      "${GENERATED_HEADERS_DIR}/repo_version.h")
endif()
include_directories(${GENERATED_HEADERS_DIR})


### SOURCE FILES:

include_directories(LiteCore/BlobStore
                    LiteCore/Database
                    LiteCore/Indexes
                    LiteCore/Query
                    LiteCore/RevTrees
                    LiteCore/Storage
                    LiteCore/Support
                    LiteCore/VersionVectors
                    C/include C)

# TODO: CMake docs say it's better to list the source files explicitly than use aux_source_directory
aux_source_directory(C                        C_SRC)
aux_source_directory(LiteCore/BlobStore       BLOBSTORE_SRC)
aux_source_directory(LiteCore/Database        DATABASE_SRC)
aux_source_directory(LiteCore/Indexes         INDEXES_SRC)
aux_source_directory(LiteCore/Query           QUERY_SRC)
aux_source_directory(LiteCore/RevTrees        REVTREES_SRC)
aux_source_directory(LiteCore/Storage         STORAGE_SRC)
aux_source_directory(LiteCore/VersionVectors  VERSIONVECTORS_SRC)
aux_source_directory(LiteCore/Support         SUPPORT_SRC)
list(REMOVE_ITEM SUPPORT_SRC LiteCore/Support/Logging_Stub.cc)
aux_source_directory(vendor/SQLiteCpp/src     SQLITECPP_SRC)
aux_source_directory(Replicator               REPLICATOR_SRC)

set(CIVETWEB_SRC "vendor/civetweb/src/civetweb.c"
                 "REST/civetUtils.cc")

if(!MSVC)
    set_source_files_properties(${C_SRC} PROPERTIES COMPILE_FLAGS -Wno-return-type-c-linkage)
endif()

### BUILDING THE LIBRARY:

set(ALL_SRC_FILES
  ${BLOBSTORE_SRC}
  ${DATABASE_SRC}
  ${INDEXES_SRC}
  ${QUERY_SRC}
  ${REVTREES_SRC}
  ${STORAGE_SRC}
  ${SUPPORT_SRC}
  ${VERSIONVECTORS_SRC}
  ${C_SRC}
  ${SQLITECPP_SRC}
  ${REPLICATOR_SRC})

if(MSVC)
	include_directories("vendor/fleece/MSVC")
elseif(ANDROID)
    set(ALL_SRC_FILES ${ALL_SRC_FILES}
    LiteCore/Unix/strlcat.c
    LiteCore/Unix/arc4random.cc
    LiteCore/Android/unicode/ndk_icu.c)
elseif(UNIX AND NOT APPLE)
    set(ALL_SRC_FILES ${ALL_SRC_FILES}
    LiteCore/Unix/strlcat.c
    LiteCore/Unix/arc4random.cc)
endif()

if("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")
    # libc++ is special - clang will introduce an implicit -lc++ when it is used.
    # That means we need to tell the linker the path to the directory containing
    # libc++.so rather than just linking the .so directly. This must be done
    # *before* the target declaration as it affects all subsequent targets.
    get_filename_component (LIBCXX_LIBDIR "${LIBCXX_LIB}" DIRECTORY)
    link_directories (${LIBCXX_LIBDIR})
endif()

add_library(LiteCoreStatic STATIC ${ALL_SRC_FILES})
target_compile_definitions(LiteCoreStatic PRIVATE -DLITECORE_IMPL)

if(WIN32)
    add_library(LiteCore SHARED MSVC/SQLiteTempDirectory.cc) #No-op for non-UWP
else()
    file(WRITE empty.cpp)
    add_library(LiteCore SHARED empty.cpp)
endif()

install (TARGETS LiteCore
         RUNTIME DESTINATION bin
         LIBRARY DESTINATION lib)

if(UNIX AND NOT APPLE)
    set(WHOLE_LIBRARY_FLAG "-Wl,--whole-archive")
    set(NO_WHOLE_LIBRARY_FLAG "-Wl,--no-whole-archive")
endif()

set(LITECORE_LIBRARIES_PRIVATE  ${WHOLE_LIBRARY_FLAG}
                                LiteCoreStatic
                                FleeceStatic
                                ${NO_WHOLE_LIBRARY_FLAG}
                                Support
                                SQLite3_UnicodeSN
                                BLIPStatic
                                CivetWeb
                                mbedcrypto)

target_link_libraries(LiteCore PRIVATE ${LITECORE_LIBRARIES_PRIVATE})

# Mac/iOS stuff
if (APPLE)
    set_target_properties(LiteCore PROPERTIES LINK_FLAGS
                          "-exported_symbols_list ${PROJECT_SOURCE_DIR}/C/c4.exp")
    target_link_libraries(LiteCore PUBLIC  "-framework CoreFoundation"
                                    "-framework Foundation"
                                    "-framework CFNetwork"
                                    "-framework Security"
                                    z)
elseif(UNIX AND NOT ANDROID)
    target_link_libraries(LiteCore PUBLIC z ${ICU4C_COMMON} ${ICU4C_I18N})
elseif(ANDROID)
    target_compile_definitions(LiteCoreStatic PUBLIC -D_CRYPTO_MBEDTLS)
    target_link_libraries(LiteCore PRIVATE "atomic" "log" zlibstatic)
else()
    target_compile_definitions(LiteCoreStatic PUBLIC -DUNICODE -D_UNICODE -D_CRYPTO_MBEDTLS -D_USE_MATH_DEFINES -DLITECORE_EXPORTS -DWIN32)
    if(WINDOWS_STORE)
        target_compile_definitions(LiteCoreStatic PUBLIC -DMBEDTLS_NO_PLATFORM_ENTROPY)
        set_target_properties(LiteCore PROPERTIES COMPILE_FLAGS /ZW)
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /nodefaultlib:kernel32.lib /nodefaultlib:ole32.lib")
    endif()
    set_target_properties(LiteCore PROPERTIES LINK_FLAGS
			  "/def:${PROJECT_SOURCE_DIR}/C/c4.def")
    target_link_libraries(LiteCore PRIVATE zlibstatic Ws2_32)
endif()

### JNI:

if (JNI)
    add_subdirectory(Java)
endif()

### REST LISTENER LIBRARY:

add_library(LiteCoreREST SHARED LiteCore/Support/Error.cc
                                LiteCore/Support/Error_android.cc
                                LiteCore/Support/Error_windows.cc
                                LiteCore/Support/FilePath.cc
                                LiteCore/Support/Logging_Stub.cc
                                LiteCore/Support/StringUtil.cc
                                C/c4ExceptionUtils.cc)
target_link_libraries(LiteCoreREST PRIVATE ${WHOLE_LIBRARY_FLAG} LiteCoreREST_Static ${NO_WHOLE_LIBRARY_FLAG} LiteCore FleeceStatic)

if (APPLE)
    set_target_properties(LiteCoreREST PROPERTIES LINK_FLAGS
                          "-exported_symbols_list ${PROJECT_SOURCE_DIR}/REST/c4REST.exp")
elseif(MSVC)
    target_link_libraries(LiteCoreREST PUBLIC)
    set_target_properties(LiteCoreREST PROPERTIES LINK_FLAGS
              "/def:${PROJECT_SOURCE_DIR}/REST/c4REST.def")
elseif(ANDROID)
    target_link_libraries(LiteCoreREST PRIVATE "atomic" "log")
endif()

### SUPPORT LIB:

if(MSVC)
    set(PLATFORM_SRC
    MSVC/asprintf.c
    vendor/fleece/MSVC/memmem.cc
    MSVC/mkstemp.cc
    MSVC/mkdtemp.cc
    MSVC/strlcat.c
    MSVC/vasprintf-msvc.c
    MSVC/arc4random.cc
    MSVC/strptime.cc
    LiteCore/Support/PlatformIO.cc)
elseif(ANDROID)
    set(PLATFORM_SRC
    LiteCore/Unix/strlcat.c
    LiteCore/Unix/arc4random.cc)
elseif(UNIX AND NOT APPLE)
    set(PLATFORM_SRC
    LiteCore/Unix/strlcat.c
    LiteCore/Unix/arc4random.cc)
endif()

add_library(Support STATIC
    C/c4ExceptionUtils.cc
    LiteCore/Support/LibC++Debug.cc
    LiteCore/Support/Error.cc
    LiteCore/Support/Error_windows.cc
    LiteCore/Support/Error_android.cc
    LiteCore/Support/FilePath.cc
    LiteCore/Support/Logging_Stub.cc
    LiteCore/Support/StringUtil.cc
    Replicator/Address.cc
    ${PLATFORM_SRC}
)

### TOOLS:

add_subdirectory(tools)

### TESTS:

add_subdirectory(LiteCore/tests)
add_subdirectory(C/tests)
