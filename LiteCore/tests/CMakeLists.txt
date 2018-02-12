cmake_minimum_required (VERSION 2.6)
project (CppTests)

set(TOP ${PROJECT_SOURCE_DIR}/../../)

add_definitions(-DLITECORE_CPP_TESTS=1 -D_USE_MATH_DEFINES)

include_directories(${TOP}LiteCore/BlobStore/ ${TOP}LiteCore/Indexes/ ${TOP}LiteCore/RevTrees/
                    ${TOP}LiteCore/Storage/ ${TOP}LiteCore/Support/ ${TOP}LiteCore/VersionVectors/
                    ${TOP}LiteCore/ ${TOP}REST/
                    ${TOP}tools/
                    ${TOP}vendor/fleece/Fleece/
                    ${TOP}vendor/fleece/vendor/catch/
                    ${TOP}vendor/fleece/vendor/jsonsl/
                    ${TOP}vendor/SQLiteCpp/include/
                    ${TOP}Replicator/
                    ${TOP}C/tests/
                )

aux_source_directory(.  TEST_SRC)
aux_source_directory(${TOP}vendor/fleece/Tests FLEECE_TEST_SRC)
LIST(REMOVE_ITEM FLEECE_TEST_SRC ${TOP}vendor/fleece/Tests/FleeceTestsMain.cc)
if(NOT APPLE)
    LIST(REMOVE_ITEM FLEECE_TEST_SRC ${TOP}vendor/fleece/Tests/ObjCTests.mm)
endif()
set(TEST_SRC ${TEST_SRC} ${FLEECE_TEST_SRC} ${TOP}/tools/ArgumentTokenizer.cc)

file(COPY data DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/LiteCore/tests)
file(COPY ../../C/tests/data DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/C/tests)
file(COPY ../../vendor/fleece/Tests/1000people.json DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/Tests)
file(COPY ../../vendor/fleece/Tests/1person.fleece DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/Tests)
file(COPY ../../vendor/fleece/Tests/1000people.json DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/Tests)
add_executable(CppTests ${TEST_SRC} ../../Replicator/tests/ReplicatorLoopbackTest.cc
  ../../C/tests/c4Test.cc ../../Replicator/tests/CookieStoreTest.cc
  ../../REST/Response.cc)

target_link_libraries(CppTests  LiteCoreStatic
                                FleeceStatic
                                mbedcrypto
                                SQLite3_UnicodeSN
                                BLIPStatic
                                CivetWeb
                                Support)

if(APPLE)
    target_link_libraries(CppTests  "-framework Foundation"
                                    "-framework CFNetwork"
                                    "-framework Security"
                                    "z")
else()
    if(UNIX AND NOT ANDROID)
        target_link_libraries(CppTests "${LIBCXX_LIB}" "${LIBCXXABI_LIB}" "${ICU4C_COMMON}" "${ICU4C_I18N}" z pthread dl)
    elseif(ANDROID)
        target_link_libraries(CppTests "atomic" "log" zlibstatic)
    else()
        set(BIN_TOP "${PROJECT_BINARY_DIR}/../..")
        target_link_libraries(CppTests ws2_32 zlibstatic)
    endif()
endif()

