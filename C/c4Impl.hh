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

typedef forestdb::slice C4Slice;

typedef struct {
    const void *buf;
    size_t size;
} C4SliceResult;

#define kC4SliceNull forestdb::slice::null


#define C4_IMPL
#include "c4Database.h"


#include "Error.hh"
#include "Database.hh"

using namespace forestdb;

Database::config c4DbConfig(C4DatabaseFlags flags);

Database* asDatabase(C4Database*);


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


#endif /* c4Impl_h */
