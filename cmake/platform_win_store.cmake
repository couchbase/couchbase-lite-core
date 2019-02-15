include("${CMAKE_CURRENT_LIST_DIR}/platform_win.cmake")

function(setup_litecore_build)
    setup_litecore_build_win()
    
    target_compile_definitions(
        LiteCoreStatic PRIVATE
        -DSQLITE_OS_WINRT               # Signal SQLite to use WinRT system calls instead of Win32
    )

    target_compile_definitions(
        mbedcrypto PRIVATE
        -DMBEDTLS_NO_PLATFORM_ENTROPY   # mbedcrypto entropy support does not account for Windows Store builds
    )

     # Enable Windows Runtime compilation
    set_target_properties(
        LiteCore PROPERTIES COMPILE_FLAGS 
        /ZW
    )
    
    # Remove the default Win32 libs from linking
    set(
        CMAKE_SHARED_LINKER_FLAGS 
        "${CMAKE_SHARED_LINKER_FLAGS} /nodefaultlib:kernel32.lib /nodefaultlib:ole32.lib"
        PARENT_SCOPE
    )
endfunction()