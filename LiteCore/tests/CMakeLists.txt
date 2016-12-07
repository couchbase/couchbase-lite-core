cmake_minimum_required (VERSION 2.6)
project (CppTests)

set(TOP ${PROJECT_SOURCE_DIR}/../../)

add_definitions(-DLITECORE_CPP_TESTS=1 -D_USE_MATH_DEFINES)

include_directories(${TOP}LiteCore/BlobStore/ ${TOP}LiteCore/Indexes/ ${TOP}LiteCore/RevTrees/
                    ${TOP}LiteCore/Storage/ ${TOP}LiteCore/Support/ ${TOP}LiteCore/VersionVectors/
                    ${TOP}vendor/fleece/Fleece/
                    ${TOP}vendor/fleece/vendor/catch/
                    ${TOP}vendor/SQLiteCpp/include/
                )
                    
aux_source_directory(.  TEST_SRC)
if(WIN32)
	set(TEST_SRC ${TEST_SRC} ../../MSVC/arc4random.c)
elseif(ANDROID)
  set(TEST_SRC ${TEST_SRC} ../Android/arc4random.cc)
endif()

add_executable(CppTests ${TEST_SRC})

target_link_libraries(CppTests  LiteCoreStatic
                                FleeceStatic
                                sqlite3
                                SQLite3_UnicodeSN)
if(NOT APPLE)
    if(UNIX AND NOT ANDROID)
        target_link_libraries(CppTests  crypto  bsd)
    elseif(ANDROID)
        target_link_libraries(CppTests ${OPENSSL_CRYPTO_LIBRARY} "atomic" "log")
    else()
        target_link_libraries(CppTests ${OPENSSL_CRYPTO_LIBRARY} ws2_32 )
        if(MSVC)
            set(FilesToCopy ${TOP}build_cmake/\$\(Configuration\)/sqlite3)
            add_custom_command(TARGET CppTests POST_BUILD
                COMMAND ${CMAKE_COMMAND}
                -DFilesToCopy="${FilesToCopy}"
                -DDestinationDirectory=${PROJECT_BINARY_DIR}/\$\(Configuration\)
                -P ${TOP}MSVC/copy_artifacts.cmake)
        endif()
    endif()
endif()

