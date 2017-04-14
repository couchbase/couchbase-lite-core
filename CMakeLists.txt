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

add_definitions(-DCMAKE)
set_property(DIRECTORY APPEND PROPERTY COMPILE_DEFINITIONS
    $<$<CONFIG:Debug>:DEBUG>
)
if(MSVC)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc /wd4068 /wd4244 /wd4018 /wd4819 -D_CRT_SECURE_NO_WARNINGS=1")
endif()

if("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")
    if(NOT "${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
        message(WARNING "${CMAKE_CXX_COMPILER_ID} is not supported for building!")
    else()
      set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
    endif()
endif()

if(WIN32 OR ANDROID)
    if(${CMAKE_GENERATOR} STREQUAL "Visual Studio 14 2015")
        set(OPENSSL_ROOT_DIR ${PROJECT_SOURCE_DIR}/vendor/openssl/libs/windows/x86/)
    elseif(${CMAKE_GENERATOR} STREQUAL "Visual Studio 14 2015 Win64")
        set(OPENSSL_ROOT_DIR ${PROJECT_SOURCE_DIR}/vendor/openssl/libs/windows/amd64/)
    elseif(ANDROID)
        set(OPENSSL_ROOT_DIR ${PROJECT_SOURCE_DIR}/vendor/openssl/libs/android/clang/${CMAKE_ANDROID_ARCH_ABI}/)
        set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--exclude-libs,libgcc.a")
    endif()
    set(OPENSSL_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/vendor/openssl/libs/include)
    set(SQLITE3_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/vendor/SQLiteCpp/sqlite3)
    if(ANDROID)
        set(OPENSSL_CRYPTO_LIBRARY ${OPENSSL_ROOT_DIR}/libcrypto.a)
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
                    "vendor/BLIP-Cpp/src/util")

if(WIN32)
  include_directories("MSVC")
elseif(ANDROID)
  include_directories("LiteCore/Android")
endif()

add_subdirectory(vendor/fleece              EXCLUDE_FROM_ALL)
add_subdirectory(vendor/sqlite3-unicodesn   EXCLUDE_FROM_ALL)
add_subdirectory(vendor/BLIP-Cpp   EXCLUDE_FROM_ALL)

if(WIN32 OR ANDROID OR LITECORE_BUILD_SQLITE)
    set(SQLITECPP_INTERNAL_SQLITE OFF)
    add_library(sqlite3 SHARED
     vendor/SQLiteCpp/sqlite3/sqlite3.c
     vendor/SQLiteCpp/sqlite3/sqlite3.h
    )
    set_target_properties(sqlite3 PROPERTIES COMPILE_FLAGS "-DSQLITE_ENABLE_FTS4_UNICODE61 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_FTS3_TOKENIZER")
    if(WIN32)
        set_target_properties(sqlite3 PROPERTIES LINK_FLAGS
                "/def:\"${CMAKE_CURRENT_LIST_DIR}/MSVC/sqlite3.def\"")	
    endif()
endif()

### MORE BUILD SETTINGS:

add_definitions(-DC4DB_THREADSAFE                   # Makes LiteCore C API thread-safe
                -DSQLITE_OMIT_LOAD_EXTENSION)       # This one's for SQLiteCpp

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
aux_source_directory(vendor/SQLiteCpp/src     SQLITECPP_SRC)
aux_source_directory(Replicator               REPLICATOR_SRC)
if (APPLE)
    include_directories("Obj-C")
    set(REPLICATOR_SRC ${REPLICATOR_SRC}
      "Replicator/Obj-C/CBLWebSocket.mm"
      "Replicator/Obj-C/CBLHTTPLogic.m"
    )
endif()

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
			     MSVC/vasprintf-msvc.c MSVC/arc4random.cc)
elseif(ANDROID)
  set(ALL_SRC_FILES ${ALL_SRC_FILES} LiteCore/Android/strlcat.c)
endif()

add_library(LiteCoreStatic STATIC ${ALL_SRC_FILES})
file(WRITE empty.cpp)
add_library(LiteCore SHARED empty.cpp)

if(UNIX AND NOT APPLE)
    set(WHOLE_LIBRARY_FLAG "-Wl,--whole-archive")
    set(NO_WHOLE_LIBRARY_FLAG "-Wl,--no-whole-archive")
endif()

target_link_libraries(LiteCore PRIVATE ${WHOLE_LIBRARY_FLAG} LiteCoreStatic
								FleeceStatic 
                SQLite3_UnicodeSN BLIPStatic ${NO_WHOLE_LIBRARY_FLAG})
								
target_link_libraries(LiteCore PUBLIC sqlite3)
		   

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
    set_target_properties(LiteCore PROPERTIES LINK_FLAGS
			  "/def:${PROJECT_SOURCE_DIR}/C/c4.def /ignore:4099")	
    target_link_libraries(LiteCore PRIVATE ${OPENSSL_CRYPTO_LIBRARY} zlibstatic Ws2_32)
endif()

### JNI:

if (JNI)
    add_subdirectory(Java)
endif()

### TESTS:

add_subdirectory(LiteCore/tests)
add_subdirectory(C/tests)
