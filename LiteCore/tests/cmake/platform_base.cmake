function(set_source_files_base)
    set(oneValueArgs RESULT)
    cmake_parse_arguments(BASE_SSS "" "${oneValueArgs}" "" ${ARGN})
    if(NOT DEFINED BASE_SSS_RESULT)
        message(FATAL_ERROR "set_source_files_base needs to be called with RESULT")
    endif()

    set(
        ${BASE_SSS_RESULT}
        c4BaseTest.cc
        DataFileTest.cc
        DocumentKeysTest.cc
        FTSTest.cc
        LiteCoreTest.cc
        LogEncoderTest.cc
        PredictiveQueryTest.cc
        QueryParserTest.cc
        QueryTest.cc
        RevTreeTest.cc
        SequenceTrackerTest.cc
        SQLiteFunctionsTest.cc
        UpgraderTest.cc
        ${TOP}REST/tests/RESTListenerTest.cc
        ${TOP}REST/tests/SyncListenerTest.cc
        ${TOP}vendor/fleece/Tests/API_ValueTests.cc
        ${TOP}vendor/fleece/Tests/DeltaTests.cc
        ${TOP}vendor/fleece/Tests/EncoderTests.cc
        ${TOP}vendor/fleece/Tests/FleeceTests.cc
        ${TOP}vendor/fleece/Tests/HashTreeTests.cc
        ${TOP}vendor/fleece/Tests/JSON5Tests.cc
        ${TOP}vendor/fleece/Tests/MutableTests.cc
        ${TOP}vendor/fleece/Tests/PerfTests.cc
        ${TOP}vendor/fleece/Tests/SharedKeysTests.cc
        ${TOP}vendor/fleece/Tests/SupportTests.cc
        ${TOP}vendor/fleece/Tests/ValueTests.cc
        ${TOP}vendor/fleece/Experimental/KeyTree.cc
        ${TOP}Replicator/tests/ReplicatorLoopbackTest.cc
        ${TOP}Replicator/tests/ReplicatorAPITest.cc
        ${TOP}Replicator/tests/ReplicatorSGTest.cc
        ${TOP}C/tests/c4Test.cc 
        ${TOP}Replicator/tests/CookieStoreTest.cc
        ${TOP}REST/Response.cc
        main.cpp
        PARENT_SCOPE
    )
endfunction()