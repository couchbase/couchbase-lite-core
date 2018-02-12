cmake_minimum_required (VERSION 2.6)
project (C4Tests)

set(TOP ${PROJECT_SOURCE_DIR}/../../)

add_definitions(-DC4_TESTS)

include_directories(${TOP}C/include/
                    ${TOP}C/tests/
                    ${TOP}C/
                    ${TOP}REST/
                    ${TOP}vendor/fleece/Fleece/
                    ${TOP}vendor/civetweb/include/
                    ${TOP}vendor/fleece/vendor/catch/
                )
                    
aux_source_directory(.  TEST_SRC)
aux_source_directory(${TOP}/REST/tests  REST_TEST_SRC)

set(TEST_SRC ${TEST_SRC} 
             ${REST_TEST_SRC}
             ${TOP}/Replicator/tests/ReplicatorAPITest.cc
             ${TOP}/LiteCore/tests/main.cpp
             ${TOP}/LiteCore/Support/StringUtil.cc
             ${TOP}/LiteCore/Support/FilePath.cc
             ${TOP}/LiteCore/Support/Logging_Stub.cc
             ${TOP}/REST/Response.cc)

add_executable(C4Tests ${TEST_SRC} )

target_link_libraries(C4Tests  LiteCore LiteCoreREST CivetWeb FleeceStatic Support)
file(COPY data DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/C/tests)
file(COPY ../../LiteCore/tests/data/replacedb/ios120/iosdb.cblite2 
  DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/LiteCore/tests/data/replacedb/ios120)

if(MSVC)
    set(BIN_TOP "${PROJECT_BINARY_DIR}/../..")
    set(FilesToCopy ${BIN_TOP}/\$\(Configuration\)/LiteCore
                    ${BIN_TOP}/\$\(Configuration\)/LiteCoreREST)
    add_custom_command(TARGET C4Tests POST_BUILD
        COMMAND ${CMAKE_COMMAND}
        -DFilesToCopy="${FilesToCopy}"
        -DDestinationDirectory=${PROJECT_BINARY_DIR}/\$\(Configuration\)
        -P ${TOP}MSVC/copy_artifacts.cmake)
elseif(ANDROID)
    target_link_libraries(C4Tests "log")
elseif(UNIX AND NOT APPLE)
    target_link_libraries(C4Tests "pthread" "${LIBCXX_LIB}" "${LIBCXXABI_LIB}" dl)
endif()
