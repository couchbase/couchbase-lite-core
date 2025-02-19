cmake_minimum_required (VERSION 3.20.3)
project (LiteCoreToolSupport)

add_library( tool_support STATIC
    Tool.cc
    LiteCoreTool.cc
    ArgumentTokenizer.cc
    ../vendor/linenoise-mob/linenoise.c
    ../vendor/linenoise-mob/utf8.c
)

target_include_directories( tool_support PUBLIC
    .
    ../vendor/linenoise-mob/

    ../C/
    ../C/include/
    ../C/Cpp_include/
    ../LiteCore/Support/
    ../vendor/fleece/API/
    ../vendor/fleece/Fleece/Support/ # PlatformCompat.hh
)

target_compile_definitions(tool_support PRIVATE
    -DCMAKE
)
if(BUILD_ENTERPRISE)
    target_compile_definitions(tool_support PRIVATE
        -DCOUCHBASE_ENTERPRISE
    )
endif()

target_link_libraries(tool_support INTERFACE
    FleeceStatic
)
