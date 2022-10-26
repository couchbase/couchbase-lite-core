//
// c4Test.hh
//
// Copyright 2015-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "fleece/Fleece.hh"
#include "c4BlobStore.h"
#include "c4Database.h"
#include "c4Document+Fleece.h"
#include "c4Private.h"
#include "fleece/function_ref.hh"
#include "fleece/Expert.hh"
#include <thread>
#include <vector>
#include <fstream>

// c4CppUtils.hh defines a bunch of useful C++ helpers for rhw LiteCore C API,
// in the `c4` namespace. Check it out!
#include "c4CppUtils.hh"

// More test utilities that don't depend on the C API.
#include "TestsCommon.hh"

#ifdef CATCH_VERSION_MAJOR
#error "This header must be included before Catch.hpp"
#endif


using namespace fleece;


#pragma mark - STREAM OPERATORS FOR LOGGING:


// Logging a C4Error to a stream, or pass it to a Catch logging macro
// like INFO() or WARN().
std::ostream& operator<< (std::ostream &out, C4Error);


// Now include Catch --
// WARNING: This has to go _after_ all the `operator<<` methods, so Catch knows about them.
#include "CatchHelper.hh"


#pragma mark - C4ERROR REPORTING:


/** A utility for logging errors from LiteCore calls. Where you would pass `&error` as a
    parameter, instead pass `ERROR_INFO(error)`. After the call returns, if the error code is
    nonzero, the error message & backtrace will be captured (by Catch's UNSCOPED_INFO())
    and then reported by the subsequent failed CHECK() or REQUIRE().

    You don't even need your own C4Error variable. You can just pass `ERROR_INFO()` with no arg.

    Example:
    ```
        C4Document *doc = c4db_getDocument(db, docID, ERROR_INFO());
        REQUIRE(doc != nullptr);
    ```

    \warning  Don't use ERROR_INFO _inside_ a CHECK() or REQUIRE() call ... due to the way Catch is
              implemented, the error check will happen too late, so the info won't be logged.*/
class ERROR_INFO {
public:
    ERROR_INFO(C4Error *outError)   :_error(outError) {*_error = {};}
    ERROR_INFO(C4Error &outError)   :ERROR_INFO(&outError) { }
    ERROR_INFO()                    :ERROR_INFO(_buf) { }
    ~ERROR_INFO();
    operator C4Error* ()            {return _error;}
private:
    C4Error* _error;
    C4Error _buf;
};


/** `WITH_ERROR` is just like `ERROR_INFO` except it's meant to be used _inside_ a CHECK() or
    REQUIRE() call. It logs the error immediately via WARN() so it'll show up at the same time as
    Catch's failure message.

    Example:
    ```
        REQUIRE( c4db_beginTransaction(db, WITH_ERROR()));
    ```
*/
class WITH_ERROR {
public:
    WITH_ERROR(C4Error *outError)   :_error(outError) {*_error = {};}
    WITH_ERROR(C4Error &outError)   :WITH_ERROR(&outError) { }
    WITH_ERROR()                    :WITH_ERROR(_buf) { }
    ~WITH_ERROR();
    operator C4Error* ()            {return _error;}
private:
    C4Error* _error;
    C4Error _buf;
};


#pragma mark - OTHER TEST UTILITIES:


/// REQUIRE, CHECK and other Catch macros can't be used on background threads because Check is not
/// thread-safe. In multithreaded code, use this instead.
/// \warning Don't use regular assert(), because if this is an optimized build it'll be ignored.
#define	C4Assert(e, ...) \
    (_usuallyFalse(!(e)) ? AssertionFailed(__func__, __FILE__, __LINE__, #e, ##__VA_ARGS__) \
                         : (void)0)
[[noreturn]] void AssertionFailed(const char *func, const char *file, unsigned line,
                                  const char *expr,
                                  const char *message =nullptr);


// Platform-specific filesystem path separator.
#ifdef _MSC_VER
    #define kPathSeparator "\\"
#else
    #define kPathSeparator "/"
#endif


// Temporary directory to use for tests.
#define TEMPDIR(PATH) c4str((TempDir() + PATH).c_str())

const std::string& TempDir();


// CHECKs that `err` matches the expected error domain/code and optional message.
void CheckError(C4Error err,
                C4ErrorDomain expectedDomain, int expectedCode,
                const char *expectedMessage =nullptr);

    
// RAII utility class that wraps `c4db_begin/endTransaction`. Use this instead of the C calls,
// because its destructor will abort the transaction if a REQUIRE fails or an exception is thrown.
// Otherwise, the `c4db_delete` call in the test's teardown will deadlock.
class TransactionHelper {
    public:
    explicit TransactionHelper(C4Database* db) {
        C4Error error;
        C4Assert(c4db_beginTransaction(db, &error));
        _db = db;
    }

    ~TransactionHelper() {
        if (_db) {
            C4Error error;
            C4Assert(c4db_endTransaction(_db, true, &error));
        }
    }

    private:
    C4Database* _db {nullptr};
};


// Calls the function, catches any exception it throws, and CHECKs that it's a C4Error
// (or litecore::error) with the expected domain and code.
// FAILs if no exception is thrown.
void C4ExpectException(C4ErrorDomain domain, int code, std::function<void()>);


#pragma mark - C4TEST BASE CLASS:

#ifndef SkipVersionVectorTest
    #define SkipVersionVectorTest 1
#endif

/// Base test fixture class for C4 tests. Creates a new empty C4Database in its setUp method,
/// and closes & deletes it in tearDown. Also checks for leaks of classes that are InstanceCounted.
class C4Test {
public:
    enum TestOptions {
        RevTreeOption = 0,
        VersionVectorOption,
        EncryptedRevTreeOption
    };
#if defined(COUCHBASE_ENTERPRISE)
    #if SkipVersionVectorTest
    static const int numberOfOptions = 2;       // rev-tree, rev-tree encrypted
    #else
    static const int numberOfOptions = 3;       // rev-tree, version vector, rev-tree encrypted
    #endif
#else
    #if SkipVersionVectorTest
    static const int numberOfOptions = 1;       // rev-tree
    #else
    static const int numberOfOptions = 2;       // rev-tree, version vector
    #endif
#endif

    static std::string sFixturesDir;            // directory where test files live
    static std::string sReplicatorFixturesDir;  // directory where replicator test files live

    static constexpr slice kDatabaseName = "cbl_core_test";
#if SkipVersionVectorTest
    C4Test(int testOption =RevTreeOption);
#else
    C4Test(int testOption =VersionVectorOption);
#endif
    ~C4Test();

    alloc_slice databasePath() const            {return alloc_slice(c4db_getPath(db));}

    /// The database handle.
    C4Database *db;

    const C4DatabaseConfig2& dbConfig() const   {return _dbConfig;}
    const C4StorageEngine storageType() const   {return _storage;}
    bool isSQLite() const                       {return storageType() == kC4SQLiteStorageEngine;}
    bool isRevTrees() const                     {return (_dbConfig.flags & kC4DB_VersionVectors) == 0;}
    bool isEncrypted() const                    {return (_dbConfig.encryptionKey.algorithm != kC4EncryptionNone);}

    static bool isRevTrees(C4Database *database) {
        return (c4db_getConfig2(database)->flags & kC4DB_VersionVectors) == 0;
    }

    C4String revOrVersID(slice revID, slice versID) const {return isRevTrees() ? revID : versID;}
    
    // Creates an extra database, with the same path as db plus the suffix.
    // Caller is responsible for closing & deleting this database when the test finishes.
    C4Database* createDatabase(const std::string &nameSuffix);
    static C4Collection* requireCollection(C4Database* db, C4CollectionSpec spec = kC4DefaultCollectionSpec);

    void closeDB();
    void reopenDB();
    void reopenDBReadOnly();
    void deleteDatabase();
    void deleteAndRecreateDB()                  {deleteAndRecreateDB(db);}

    static void deleteAndRecreateDB(C4Database*&);
    static alloc_slice copyFixtureDB(const std::string &name);
    
    static C4Collection* createCollection(C4Database* db, C4CollectionSpec spec);
    static C4Collection* getCollection(C4Database* db, C4CollectionSpec spec, bool mustExist =true);
    int addDocs(C4Database* database, C4CollectionSpec spec, int total, std::string idprefix = "");
    int addDocs(C4Collection* collection, int total, std::string idprefix ="");

    // Creates a new document revision with the given revID as a child of the current rev
    void createRev(C4Slice docID, C4Slice revID, C4Slice body, C4RevisionFlags flags =0);
    static void createRev(C4Database *db, C4Slice docID, C4Slice revID, C4Slice body, C4RevisionFlags flags =0);
    static void createRev(C4Collection *collection, C4Slice docID, C4Slice revID, C4Slice body, C4RevisionFlags flags =0);
    static std::string createFleeceRev(C4Database *db, C4Slice docID, C4Slice revID, C4Slice jsonBody, C4RevisionFlags flags =0);
    static std::string createFleeceRev(C4Collection *collection, C4Slice docID, C4Slice revID, C4Slice jsonBody, C4RevisionFlags flags =0);
    
    static std::string createNewRev(C4Database *db, C4Slice docID, C4Slice curRevID,
                                    C4Slice body, C4RevisionFlags flags =0);
    static std::string createNewRev(C4Database *db, C4Slice docID,
                                    C4Slice body, C4RevisionFlags flags =0);
    
    static std::string createNewRev(C4Collection *collection, C4Slice docID, C4Slice curRevID,
                                    C4Slice body, C4RevisionFlags flags =0);
    static std::string createNewRev(C4Collection *collection, C4Slice docID,
                                    C4Slice body, C4RevisionFlags flags =0);

    static void createConflictingRev(C4Collection *collection,
                                     C4Slice docID,
                                     C4Slice parentRevID,
                                     C4Slice newRevID,
                                     C4Slice body =kFleeceBody,
                                     C4RevisionFlags flags =0);

    static void createConflictingRev(C4Database *db,
                                     C4Slice docID,
                                     C4Slice parentRevID,
                                     C4Slice newRevID,
                                     C4Slice body =kFleeceBody,
                                     C4RevisionFlags flags =0);

    // Makeshift of c++20 jthread, automatically rejoins on destruction
    struct Jthread {
        std::thread thread;
        Jthread(std::thread&& thread_)
        : thread(move(thread_))
        {}
        Jthread() = default;
        ~Jthread() {
            thread.join();
        }
    };

    void createNumberedDocs(unsigned numberOfDocs);

    std::vector<C4BlobKey> addDocWithAttachments(C4Slice docID,
                                                 std::vector<std::string> attachments,
                                                 const char *contentType,
                                                 std::vector<std::string>* legacyNames =nullptr,
                                                 C4RevisionFlags flags =0);
    std::vector<C4BlobKey> addDocWithAttachments(C4Database* database,
                                                 C4CollectionSpec collectionSpec,
                                                 C4Slice docID,
                                                 std::vector<std::string> attachments,
                                                 const char *contentType,
                                                 std::vector<std::string>* legacyNames =nullptr,
                                                 C4RevisionFlags flags =0);
    void checkAttachment(C4Database *inDB, C4BlobKey blobKey, C4Slice expectedData);
    void checkAttachments(C4Database *inDB, std::vector<C4BlobKey> blobKeys,
                          std::vector<std::string> expectedData);

    static std::string getDocJSON(C4Database* inDB, C4Slice docID);
    static std::string getDocJSON(C4Collection* collection, C4Slice docID);

    std::string listSharedKeys(std::string delimiter =", ");

    static std::filesystem::path findProjectRoot();
    static fleece::alloc_slice readFile(std::filesystem::path path);
    unsigned importJSONFile(std::string path,
                            std::string idPrefix ="",
                            double timeout =0.0,
                            bool verbose =false);
    bool readFileByLines(std::string path, function_ref<bool(FLSlice)>, size_t maxLines);
    unsigned importJSONLines(std::string path, C4Collection*, double timeout =0.0, bool verbose =false,
                             size_t maxLines =0, const std::string& idPrefix ="");
    unsigned importJSONLines(std::string path, double timeout =0.0, bool verbose =false,
                             C4Database* database = nullptr, size_t maxLines =0,
                             const std::string& idPrefix ="");


    bool docBodyEquals(C4Document *doc NONNULL, slice fleece);

    static std::string fleece2json(slice fleece) {
        auto value = ValueFromData(fleece);
        REQUIRE(value);
        return value.toJSON(true, true).asString();
    }


    alloc_slice json2fleece(const char *json5str) {
        std::string jsonStr = json5(json5str);
        TransactionHelper t(db);
        alloc_slice encodedBody = c4db_encodeJSON(db, slice(jsonStr), nullptr);
        REQUIRE(encodedBody);
        return encodedBody;
    }

    Doc json2dict(const char *json) {
        return Doc(json2fleece(json), kFLTrusted, c4db_getFLSharedKeys(db));
    }


    // Some handy constants to use
    static const C4Slice kDocID;    // "mydoc"

                            // REV-TREES:       VERSION VECTORS:
    C4Slice kRevID;         // "1-abcd"         "1@*"
    C4Slice kRev1ID;        // "1-abcd"         "1@*"
    C4Slice kRev1ID_Alt;    // "1-dcba"         "1@*"
    C4Slice kRev2ID;        // "2-c001d00d"     "2@*"
    C4Slice kRev3ID;        // "3-deadbeef"     "3@*"
    C4Slice kRev4ID;        // "4-44444444"     "4@*"

    static C4Slice kFleeceBody;             // {"ans*wer":42}, in Fleece
    static C4Slice kEmptyFleeceBody;        // {}, in Fleece

private:
    const C4StorageEngine _storage;
    C4DatabaseConfig2 _dbConfig;
    int objectCount;
};
