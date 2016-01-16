//
//  c4Impl.hh
//  CBForest
//
//  Created by Jens Alfke on 9/15/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#ifndef c4Impl_h
#define c4Impl_h

#include "slice.hh"
#include "Database.hh"
#include "Collatable.hh"
#include "Error.hh"
#include <functional>

// Defining C4DB_THREADSAFE as 1 will make C4Database thread-safe: the same handle can be called
// simultaneously from multiple threads. Transactions will be single-threaded: once a thread has
// called c4db_beginTransaction, other threads making that call will block until the transaction
// ends.
#if C4DB_THREADSAFE
#include <mutex>
#endif

using namespace cbforest;

namespace cbforest {
    class VersionedDocument;
}


// Predefine C4Slice as a typedef of slice so we can use the richer slice API:

typedef slice C4Slice;

typedef struct {
    const void *buf;
    size_t size;
} C4SliceResult;

#define C4_IMPL // This tells c4.h to skip its declaration of C4Slice

#define kC4SliceNull slice::null


// INTERNAL APIs:


#include "c4Database.h"

struct C4Document;
struct C4DocEnumerator;

namespace c4Internal {

    void recordError(C4ErrorDomain domain, int code, C4Error* outError);
    void recordHTTPError(int httpStatus, C4Error* outError);
    void recordError(error e, C4Error* outError);
    void recordUnknownException(C4Error* outError);
    static inline void clearError(C4Error* outError) {if (outError) outError->code = 0;}

    #define catchError(OUTERR) \
        catch (error err) { \
            recordError(err, OUTERR); \
        } catch (...) { \
            recordUnknownException(OUTERR); \
        }

    Database::config c4DbConfig(C4DatabaseFlags flags, const C4EncryptionKey *key);

    bool rekey(Database* database, const C4EncryptionKey *newKey, C4Error *outError);

    C4Document* newC4Document(C4Database*, const Document&);

    const VersionedDocument& versionedDocument(C4Document*);

    void setEnumFilter(C4DocEnumerator*,
                       std::function<bool(slice docID, sequence sequence, slice docType)> filter);
}

using namespace c4Internal;


struct c4Database : public Database {
    c4Database(std::string path, const config& cfg);
    Transaction* transaction() {
        CBFAssert(_transaction);
        return _transaction;
    }
    // Transaction methods below acquire _transactionMutex. Do not call them if
    // _mutex is already locked, or deadlock may occur!
    void beginTransaction();
    bool inTransaction();
    bool mustBeInTransaction(C4Error *outError);
    bool mustNotBeInTransaction(C4Error *outError);
    bool endTransaction(bool commit);

#if C4DB_THREADSAFE
    // Mutex for synchronizing Database calls. Non-recursive!
    std::mutex _mutex;
#endif

private:
#if C4DB_THREADSAFE
    // Recursive mutex for accessing _transaction and _transactionLevel.
    // Must be acquired BEFORE _mutex, or deadlock may occur!
    std::recursive_mutex _transactionMutex;
#endif
    Transaction* _transaction;
    int _transactionLevel;
};


#if C4DB_THREADSAFE
#define WITH_LOCK(db) std::lock_guard<std::mutex> _lock((db)->_mutex)
#else
#define WITH_LOCK(db) do { } while (0)  // no-op
#endif


struct c4Key : public CollatableBuilder {
    c4Key()                 :CollatableBuilder() { }
    c4Key(C4Slice bytes)    :CollatableBuilder(bytes, true) { }
};


struct c4KeyValueList {
    std::vector<Collatable> keys;
    std::vector<slice> values;
};

#endif /* c4Impl_h */
