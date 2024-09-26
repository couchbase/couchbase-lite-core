#[[
LiteCore Static Library Tests CMake Project

This project builds a test runner that links against the static LiteCore library to
verify that the inner logic is working correctly (i.e. non exported functions, etc).
It uses some predefined data as follows:

- Everything in the 'data' directory
- Everything in the '../../C/tests/data' directory
- Fleece and JSON data files from '../../vendor/fleece/Tests'

The test runner depends on multiple other libraries from multiple git repositories:

- LiteCore static library (../..)
- BLIP object library (../../Networking/BLIP)

In addition, there are also some system dependencies that are not listed here
]]#
cmake_minimum_required (VERSION 3.9)
project (CppTests)

if(ANDROID OR WINDOWS_STORE)
    # No sense building it for Android since using an Android executable is
    # tricky and strange.  Windows Store projects are also not supported here.
    return()
endif()

if(NOT LITECORE_BUILD_TESTS)
    return()
endif()

# This project is not standalone.  Point to the "root" directory
set(TOP ${PROJECT_SOURCE_DIR}/../../)

if(MSVC)
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/platform_win.cmake")
elseif(APPLE)
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/platform_apple.cmake")
elseif("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/platform_linux.cmake")
else()
    message(FATAL_ERROR "Unsupported platform ${CMAKE_SYSTEM_NAME}")
endif()

# Copy data files to output directory
file(COPY data DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/LiteCore/tests)
file(COPY ../../Replicator/tests/data/cert DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/Replicator/tests/data/)
file(COPY ../../C/tests/data DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/C/tests)
file(GLOB FLEECE_FILES "../../vendor/fleece/Tests/*.json" "../../vendor/fleece/Tests/*.json5" "../../vendor/fleece/Tests/*.fleece" "../../vendor/fleece/Tests/*.txt")
file(COPY ${FLEECE_FILES} DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/vendor/fleece/Tests)
add_executable(
    CppTests
    ResultTest.cc
    c4BaseTest.cc
    c4DocumentTest_Internal.cc
    DataFileTest.cc
    DocumentKeysTest.cc
    LiteCoreTest.cc
    LogEncoderTest.cc
    N1QLParserTest.cc
    QueryTranslatorTest.cc
    QueryTest.cc
    FTSTest.cc
    ArrayQueryTest.cc
    VectorQueryTest.cc
    PredictiveQueryTest.cc
    PredictiveVectorQueryTest.cc
    SequenceSetTest.cc
    SQLiteFunctionsTest.cc
    SequenceTrackerTest.cc
    UpgraderTest.cc
    VectorQueryTest.cc
    LazyVectorQueryTest.cc
    VersionVectorTest.cc
    ${TOP}REST/tests/RESTListenerTest.cc
    ${TOP}REST/tests/RESTClientTest.cc
    ${TOP}REST/tests/SyncListenerTest.cc
    ${TOP}vendor/fleece/Tests/API_ValueTests.cc
    ${TOP}vendor/fleece/Tests/DeltaTests.cc
    ${TOP}vendor/fleece/Tests/EncoderTests.cc
    ${TOP}vendor/fleece/Tests/FleeceTests.cc
    ${TOP}vendor/fleece/Tests/JSON5Tests.cc
    ${TOP}vendor/fleece/Tests/MutableTests.cc
    ${TOP}vendor/fleece/Tests/PerfTests.cc
    ${TOP}vendor/fleece/Tests/SharedKeysTests.cc
    ${TOP}vendor/fleece/Tests/SupportTests.cc
    ${TOP}vendor/fleece/Tests/ValueTests.cc
    ${TOP}Replicator/tests/DBAccessTestWrapper.cc
    ${TOP}Replicator/tests/PropertyEncryptionTests.cc
    ${TOP}Replicator/tests/ReplicatorLoopbackTest.cc
    ${TOP}Replicator/tests/ReplicatorAPITest.cc
    ${TOP}Replicator/tests/ReplicatorSGTest.cc
    ${TOP}Replicator/tests/ReplicatorCollectionTest.cc
    ${TOP}Replicator/tests/ReplicatorCollectionSGTest.cc
    ${TOP}Replicator/tests/ReplicatorSG30Test.cc
    ${TOP}Replicator/tests/ReplicatorVVUpgradeTest.cc
    ${TOP}Replicator/tests/SG.cc
    ${TOP}Replicator/tests/SGTestUser.cc
    ${TOP}Replicator/tests/ReplParams.cc
    ${TOP}C/tests/c4Test.cc
    ${TOP}Replicator/tests/CookieStoreTest.cc
    ${TOP}Crypto/CertificateTest.cc
    ${TOP}LiteCore/Support/TestsCommon.cc
    main.cpp
)

get_directory_property(this_targets DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} BUILDSYSTEM_TARGETS)
set(LITECORE_TARGETS ${LITECORE_TARGETS} ${this_targets} PARENT_SCOPE)
setup_build()

target_compile_definitions(
    CppTests PRIVATE
    -DLITECORE_CPP_TESTS=1
    -D_USE_MATH_DEFINES     # Define math constants like PI
    -DNOMINMAX              # Get rid of min/max macros that interfere with std::min/std::max
    -DCATCH_CONFIG_CPP11_STREAM_INSERTABLE_CHECK
)

target_include_directories(
    CppTests PRIVATE
    ${TOP}vendor/fleece/API
    ${TOP}vendor/fleece/Experimental
    ${TOP}vendor/fleece/Fleece/Core
    ${TOP}vendor/fleece/Fleece/Mutable
    ${TOP}vendor/fleece/Fleece/Support
    ${TOP}vendor/fleece/Fleece/Tree
    ${TOP}vendor/fleece/vendor/catch
    ${TOP}vendor/fleece/vendor/jsonsl
    ${TOP}vendor/SQLiteCpp/include
    ${TOP}vendor/SQLiteCpp/sqlite3
    ${TOP}C
    ${TOP}C/include
    ${TOP}C/Cpp_include
    ${TOP}C/tests
    ${TOP}Crypto
    ${TOP}Networking
    ${TOP}Networking/BLIP
    ${TOP}Networking/HTTP
    ${TOP}Networking/WebSockets
    ${TOP}LiteCore/BlobStore
    ${TOP}LiteCore/Database
    ${TOP}LiteCore/RevTrees
    ${TOP}LiteCore/Storage
    ${TOP}LiteCore/Support
    ${TOP}LiteCore/Query
    ${TOP}LiteCore/Query/N1QL_Parser
    ${TOP}LiteCore/Query/Translator
    ${TOP}LiteCore/tests
    ${TOP}Replicator
    ${TOP}Replicator/tests
    ${TOP}REST
    ${TOP}REST/tests
    ${TOP}vendor/sockpp/include
    ${TOP}vendor/fleece/vendor/date/include
    ${TOP}vendor/vector_search
)

target_link_libraries(
    CppTests PRIVATE
    LiteCoreUnitTesting
    FleeceObjects
    BLIPObjects
    LiteCoreREST_Static
    LiteCoreWebSocket
)

if(${CMAKE_CXX_COMPILER_ID} STREQUAL "Clang")
    target_compile_options(
        CppTests PRIVATE
        "-Wno-nonportable-system-include-path" # Some deps are not honoring this
        -Wno-extra-semi-stmt
        -Wno-header-hygiene
        -Wno-unused-template
        -Wno-unused-but-set-variable
        -Wno-double-promotion
        -Wno-implicit-int-float-conversion # TODO: Benchmark.hh, 
        -Wno-reorder-ctor
        -Wno-deprecated-literal-operator
    )
endif()