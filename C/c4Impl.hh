//
//  c4Impl.h
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

using namespace forestdb;


// Predefine C4Slice as a typedef of slice so we can use the richer slice API:

typedef slice C4Slice;

typedef struct {
    const void *buf;
    size_t size;
} C4SliceResult;

#define C4_IMPL // This tells c4.h to skip its declaration of C4Slice


// INTERNAL APIs:


#include "c4Database.h"

namespace c4Internal {

    void recordError(C4ErrorDomain domain, int code, C4Error* outError);
    void recordHTTPError(int httpStatus, C4Error* outError);
    void recordError(error e, C4Error* outError);
    void recordUnknownException(C4Error* outError);

    #define catchError(OUTERR) \
        catch (error err) { \
            recordError(err, OUTERR); \
        } catch (...) { \
            recordUnknownException(OUTERR); \
        }

    Database::config c4DbConfig(C4DatabaseFlags flags, const C4EncryptionKey *key);

    Database* asDatabase(C4Database*);
    bool mustBeInTransaction(C4Database*, C4Error *outError);
    Document dbGetDoc(C4Database*, sequence);
    Transaction* dbGetTransaction(C4Database*);

    bool rekey(Database* database, const C4EncryptionKey *newKey, C4Error *outError);

    C4Document* newC4Document(C4Database*, const Document&);

}

using namespace c4Internal;


struct c4Key : public Collatable {
    c4Key()                 :Collatable() { }
    c4Key(C4Slice bytes)    :Collatable(bytes, true) { }
};


#endif /* c4Impl_h */
