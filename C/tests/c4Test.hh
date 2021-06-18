//
// c4Test.hh
//
// Copyright (c) 2015 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#pragma once
#include "fleece/Fleece.hh"
#include "c4BlobStore.h"
#include "c4Database.h"
#include "c4Document+Fleece.h"
#include "c4Private.h"
#include "function_ref.hh"
#include <vector>

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


#pragma mark - C4TEST BASE CLASS:

#define SkipVersionVectorTest 1

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

    void closeDB();
    void reopenDB();
    void reopenDBReadOnly();
    void deleteDatabase();
    void deleteAndRecreateDB()                  {deleteAndRecreateDB(db);}

    static void deleteAndRecreateDB(C4Database*&);
    static alloc_slice copyFixtureDB(const std::string &name);

    // Creates a new document revision with the given revID as a child of the current rev
    void createRev(C4Slice docID, C4Slice revID, C4Slice body, C4RevisionFlags flags =0);
    static void createRev(C4Database *db, C4Slice docID, C4Slice revID, C4Slice body, C4RevisionFlags flags =0);
    static std::string createFleeceRev(C4Database *db, C4Slice docID, C4Slice revID, C4Slice jsonBody, C4RevisionFlags flags =0);
    static std::string createNewRev(C4Database *db, C4Slice docID, C4Slice curRevID,
                                    C4Slice body, C4RevisionFlags flags =0);
    static std::string createNewRev(C4Database *db, C4Slice docID,
                                    C4Slice body, C4RevisionFlags flags =0);

    static void createConflictingRev(C4Database *db,
                                     C4Slice docID,
                                     C4Slice parentRevID,
                                     C4Slice newRevID,
                                     C4Slice body =kFleeceBody,
                                     C4RevisionFlags flags =0);

    void createNumberedDocs(unsigned numberOfDocs);

    std::vector<C4BlobKey> addDocWithAttachments(C4Slice docID,
                                                 std::vector<std::string> attachments,
                                                 const char *contentType,
                                                 std::vector<std::string>* legacyNames =nullptr,
                                                 C4RevisionFlags flags =0);
    void checkAttachment(C4Database *inDB, C4BlobKey blobKey, C4Slice expectedData);
    void checkAttachments(C4Database *inDB, std::vector<C4BlobKey> blobKeys,
                          std::vector<std::string> expectedData);

    static std::string getDocJSON(C4Database* inDB, C4Slice docID);

    std::string listSharedKeys(std::string delimiter =", ");

    static fleece::alloc_slice readFile(std::string path);
    unsigned importJSONFile(std::string path,
                            std::string idPrefix ="",
                            double timeout =0.0,
                            bool verbose =false);
    bool readFileByLines(std::string path, function_ref<bool(FLSlice)>);
    unsigned importJSONLines(std::string path, double timeout =0.0, bool verbose =false,
                             C4Database* database = nullptr);


    bool docBodyEquals(C4Document *doc NONNULL, slice fleece);

    static std::string fleece2json(slice fleece) {
        auto value = Value::fromData(fleece);
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
