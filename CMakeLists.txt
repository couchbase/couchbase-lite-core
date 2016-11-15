cmake_minimum_required (VERSION 2.6)
project (LiteCore)

### BUILD SETTINGS:

set(COMPILE_FLAGS   "${COMPILE_FLAGS}   -Wall -Werror")

if(NOT MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
endif()

if(NOT MSVC)
    set(CMAKE_CXX_FLAGS_DEBUG   "${CMAKE_CXX_FLAGS_DEBUG}   -O0    -DDEBUG")
else()
    set(CMAKE_CXX_FLAGS_DEBUG   "${CMAKE_CXX_FLAGS_DEBUG} -DDEBUG")
endif()

set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -Ofast -DNDEBUG")

if(WIN32)
    set(_ENCRYPTION openssl)
endif()

### SUBMODULES:

add_subdirectory(vendor/forestdb            EXCLUDE_FROM_ALL)
add_subdirectory(vendor/fleece              EXCLUDE_FROM_ALL)
add_subdirectory(vendor/sqlite3-unicodesn   EXCLUDE_FROM_ALL)
if(WIN32)
    add_subdirectory(vendor/SQLiteCpp/sqlite3 EXCLUDE_FROM_ALL)
endif()


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
    # Configure ForestDB features, and work around compiler warning about deprecated OSSpinLock API:
    set_target_properties(forestdb PROPERTIES COMPILE_FLAGS "-DHAVE_GCC_ATOMICS=1 -D_CRYPTO_CC -D_DOC_COMP -Wno-deprecated-declarations")

elseif(UNIX)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_CRYPTO_OPENSSL")
    set_target_properties(forestdb PROPERTIES COMPILE_FLAGS "-DHAVE_GCC_ATOMICS=1 -D_CRYPTO_OPENSSL -D_DOC_COMP")
else()
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_CRYPTO_OPENSSL -D_USE_MATH_DEFINES -DLITECORE_EXPORTS -DWIN32")
    set_target_properties(forestdb PROPERTIES COMPILE_FLAGS "-DHAVE_GCC_ATOMICS=1 -D_CRYPTO_OPENSSL")
    target_include_directories(forestdb PRIVATE "vendor/openssl/libs/include")
    include_directories("vendor/openssl/libs/include"
                        "vendor/SQLiteCpp/sqlite3"
			"MSVC")
    target_compile_options(forestdb PRIVATE /EHsc -DWIN32)
endif()

### SOURCE FILES:
              
include_directories(LiteCore/BlobStore LiteCore/Database LiteCore/Indexes LiteCore/RevTrees LiteCore/Storage
                    LiteCore/Support LiteCore/VersionVectors C/include C)

# TODO: CMake docs say it's better to list the source files explicitly than use aux_source_directory
aux_source_directory(C                        C_SRC)
aux_source_directory(LiteCore/BlobStore       BLOBSTORE_SRC)
aux_source_directory(LiteCore/Database        DATABASE_SRC)
aux_source_directory(LiteCore/Indexes         INDEXES_SRC)
aux_source_directory(LiteCore/RevTrees        REVTREES_SRC)
aux_source_directory(LiteCore/Storage         STORAGE_SRC)
aux_source_directory(LiteCore/VersionVectors  VERSIONVECTORS_SRC)

set(SUPPORT_SRC "LiteCore/Support/Collatable.cc"
		"LiteCore/Support/EncryptedStream.cc"
		"LiteCore/Support/Error.cc"
		"LiteCore/Support/FilePath.cc"
		"LiteCore/Support/ForestDBMain.c"
		"LiteCore/Support/Logging.cc"
		"LiteCore/Support/RefCounted.cc")

aux_source_directory(vendor/SQLiteCpp/src     SQLITECPP_SRC)

if(!WIN32)
    set_source_files_properties(${C_SRC} PROPERTIES COMPILE_FLAGS -Wno-return-type-c-linkage)
else()
    
endif()

### BUILDING THE LIBRARY:

set(ALL_SRC_FILES ${SUPPORT_SRC} ${STORAGE_SRC} ${INDEXES_SRC}
                              ${REVTREES_SRC} ${VERSIONVECTORS_SRC} ${BLOBSTORE_SRC} ${C_SRC}
                              ${SQLITECPP_SRC} ${DATABASE_SRC})
							  
if(WIN32)
	set(ALL_SRC_FILES ${ALL_SRC_FILES} MSVC/asprintf.c
			     MSVC/memmem.cc MSVC/mkstemps.cc MSVC/strlcat.c MSVC/strlcat.c
			     MSVC/vasprintf-msvc.c)
endif()

add_library(LiteCoreStatic STATIC ${ALL_SRC_FILES})
file(WRITE empty.cpp)
add_library(LiteCore SHARED empty.cpp)

target_link_libraries(LiteCore PRIVATE LiteCoreStatic
								FleeceStatic 
                                sqlite3
                                SQLite3_UnicodeSN)
								
target_link_libraries(LiteCore PUBLIC forestdb)
		   

# Mac/iOS stuff
if (APPLE)
    set_target_properties(LiteCore PROPERTIES LINK_FLAGS
                          "-exported_symbols_list ${PROJECT_SOURCE_DIR}/C/c4.exp")
    target_link_libraries(LiteCore  "-framework CoreFoundation" 
                                    "-framework Foundation")
elseif(UNIX)
    target_link_libraries(LiteCore  crypto  bsd)
else()
    set_target_properties(LiteCore PROPERTIES LINK_FLAGS
			  "/def:${PROJECT_SOURCE_DIR}/C/c4.def")	
    target_link_libraries(LiteCore PRIVATE ${OPENSSL_LIBRARIES} Ws2_32)
endif()

### TESTS:

add_subdirectory(LiteCore/tests)
add_subdirectory(C/tests)
