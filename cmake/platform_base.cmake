function(set_litecore_source_base)
    set(oneValueArgs RESULT)
    cmake_parse_arguments(BASE_SSS ""  ${oneValueArgs} "" ${ARGN})
    if(NOT DEFINED BASE_SSS_RESULT)
        message(FATAL_ERROR set_source_files_base needs to be called with RESULT)
    endif()

    set(LITECORE_SHARED_LINKER_FLAGS "" CACHE INTERNAL "")

    # Error.cc will be added here *and* in Support because the Support version is a stub
    # That goes through the C API of LiteCore.  If the stub were compiled into LiteCore
    # itself, that's an infinite recursive call
    set(
        ${BASE_SSS_RESULT}
        C/c4Base.cc
        C/c4BlobStore.cc
        C/c4CAPI.cc
        C/c4Certificate.cc
        C/c4Collection.cc
        C/c4Database.cc
        C/c4DocEnumerator.cc
        C/c4Document.cc
        C/c4Error.cc
        C/c4Index.cc
        C/c4Observer.cc
        C/c4PredictiveQuery.cc
        C/c4Query.cc
        Crypto/SecureRandomize.cc
        Crypto/mbedUtils.cc
        Crypto/mbedSnippets.cc
        Crypto/Certificate.cc
        Crypto/PublicKey.cc
        Crypto/SecureDigest.cc
        Crypto/SecureSymmetricCrypto.cc
        LiteCore/BlobStore/BlobStreams.cc
        LiteCore/BlobStore/Stream.cc
        LiteCore/Database/BackgroundDB.cc
        LiteCore/Database/DatabaseImpl.cc
        LiteCore/Database/Housekeeper.cc
        LiteCore/Database/LegacyAttachments.cc
        LiteCore/Database/LiveQuerier.cc
        LiteCore/Database/PrebuiltCopier.cc
        LiteCore/Database/SequenceTracker.cc
        LiteCore/Database/TreeDocument.cc
        LiteCore/Database/Upgrader.cc
        LiteCore/Database/VectorDocument.cc
        LiteCore/Query/DateFormat.cc
        LiteCore/Query/IndexSpec.cc
        LiteCore/Query/LazyIndex.cc
        LiteCore/Query/PredictiveModel.cc
        LiteCore/Query/Query.cc
        LiteCore/Query/Translator/QueryTranslator.cc
        LiteCore/Query/Translator/ExprNodes.cc
        LiteCore/Query/Translator/IndexedNodes.cc
        LiteCore/Query/Translator/Node.cc
        LiteCore/Query/Translator/SelectNodes.cc
        LiteCore/Query/Translator/NodesToSQL.cc
        LiteCore/Query/Translator/TranslatorUtils.cc
        LiteCore/Query/SQLiteDataFile+Indexes.cc
        LiteCore/Query/SQLiteFleeceEach.cc
        LiteCore/Query/SQLiteFleeceFunctions.cc
        LiteCore/Query/SQLiteFleeceUtil.cc
        LiteCore/Query/SQLiteFTSRankFunction.cc
        LiteCore/Query/SQLiteKeyStore+ArrayIndexes.cc
        LiteCore/Query/SQLiteKeyStore+FTSIndexes.cc
        LiteCore/Query/SQLiteKeyStore+Indexes.cc
        LiteCore/Query/SQLiteKeyStore+PredictiveIndexes.cc
        LiteCore/Query/SQLiteKeyStore+VectorIndex.cc
        LiteCore/Query/SQLiteN1QLFunctions.cc
        LiteCore/Query/SQLitePredictionFunction.cc
        LiteCore/Query/SQLiteQuery.cc
        LiteCore/Query/SQLUtil.cc
        LiteCore/Query/N1QL_Parser/n1ql.cc
        LiteCore/RevTrees/RawRevTree.cc
        LiteCore/RevTrees/RevID.cc
        LiteCore/RevTrees/RevTree.cc
        LiteCore/RevTrees/RevTreeRecord.cc
        LiteCore/RevTrees/VectorRecord.cc
        LiteCore/RevTrees/Version.cc
        LiteCore/RevTrees/VersionVector.cc
        LiteCore/Storage/BothKeyStore.cc
        LiteCore/Storage/DataFile.cc
        LiteCore/Storage/KeyStore.cc
        LiteCore/Storage/Record.cc
        LiteCore/Storage/RecordEnumerator.cc
        LiteCore/Storage/SQLiteDataFile.cc
        LiteCore/Storage/SQLiteEnumerator.cc
        LiteCore/Storage/SQLiteKeyStore.cc
        LiteCore/Storage/UnicodeCollator.cc
        Networking/Address.cc
        Networking/HTTP/CookieStore.cc
        vendor/SQLiteCpp/src/Backup.cpp
        vendor/SQLiteCpp/src/Column.cpp
        vendor/SQLiteCpp/src/Database.cpp
        vendor/SQLiteCpp/src/Exception.cpp
        vendor/SQLiteCpp/src/Statement.cpp
        vendor/SQLiteCpp/src/Transaction.cpp
        vendor/SQLiteCpp/sqlite3/ext/carray.cc
        vendor/SQLiteCpp/sqlite3/ext/carray_bind.cc
        vendor/vector_search/VectorIndexSpec.cc
        Replicator/c4Replicator.cc
        Replicator/c4Replicator_CAPI.cc
        Replicator/c4Socket.cc
        Replicator/ChangesFeed.cc
        Replicator/Checkpoint.cc
        Replicator/Checkpointer.cc
        Replicator/DatabaseCookies.cc
        Replicator/DBAccess.cc
        Replicator/IncomingRev.cc
        Replicator/IncomingRev+Blobs.cc
        Replicator/Inserter.cc
        Replicator/PropertyEncryption_stub.cc
        Replicator/Puller.cc
        Replicator/Pusher.cc
        Replicator/Pusher+Attachments.cc
        Replicator/Pusher+Revs.cc
        Replicator/Replicator.cc
        Replicator/ReplicatorTypes.cc
        Replicator/RevFinder.cc
        Replicator/URLTransformer.cc
        Replicator/Worker.cc
        LiteCore/Support/Arena.cc
        LiteCore/Support/Logging.cc
        LiteCore/Support/DefaultLogger.cc
        LiteCore/Support/Error.cc
        LiteCore/Support/EncryptedStream.cc
        LiteCore/Support/FilePath.cc
        LiteCore/Support/HybridClock.cc
        LiteCore/Support/LogDecoder.cc
        LiteCore/Support/LogEncoder.cc
        LiteCore/Support/PlatformIO.cc
        LiteCore/Support/SequenceSet.cc
        LiteCore/Support/StringUtil.cc
        LiteCore/Support/ThreadUtil.cc
        LiteCore/Support/ChannelManifest.cc
        LiteCore/Support/Extension.cc
        PARENT_SCOPE
    )
endfunction()

function(setup_litecore_build_base)  
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        add_compile_options("-fuse-ld=lld")
        if(LITECORE_WARNINGS_HARDCORE)
            set(LITECORE_CXX_WARNINGS
                -Werror
                -Weverything            # "WARN ALL THE THINGS!!!"
                -Wformat=2
                # Disabled C++ warnings:
                -Wno-cast-qual  # TODO: "cast drops const qualifier"
                -Wno-nullable-to-nonnull-conversion # TODO: "implicit conversion from nullable pointer to non-nullable pointer type"
                -Wno-sign-compare # TODO "comparison of integers of different signs"
                -Wno-sign-conversion # TODO "implicit conversion changes signedness"
                -Wno-switch-enum # TODO: "enumeration values not explicitly handled in switch"
                -Wno-alloca
                -Wno-atomic-implicit-seq-cst # "implicit use of sequentially-consistent atomic may incur stronger memory barriers than necessary"
                -Wno-c++98-compat
                -Wno-c++98-compat-pedantic
                -Wno-cast-align # "cast from X* to Y* increases required alignment"
                -Wno-cast-function-type # "cast from X* to Y* converts to incompatible function type"
                -Wno-covered-switch-default # "default label in switch which covers all enumeration values"
                -Wno-c99-extensions
                -Wno-date-time # "expansion of date or time macro is not reproducible"
                -Wno-deprecated-copy-with-user-provided-dtor # "definition of implicit copy constructor is deprecated because it has a user-provided destructor"
                -Wno-exit-time-destructors # "declaration requires an exit-time destructor"
                -Wno-extra-semi # "extra ';' after member function definition"
                -Wno-float-equal
                -Wno-format-pedantic # "format specifies type 'void *' but the argument has type 'C4Document *'"
                -Wno-global-constructors
                -Wno-gnu-anonymous-struct # "anonymous structs are a GNU extension"
                -Wno-gnu-zero-variadic-macro-arguments # "token pasting of ',' and __VA_ARGS__ is a GNU extension"
                -Wno-inconsistent-missing-destructor-override # "'~Foo' overrides a destructor but is not marked 'override'"
                -Wno-missing-field-initializers # "missing field 'x' initializer"
                -Wno-missing-noreturn # "function could be declared with attribute 'noreturn'"
                -Wno-nested-anon-types # "anonymous types declared in an anonymous union are an extension"
                -Wno-nullability-extension
                -Wno-old-style-cast
                -Wno-padded
                -Wno-shadow-field # "parameter shadows member inherited from type"
                -Wno-shadow-uncaptured-local # "declaration [of a lambda parameter] shadows a local variable"
                -Wno-suggest-destructor-override # "'~Foo' overrides a destructor but is not marked 'override'"
                -Wno-undef      # `#if X` where X isn't defined
                -Wno-unused-macros
                -Wno-unused-exception-parameter
                -Wno-unused-parameter # Unused fn parameter
                -Wno-weak-vtables # "Class has no out-of-line virtual method definitions; its vtable will be emitted in every translation unit"
                -Wno-zero-as-null-pointer-constant # Using 0 instead of nullptr. Some of our dependencies do this in headers.
                -Wno-documentation-deprecated-sync # "declaration is marked with '\deprecated' command but does not have a deprecation attribute" --mbedTLS does this
                -Wno-unsafe-buffer-usage
                -Wno-reserved-macro-identifier
                -Wno-reserved-identifier
                -Wno-ctad-maybe-unsupported
                -Wno-language-extension-token # Many implementation assume _MSC_VER == MSVC, but it can also mean clang-cl which warns about Microsoft specific things
            PARENT_SCOPE)

            set_source_files_properties(LiteCore/Query/N1QL_Parser/n1ql.cc PROPERTIES COMPILE_FLAGS -Wno-extra-semi-stmt)
        endif()
    endif()
endfunction()
