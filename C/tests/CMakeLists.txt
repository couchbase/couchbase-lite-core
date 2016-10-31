cmake_minimum_required (VERSION 2.6)
project (C4Tests)

set(TOP ../../)

add_definitions(-DDEBUG)

include_directories(${TOP}C/include/
                    ${TOP}C/
                    ${TOP}vendor/fleece/Fleece/
                    ${TOP}vendor/fleece/vendor/catch/
                )
                    
aux_source_directory(.  TEST_SRC)

add_executable(C4Tests ${TEST_SRC} ${TOP}/LiteCore/tests/main.cpp)

target_link_libraries(C4Tests  LiteCore)
