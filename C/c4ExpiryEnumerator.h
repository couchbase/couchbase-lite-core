//
//  c4ExpiryEnumerator.h
//  CBForest
//
//  Created by Jim Borden on 4/13/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#ifndef c4ExpiryEnumerator_h
#define c4ExpiryEnumerator_h

#include "c4Database.h"
#include "c4DocEnumerator.h"

#ifdef __cplusplus
extern "C" {
#endif
    
    /** Opaque handle to an enumerator that iterates through expired documents. */
    typedef struct C4ExpiryEnumerator C4ExpiryEnumerator;
    
    /** Creates an enumerator for iterating over expired documents
        Caller is responsible for freeing the enumerator when finished with it.
        @param database  The database.
        @param outError  Error will be stored here on failure.
        @return  A new enumerator, or NULL on failure. */
    C4ExpiryEnumerator *c4db_enumerateExpired(C4Database *database,
                                              C4Error *outError);

    /** Advances the enumerator to the next document.
        Returns false at the end, or on error; look at the C4Error to determine which occurred,
        and don't forget to free the enumerator. */
    bool c4exp_next(C4ExpiryEnumerator *e, C4Error *outError);

    /** Gets the document ID of the current doc being enumerated
        @param e The enumerator.
        @return A slice representing the doc ID (caller must free)
    */
    C4SliceResult c4exp_getDocID(const C4ExpiryEnumerator *e);
    
    /** Purges the processed entries from the expiration key value store */
    bool c4exp_purgeExpired(C4ExpiryEnumerator *e, C4Error *outError);

    /** Closes the enumeator and disallows further use */
    void c4exp_close(C4ExpiryEnumerator *e);
    
    /** Frees a C4DocEnumerator handle */
    void c4exp_free(C4ExpiryEnumerator *e);

        
#ifdef __cplusplus
    }
#endif

#endif /* c4ExpiryEnumerator_h */
