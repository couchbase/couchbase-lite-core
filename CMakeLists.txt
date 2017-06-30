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
OPTION(LITECORE_BUILD_SQLITE "If ON, builds libsqlite as part of the build process")

add_definitions(-DC4DB_THREADSAFE                   # Makes LiteCore C API thread-safe
                -DSQLITE_OMIT_LOAD_EXTENSION        # This one's for SQLiteCpp
                -DUSE_WEBSOCKET                     # Enables CivetWeb WebSocket support
                -DNO_FILES                          # No file system support in CivetWeb (unneeded)
                -DNO_CGI                            # No CGI support in CivetWeb (unneeded)
                -DNO_CACHING                        # No file caching support in CivetWeb (unneeded)
                -DUSE_IPV6                          # Enable IPv6 support in CivetWeb
                -DCMAKE                             # Let the source know this is a CMAKE build
                -D__STDC_LIMIT_MACROS)              # Ensure the presence of UINT64_MAX, etc

set_property(DIRECTORY APPEND PROPERTY COMPILE_DEFINITIONS
    $<$<CONFIG:Debug>:DEBUG>
)
if(MSVC)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc /wd4068 /wd4244 /wd4018 /wd4819 /wd4800 -D_CRT_SECURE_NO_WARNINGS=1")
endif()

if("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")
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
endif()

if(WIN32 OR ANDROID)
    if(${CMAKE_GENERATOR} STREQUAL "Visual Studio 14 2015")
        set(OPENSSL_ROOT_DIR ${PROJECT_SOURCE_DIR}/vendor/openssl/libs/windows/x86/)
    elseif(${CMAKE_GENERATOR} STREQUAL "Visual Studio 14 2015 Win64")
        set(OPENSSL_ROOT_DIR ${PROJECT_SOURCE_DIR}/vendor/openssl/libs/windows/amd64/)
    elseif(${CMAKE_GENERATOR} STREQUAL "Visual Studio 14 2015 ARM")
        set(OPENSSL_ROOT_DIR ${PROJECT_SOURCE_DIR}/vendor/openssl/libs/windows/arm/)
    elseif(ANDROID)
        set(OPENSSL_ROOT_DIR ${PROJECT_SOURCE_DIR}/vendor/openssl/libs/android/${CMAKE_ANDROID_ARCH_ABI}/)
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--exclude-libs,libgcc.a")
    endif()
    set(OPENSSL_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/vendor/openssl/libs/include)
    set(SQLITE3_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/vendor/SQLiteCpp/sqlite3)
    if(ANDROID)
        set(OPENSSL_CRYPTO_LIBRARY ${OPENSSL_ROOT_DIR}/libcrypto.a)
    elseif(WINDOWS_STORE)
        set(OPENSSL_CRYPTO_LIBRARY ${OPENSSL_ROOT_DIR}/libeay32_store.lib)
    else()
        set(OPENSSL_CRYPTO_LIBRARY ${OPENSSL_ROOT_DIR}/libeay32.lib)
    endif()
endif()

### SUBMODULES:

include_directories("vendor/fleece/Fleece"
                    "vendor/fleece/vendor"
                    "vendor/SQLiteCpp/include"
                    "vendor/sqlite3-unicodesn"
                    "vendor/BLIP-Cpp/include/blip_cpp"
                    "vendor/BLIP-Cpp/src/util"
                    "vendor/civetweb/include")

if(WIN32)
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /ignore:4099")
  include_directories("MSVC")
elseif(ANDROID)
  include_directories("LiteCore/Android")
endif()

if(WIN32 OR ANDROID OR LITECORE_BUILD_SQLITE)
    set(SQLITECPP_INTERNAL_SQLITE OFF)
    add_library(sqlite3 SHARED
     vendor/SQLiteCpp/sqlite3/sqlite3.c
     vendor/SQLiteCpp/sqlite3/sqlite3.h
    )
    install (TARGETS sqlite3
             RUNTIME DESTINATION bin
             LIBRARY DESTINATION lib)
    include_directories("vendor/SQLiteCpp/sqlite3/")

    if(WINDOWS_STORE)
        set(SQLITE3_FLAGS "-DSQLITE_ENABLE_FTS4_UNICODE61 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_FTS3_TOKENIZER -DSQLITE_OS_WINRT")
    else()
        set(SQLITE3_FLAGS "-DSQLITE_ENABLE_FTS4_UNICODE61 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_FTS3_TOKENIZER")
    endif()
    set_target_properties(sqlite3 PROPERTIES COMPILE_FLAGS ${SQLITE3_FLAGS})
    if(WIN32)
        set_target_properties(sqlite3 PROPERTIES LINK_FLAGS
                "/def:\"${CMAKE_CURRENT_LIST_DIR}/MSVC/sqlite3.def\"")
    endif()
endif()

add_subdirectory(REST)

add_subdirectory(vendor/fleece              EXCLUDE_FROM_ALL)
add_subdirectory(vendor/sqlite3-unicodesn   EXCLUDE_FROM_ALL)
add_subdirectory(vendor/BLIP-Cpp            EXCLUDE_FROM_ALL)

### MORE BUILD SETTINGS:

# Mac/iOS stuff
if (APPLE)
    # Specify that we want to use CommonCrypto:
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_CRYPTO_CC")

  elseif(UNIX AND NOT ANDROID)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_CRYPTO_OPENSSL")
else()
    include_directories(${OPENSSL_INCLUDE_DIR}
      ${SQLITE3_INCLUDE_DIR})
endif()

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
	set(ALL_SRC_FILES ${ALL_SRC_FILES} MSVC/asprintf.c
			     vendor/fleece/MSVC/memmem.cc MSVC/mkstemp.cc MSVC/mkdtemp.cc MSVC/strlcat.c
			     MSVC/vasprintf-msvc.c MSVC/arc4random.cc MSVC/strptime.cc)
elseif(ANDROID)
  set(ALL_SRC_FILES ${ALL_SRC_FILES} LiteCore/Android/strlcat.c)
endif()

add_library(LiteCoreStatic STATIC ${ALL_SRC_FILES})
file(WRITE empty.cpp)
add_library(LiteCore SHARED empty.cpp)
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
                                SQLite3_UnicodeSN
                                BLIPStatic
                                CivetWeb
                                ${NO_WHOLE_LIBRARY_FLAG})
set(LITECORE_LIBRARIES_PUBLIC   sqlite3)

target_link_libraries(LiteCore PRIVATE ${LITECORE_LIBRARIES_PRIVATE})
target_link_libraries(LiteCore PUBLIC  ${LITECORE_LIBRARIES_PUBLIC})

if("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")
    target_link_libraries(LiteCore PUBLIC ${LIBCXX_LIB})
endif()

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
    target_link_libraries(LiteCore PRIVATE crypto bsd atomic)
    target_link_libraries(LiteCore PUBLIC z)
elseif(ANDROID)
   target_compile_definitions(LiteCoreStatic PUBLIC -D_CRYPTO_OPENSSL)
    target_link_libraries(LiteCore PRIVATE ${OPENSSL_CRYPTO_LIBRARY} "atomic" "log")
    target_link_libraries(LiteCore PUBLIC z)
else()
    target_compile_definitions(LiteCoreStatic PUBLIC -DUNICODE -D_UNICODE -D_CRYPTO_OPENSSL -D_USE_MATH_DEFINES -DLITECORE_EXPORTS -DWIN32)
    if(WINDOWS_STORE)
        set_target_properties(LiteCore PROPERTIES COMPILE_FLAGS /ZW)
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} /nodefaultlib:kernel32.lib /nodefaultlib:ole32.lib")
    endif()
    set_target_properties(LiteCore PROPERTIES LINK_FLAGS
			  "/def:${PROJECT_SOURCE_DIR}/C/c4.def")
    target_link_libraries(LiteCore PRIVATE ${OPENSSL_CRYPTO_LIBRARY} zlibstatic Ws2_32 Dbghelp.lib)
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
                                LiteCore/Support/RefCounted.cc
                                C/c4ExceptionUtils.cc)
target_link_libraries(LiteCoreREST PRIVATE ${WHOLE_LIBRARY_FLAG} LiteCoreREST_Static ${NO_WHOLE_LIBRARY_FLAG} LiteCore FleeceStatic)

if (APPLE)
    set_target_properties(LiteCoreREST PROPERTIES LINK_FLAGS
                          "-exported_symbols_list ${PROJECT_SOURCE_DIR}/REST/c4REST.exp")
elseif(MSVC)
    target_link_libraries(LiteCoreREST PUBLIC Dbghelp.lib)
    set_target_properties(LiteCoreREST PROPERTIES LINK_FLAGS
              "/def:${PROJECT_SOURCE_DIR}/REST/c4REST.def")

endif()

### LITECORESERV:

#add_executable(LiteCoreServ REST/LiteCoreServ.cc)
#target_link_libraries(LiteCoreServ PRIVATE LiteCoreREST_Static)
#target_link_libraries(LiteCoreServ PRIVATE ${LITECORE_LIBRARIES_PRIVATE})
#target_link_libraries(LiteCoreServ PUBLIC  ${LITECORE_LIBRARIES_PUBLIC})
#if(APPLE)
#  target_link_libraries(LiteCoreServ PRIVATE "-framework Foundation"
#                                             "-framework CFNetwork"
#                                             "-framework Security"
#                                             z)
#else()
#  target_link_libraries(LiteCoreServ PRIVATE ${OPENSSL_CRYPTO_LIBRARY})
#  if(ANDROID)
#    target_link_libraries(LiteCoreServ PRIVATE log z)
#  elseif(MSVC)
#    set_target_properties(LiteCoreServ PROPERTIES LINK_FLAGS
#              "/ignore:4099")
#    target_link_libraries(LiteCoreServ PRIVATE zlibstatic)
#  elseif(UNIX)
#    target_link_libraries(LiteCoreServ PRIVATE pthread z atomic crypto dl bsd c++abi)
#  endif()
#endif()


### TESTS:

add_subdirectory(LiteCore/tests)
add_subdirectory(C/tests)
