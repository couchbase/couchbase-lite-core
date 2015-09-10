//
//  c4View.h
//  CBForest
//
//  C API for view and query access.
//
//  Created by Jens Alfke on 9/10/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#ifndef c4View_h
#define c4View_h

#include "c4.h"

#ifdef __cplusplus
extern "C" {
#endif

    //////// VIEWS:
    

    /** Opaque handle to an opened view. */
    typedef struct c4View C4View;

    typedef void (*C4EmitFn)(void *emitContext,
                             C4Slice keyJSON,
                             C4Slice valueJSON);

    typedef bool (*C4MapFn)(void *mapContext,
                            C4Slice docID,
                            C4Slice revID,
                            C4Slice json,
                            C4EmitFn *emit,
                            void *emitContext,
                            C4Error *outError);

    /** Opens a view. */
    C4View* c4view_open(C4Slice path,
                        C4MapFn *mapFn,
                        //C4ReduceFn *reduceFn, //TODO
                        C4Slice version,
                        C4Error *outError);

    /** Closes the view and frees the object. */
    void c4view_close(C4View* view);
    
    bool c4view_eraseIndex(C4View *view, C4Error *outError);

    bool c4view_delete(C4View *view, C4Error *outError);

    bool c4view_updateIndexes(C4View *views[], int viewCount, C4Error *outError);


    uint64_t c4view_getTotalRows(C4View *view);

    C4SequenceNumber c4view_getLastSequenceIndexed(C4View *view);

    C4SequenceNumber c4view_getLastSequenceChangedAt(C4View *view);


    //////// QUERYING:


    typedef struct {
        unsigned prefixMatchLevel;
        unsigned skip;
        unsigned limit;
        unsigned groupLevel;
        C4Slice startKeyJSON;
        C4Slice endKeyJSON;
        C4Slice startKeyDocID;
        C4Slice endKeyDocID;
        const C4Slice* keys;
        unsigned keysCount;
        bool descending;
        bool includeDocs;
        bool updateSeq;
        bool localSeq;
        bool inclusiveStart;
        bool inclusiveEnd;
        bool reduceSpecified;
        bool reduce;                   // Ignored if !reduceSpecified
        bool group;
    } C4QueryOptions;

    typedef struct c4QueryEnumerator C4QueryEnumerator;

    typedef struct {
        C4Slice keyJSON;
        C4Slice valueJSON;
        C4Slice docID;
    } C4QueryRow;

    C4QueryEnumerator* c4view_query(C4View *view,
                                    const C4QueryOptions *options,
                                    C4Error *outError);

    C4QueryRow* c4queryenum_next(C4QueryEnumerator *e);

    void c4queryenum_free(C4QueryEnumerator *e);

    void c4queryrow_free(C4QueryRow *row);

#ifdef __cplusplus
}
#endif

#endif /* c4View_h */
