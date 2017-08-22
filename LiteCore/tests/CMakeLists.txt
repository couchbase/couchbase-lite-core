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
elseif(UNIX AND NOT APPLE)
  set(TEST_SRC ${TEST_SRC} ../Android/arc4random.cc)
endif()

file(COPY data DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/LiteCore/tests)
file(COPY ../../C/tests/data DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/C/tests)
add_executable(CppTests ${TEST_SRC} ../../Replicator/tests/ReplicatorLoopbackTest.cc 
  ../../C/tests/c4Test.cc)

target_link_libraries(CppTests  LiteCoreStatic
                                FleeceStatic
                                sqlcipher
                                mbedcrypto
                                SQLite3_UnicodeSN
                                BLIPStatic
                                CivetWeb)

if(APPLE)
    target_link_libraries(CppTests  "-framework Foundation"
                                    "-framework CFNetwork"
                                    "-framework Security"
                                    "z")
else()
    if(UNIX AND NOT ANDROID)
        target_link_libraries(CppTests "${LIBCXX_LIB}" "${LIBCXXABI_LIB}" "${ICU4C_COMMON}" "${ICU4C_I18N}" z pthread dl)
    elseif(ANDROID)
        target_link_libraries(CppTests "atomic" "log" "z")
    else()
        set(BIN_TOP "${PROJECT_BINARY_DIR}/../..")
        target_link_libraries(CppTests ws2_32 zlibstatic Dbghelp.lib)
    endif()
endif()

