SEPARATE_ARGUMENTS(FilesToCopy)
foreach(file ${FilesToCopy})
    message("Copying ${file}.dll to ${DestinationDirectory}")
    execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
    ${file}.dll 
    ${DestinationDirectory})
    if(EXISTS ${file}.pdb)
        message("Copying ${file}.pdb to ${DestinationDirectory}")
        execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${file}.pdb 
        ${DestinationDirectory})
    endif()
endforeach()