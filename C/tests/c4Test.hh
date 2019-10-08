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

using namespace fleece;

#include "c4Database.h"
#include "c4Document+Fleece.h"
#include "c4Private.h"

#include "CatchHelper.hh"
#include "PlatformCompat.hh"
#include <function_ref.hh>
#include <functional>


#if 0 // disabled because CMake is building test binaries with optimization
#ifdef NDEBUG
    // Catch's assertion macros are pretty slow, and affect benchmark times.
    // So replace them with quick-n-dirty alternatives in an optimized build.
    #undef REQUIRE
    #define REQUIRE(X) do {if (!(X)) abort();} while (0)
    #undef CHECK
    #define CHECK(X) do {if (!(X)) abort();} while (0)
    #undef INFO
    #define INFO(X)
#endif
#endif


// REQUIRE, CHECK and other Catch macros can't be used on background threads because Check is not
// thread-safe. Use this instead. Don't use regular assert() because if this is an optimized build
// it'll be ignored.
#undef Assert
#define	Assert(e, ...) \
    (_usuallyFalse(!(e)) ? AssertionFailed(__func__, __FILE__, __LINE__, #e, ##__VA_ARGS__) \
                         : (void)0)

[[noreturn]] void AssertionFailed(const char *func, const char *file, unsigned line,
                                  const char *expr,
                                  const char *message =nullptr);


#ifdef _MSC_VER
    #define kPathSeparator "\\"
#else
    #define kPathSeparator "/"
#endif


#define TEMPDIR(PATH) c4str((TempDir() + PATH).c_str())

const std::string& TempDir();


// These '<<' functions help Catch log values of custom types in assertion messages:
std::ostream& operator<< (std::ostream& o, fleece::slice s);
std::ostream& operator<< (std::ostream& o, fleece::alloc_slice s);

std::ostream& operator<< (std::ostream& o, C4Slice s);
std::ostream& operator<< (std::ostream& o, C4SliceResult s);

std::ostream& operator<< (std::ostream &out, C4Error error);

template <class T>
std::ostream& operator<< (std::ostream &o, const std::set<T> &things) {
    o << "{";
    int n = 0;
    for (const T &thing : things) {
        if (n++) o << ", ";
        o << '"' << thing << '"';
    }
    o << "}";
    return o;
}


// Converts a slice to a C++ string
static inline std::string toString(C4Slice s)   {return std::string((char*)s.buf, s.size);}


// Converts JSON5 to JSON; helps make JSON test input more readable!
std::string json5(std::string);
fleece::alloc_slice json5slice(std::string str);


void CheckError(C4Error err,
                C4ErrorDomain expectedDomain, int expectedCode,
                const char *expectedMessage =nullptr);

    
// Waits for the predicate to return true, checking every 100ms.
// If the timeout elapses, calls FAIL.
void WaitUntil(int timeoutMillis, function_ref<bool()> predicate);


// This helper is necessary because it ends an open transaction if an assertion fails.
// If the transaction isn't ended, the c4db_delete call in tearDown will deadlock.
class TransactionHelper {
    public:
    explicit TransactionHelper(C4Database* db) {
        C4Error error;
        Assert(c4db_beginTransaction(db, &error));
        _db = db;
    }

    ~TransactionHelper() {
        if (_db) {
            C4Error error;
            Assert(c4db_endTransaction(_db, true, &error));
        }
    }

    private:
    C4Database* _db {nullptr};
};


struct ExpectingExceptions {
    ExpectingExceptions()    {++gC4ExpectExceptions; c4log_warnOnErrors(false);}
    ~ExpectingExceptions()   {--gC4ExpectExceptions; c4log_warnOnErrors(true);}
};


// Handy base class that creates a new empty C4Database in its setUp method,
// and closes & deletes it in tearDown.
class C4Test {
public:
#if ENABLE_VERSION_VECTORS
    static const int numberOfOptions = 3;       // rev-tree, rev-tree encrypted, version vector
#elif defined(COUCHBASE_ENTERPRISE)
    static const int numberOfOptions = 2;       // rev-tree, rev-tree encrypted
#else
    static const int numberOfOptions = 1;       // rev-tree
#endif

    static std::string sFixturesDir;            // directory where test files live
    static std::string sReplicatorFixturesDir;  // directory where replicator test files live
    static const std::string kDatabaseName;

    C4Test(int testOption =1);
    ~C4Test();

    C4Slice databasePath() const                {return c4str(_dbPath.c_str());}
    const std::string& databasePathString() const  {return _dbPath;}

    C4Database *db;

    const C4StorageEngine storageType() const   {return _storage;}
    bool isSQLite() const                       {return storageType() == kC4SQLiteStorageEngine;}
    C4DocumentVersioning versioning() const     {return _versioning;}
    bool isRevTrees() const                     {return _versioning == kC4RevisionTrees;}
    bool isEncrypted() const                    {return (c4db_getConfig(db)->encryptionKey.algorithm != kC4EncryptionNone);}

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
                            double timeout =15.0,
                            bool verbose =false);
    bool readFileByLines(std::string path, std::function<bool(FLSlice)>);
    unsigned importJSONLines(std::string path, double timeout =15.0, bool verbose =false,
                             C4Database* database = nullptr);

    
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
    C4Slice kRevID;    // "1-abcd"
    C4Slice kRev2ID;   // "2-c001d00d"
    C4Slice kRev3ID;   // "3-deadbeef"
    static C4Slice kFleeceBody;
    static C4Slice kEmptyFleeceBody;

private:
    const C4StorageEngine _storage;
    const C4DocumentVersioning _versioning;
    std::string _dbPath;
    int objectCount;
};
