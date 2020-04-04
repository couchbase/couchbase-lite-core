function(register_litecore_target TARGET_NAME)

    string(TOUPPER ${TARGET_NAME} UPPER_TARGET_NAME)
    set(COMPILER_FLAGS ${${UPPER_TARGET_NAME}_FLAGS})
    set(C_FLAGS ${${UPPER_TARGET_NAME}_C_FLAGS})
    set(CXX_FLAGS ${${UPPER_TARGET_NAME}_CXX_FLAGS})

    if(NOT "${COMPILER_FLAGS}" STREQUAL "")
        message(STATUS "Detected custom compiler flags for ${TARGET_NAME}: ${COMPILER_FLAGS}")
    endif()

    if(NOT "${C_FLAGS}" STREQUAL "")
        message(STATUS "Detected custom C compiler flags for ${TARGET_NAME}: ${C_FLAGS}")
    endif()

    if(NOT "${CXX_FLAGS}" STREQUAL "")
        message(STATUS "Detected custom CXX compiler flags for ${TARGET_NAME}: ${CXX_FLAGS}")
    endif()

    target_compile_options(
        ${TARGET_NAME} PRIVATE
        ${COMPILER_FLAGS}
        $<$<COMPILE_LANGUAGE:CXX>:${${CXX_FLAGS}}>
        $<$<COMPILE_LANGUAGE:C>:${C_FLAGS}>
    )

    set(DEFINITIONS ${${UPPER_TARGET_NAME}_COMPILER_DEFINITIONS})
    if(NOT "${DEFINITIONS}" STREQUAL "")
        message(STATUS "Detected custom preprocessor definitions for ${TARGET_NAME}: ${DEFINITIONS}")
    endif()

    target_compile_definitions(
        ${TARGET_NAME} PRIVATE
        ${DEFINITIONS}
    )
endfunction()
