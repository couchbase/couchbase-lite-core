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
             ${TOP}/LiteCore/tests/main.cpp
             ${TOP}/LiteCore/Support/StringUtil.cc
             ${TOP}/LiteCore/Support/FilePath.cc
             ${TOP}/LiteCore/Support/Logging_Stub.cc
             ${TOP}/REST/Response.cc)

if(NOT "${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")
  # I give up trying to explain why Linux is the way it is...
  set(TEST_SRC ${TEST_SRC}
    ${TOP}/LiteCore/Support/Error.cc
    ${TOP}/LiteCore/Support/Error_android.cc
    ${TOP}/LiteCore/Support/Error_windows.cc)
endif()
if(MSVC)
	set(TEST_SRC ${TEST_SRC} ${TOP}/MSVC/vasprintf-msvc.c ${TOP}/LiteCore/Support/PlatformIO.cc ${TOP}/MSVC/strlcat.c ${TOP}/MSVC/mkdtemp.cc ${TOP}/MSVC/mkstemp.cc)
endif()

add_executable(C4Tests ${TEST_SRC} )

target_link_libraries(C4Tests  LiteCore LiteCoreREST CivetWeb FleeceStatic)
file(COPY data DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/C/tests)

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
