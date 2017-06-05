cmake_minimum_required (VERSION 2.6)
project (CppTests)

set(TOP ${PROJECT_SOURCE_DIR}/../../)

add_definitions(-DLITECORE_CPP_TESTS=1 -D_USE_MATH_DEFINES)

include_directories(${TOP}LiteCore/BlobStore/ ${TOP}LiteCore/Indexes/ ${TOP}LiteCore/RevTrees/
                    ${TOP}LiteCore/Storage/ ${TOP}LiteCore/Support/ ${TOP}LiteCore/VersionVectors/
                    ${TOP}LiteCore/
                    ${TOP}vendor/fleece/Fleece/
                    ${TOP}vendor/fleece/vendor/catch/
                    ${TOP}vendor/SQLiteCpp/include/
                    ${TOP}Replicator/
                    ${TOP}C/tests/
                )
                    
aux_source_directory(.  TEST_SRC)
if(MSVC)
	set(TEST_SRC ${TEST_SRC} ../../MSVC/arc4random.cc)
elseif(ANDROID)
  set(TEST_SRC ${TEST_SRC} ../Android/arc4random.cc)
endif()

file(COPY data DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/LiteCore/tests)
file(COPY ../../C/tests/data DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/C/tests)
add_executable(CppTests ${TEST_SRC} ../../Replicator/tests/ReplicatorLoopbackTest.cc 
  ../../C/tests/c4Test.cc)

target_link_libraries(CppTests  LiteCoreStatic
                                FleeceStatic
                                sqlite3
                                SQLite3_UnicodeSN
                                BLIPStatic
                                CivetWeb)

if(APPLE)
    target_link_libraries(CppTests  "-framework Foundation"
                                    "-framework CFNetwork"
                                    "z")
else()
    if(UNIX AND NOT ANDROID)
        target_link_libraries(CppTests  crypto  bsd "${LIBCXX_LIB}" "${LIBCXXABI_LIB}" z pthread atomic dl)
    elseif(ANDROID)
        target_link_libraries(CppTests ${OPENSSL_CRYPTO_LIBRARY} "atomic" "log" "z")
    else()
        set(BIN_TOP "${PROJECT_BINARY_DIR}/../..")
        target_link_libraries(CppTests ${OPENSSL_CRYPTO_LIBRARY} ws2_32 zlibstatic)
        set(FilesToCopy ${BIN_TOP}/\$\(Configuration\)/sqlite3)
        add_custom_command(TARGET CppTests POST_BUILD
            COMMAND ${CMAKE_COMMAND}
            -DFilesToCopy="${FilesToCopy}"
            -DDestinationDirectory=${PROJECT_BINARY_DIR}/\$\(Configuration\)
            -P ${TOP}MSVC/copy_artifacts.cmake)
    endif()
else()
  target_link_libraries(CppTests  z)
endif()

