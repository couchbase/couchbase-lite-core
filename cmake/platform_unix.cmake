include(${CMAKE_CURRENT_LIST_DIR}/platform_base.cmake)

function(setup_globals_unix)
    set(CMAKE_C_FLAGS_MINSIZEREL "-Oz -DNDEBUG -g" CACHE INTERNAL "")
    set(CMAKE_CXX_FLAGS_MINSIZEREL "-Oz -DNDEBUG -g" CACHE INTERNAL "")
endfunction()

function(setup_litecore_build_unix)
    FILE(GLOB C_SRC LIST_DIRECTORIES FALSE "C/*.cc")
    set_source_files_properties(${C_SRC} PROPERTIES COMPILE_FLAGS -Wno-return-type-c-linkage)
    set_target_properties(LiteCore PROPERTIES CXX_VISIBILITY_PRESET hidden)
    set_target_properties(LiteCore PROPERTIES C_VISIBILITY_PRESET hidden)
endfunction()
