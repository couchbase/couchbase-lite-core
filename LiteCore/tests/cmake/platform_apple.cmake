function(setup_build)
    target_sources(
        CppTests PRIVATE
        ${TOP}vendor/fleece/Tests/ObjCTests.mm
    )
endfunction()