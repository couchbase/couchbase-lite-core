function(set_source_files_base)
    set(oneValueArgs RESULT)
    cmake_parse_arguments(BASE_SSS "" "${oneValueArgs}" "" ${ARGN})
    if(NOT DEFINED BASE_SSS_RESULT)
        message(FATAL_ERROR "set_source_files_base needs to be called with RESULT")
    endif()

    set(
        ${BASE_SSS_RESULT}
        c4AllDocsPerformanceTest.cc
        c4BlobStoreTest.cc
        c4DatabaseInternalTest.cc
        c4DatabaseTest.cc
        c4DocumentTest.cc
        c4ObserverTest.cc
        c4PerfTest.cc
        c4QueryTest.cc
        c4Test.cc
        c4ThreadingTest.cc
        # EE tests:
        c4CertificateTest.cc
        ${TOP}LiteCore/tests/main.cpp
        PARENT_SCOPE
    )
endfunction()
