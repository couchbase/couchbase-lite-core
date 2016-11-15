cmake_minimum_required (VERSION 2.6)
project (C4Tests)

set(TOP ../../)

add_definitions(-DDEBUG -DC4_TESTS)

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
