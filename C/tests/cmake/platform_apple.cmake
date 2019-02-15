include("${CMAKE_CURRENT_LIST_DIR}/platform_base.cmake")

function(set_source_files)
    set(oneValueArgs RESULT)
    cmake_parse_arguments(APPLE_SSS "" "${oneValueArgs}" "" ${ARGN})
    if(NOT DEFINED APPLE_SSS_RESULT)
        message(FATAL_ERROR "set_source_files_base needs to be called with RESULT")
    endif()

    set_source_files_base(RESULT BASE_SRC_FILES)
    set(
        ${APPLE_SSS_RESULT}
        ${BASE_SRC_FILES}
        c4PredictiveQueryTest+CoreML.mm
        CoreMLPredictiveModel.mm
        PARENT_SCOPE
    )
endfunction()

function(setup_build)
    if(BUILD_ENTERPRISE)
        target_link_libraries(
            C4Tests PRIVATE  
            "-framework CoreML"
            "-framework Vision"
        )
    endif()
endfunction()