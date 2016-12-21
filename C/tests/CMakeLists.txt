cmake_minimum_required (VERSION 2.6)
project (C4Tests)

set(TOP ${PROJECT_SOURCE_DIR}/../../)

add_definitions(-DC4_TESTS)

include_directories(${TOP}C/include/
                    ${TOP}C/
                    ${TOP}vendor/fleece/Fleece/
                    ${TOP}vendor/fleece/vendor/catch/
                )
                    
aux_source_directory(.  TEST_SRC)
set(TEST_SRC ${TEST_SRC} ${TOP}/LiteCore/tests/main.cpp ${TOP}/LiteCore/Support/Logging.cc)
if(MSVC)
	set(TEST_SRC ${TEST_SRC} ${TOP}/MSVC/vasprintf-msvc.c)
endif()

add_executable(C4Tests ${TEST_SRC} )

target_link_libraries(C4Tests  LiteCore)
if(MSVC)
    set(BIN_TOP "${PROJECT_BINARY_DIR}/../..")
    set(FilesToCopy ${BIN_TOP}/\$\(Configuration\)/sqlite3
                    ${BIN_TOP}/\$\(Configuration\)/LiteCore)
    add_custom_command(TARGET C4Tests POST_BUILD
        COMMAND ${CMAKE_COMMAND}
        -DFilesToCopy="${FilesToCopy}"
        -DDestinationDirectory=${PROJECT_BINARY_DIR}/\$\(Configuration\)
        -P ${TOP}MSVC/copy_artifacts.cmake)
elseif(ANDROID)
    target_link_libraries(C4Tests "log")
elseif(UNIX AND NOT APPLE)
    target_link_libraries(C4Tests "pthread" c++abi)
endif()
