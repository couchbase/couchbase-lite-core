cmake_minimum_required (VERSION 2.6)
project (LiteCore)

### BUILD SETTINGS:

set(COMPILE_FLAGS   "${COMPILE_FLAGS}   -Wall -Werror")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")

set(CMAKE_CXX_FLAGS_DEBUG   "${CMAKE_CXX_FLAGS_DEBUG}   -O0    -DDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -Ofast -DNDEBUG")

### SUBMODULES:

add_subdirectory(vendor/forestdb            EXCLUDE_FROM_ALL)
# Configure ForestDB features, and work around compiler warning about deprecated OSSpinLock API:
set_target_properties(forestdb PROPERTIES COMPILE_FLAGS "-DHAVE_GCC_ATOMICS=1 -D_CRYPTO_CC -D_DOC_COMP -Wno-deprecated-declarations")

add_subdirectory(vendor/fleece              EXCLUDE_FROM_ALL)
add_subdirectory(vendor/sqlite3-unicodesn   EXCLUDE_FROM_ALL)


include_directories("vendor/fleece/Fleece" 
                    "vendor/fleece/vendor" 
                    "vendor/forestdb/include/libforestdb"
                    "vendor/SQLiteCpp/include"
                    "vendor/sqlite3-unicodesn")

### MORE BUILD SETTINGS:

add_definitions(-DC4DB_THREADSAFE                   # Makes LiteCore C API thread-safe
                -DSQLITE_OMIT_LOAD_EXTENSION)       # This one's for SQLiteCpp

# Mac/iOS stuff
if (APPLE)
    # Specify that we want to use CommonCrypto:
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_CRYPTO_CC")
elseif(UNIX)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_CRYPTO_OPENSSL")
endif()

### SOURCE FILES:
              
include_directories(LiteCore/BlobStore LiteCore/Indexes LiteCore/RevTrees LiteCore/Storage
                    LiteCore/Support LiteCore/VersionVectors C/include)

# TODO: CMake docs say it's better to list the source files explicitly than use aux_source_directory
aux_source_directory(C                        C_SRC)
aux_source_directory(LiteCore/BlobStore       BLOBSTORE_SRC)
aux_source_directory(LiteCore/Indexes         INDEXES_SRC)
aux_source_directory(LiteCore/RevTrees        REVTREES_SRC)
aux_source_directory(LiteCore/Storage         STORAGE_SRC)
aux_source_directory(LiteCore/Support         SUPPORT_SRC)
aux_source_directory(LiteCore/VersionVectors  VERSIONVECTORS_SRC)

aux_source_directory(vendor/SQLiteCpp/src     SQLITECPP_SRC)

set_source_files_properties(${C_SRC} PROPERTIES COMPILE_FLAGS -Wno-return-type-c-linkage)

### BUILDING THE LIBRARY:

add_library(LiteCoreStatic STATIC  ${SUPPORT_SRC} ${STORAGE_SRC} ${INDEXES_SRC}
                              ${REVTREES_SRC} ${VERSIONVECTORS_SRC} ${BLOBSTORE_SRC} ${C_SRC}
                              ${SQLITECPP_SRC})

add_library(LiteCore  SHARED  )

target_link_libraries(LiteCore  LiteCoreStatic
                                FleeceStatic
                                forestdb 
                                sqlite3
                                SQLite3_UnicodeSN)

# Mac/iOS stuff
if (APPLE)
    set_target_properties(LiteCore PROPERTIES LINK_FLAGS
                          "-exported_symbols_list ${PROJECT_SOURCE_DIR}/C/c4.exp")
    target_link_libraries(LiteCore  "-framework CoreFoundation" 
                                    "-framework Foundation")
endif()

### TESTS:

add_subdirectory(LiteCore/tests)
add_subdirectory(C/tests)
