function(setup_build)
    target_sources(
        C4Tests PRIVATE
        c4PredictiveQueryTest+CoreML.mm
        CoreMLPredictiveModel.mm
    )

    target_link_libraries(
        C4Tests PRIVATE
        "-framework Foundation"
    )

    if(BUILD_ENTERPRISE)
        target_link_libraries(
            C4Tests PRIVATE
            "-framework CoreML"
            "-framework Vision"
        )
    endif()
endfunction()
