//
//  c4ExpiryEnumerator.h
//  Couchbase Lite Core
//
//  Created by Jim Borden on 4/13/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#pragma once

#include "c4Database.h"
#include "c4DocEnumerator.h"

#ifdef __cplusplus
extern "C" {
#endif
    
    /** \defgroup Expiration Document Expiration
        @{ */


    /** Opaque handle to an enumerator that iterates through expired documents. */
    typedef struct C4ExpiryEnumerator C4ExpiryEnumerator;
    
    /** Creates an enumerator for iterating over expired documents
        Caller is responsible for freeing the enumerator when finished with it.
        @param database  The database.
        @param outError  Error will be stored here on failure.
        @return  A new enumerator, or NULL on failure. */
    C4ExpiryEnumerator *c4db_enumerateExpired(C4Database *database,
                                              C4Error *outError) C4API;

    /** Advances the enumerator to the next document.
        Returns false at the end, or on error; look at the C4Error to determine which occurred,
        and don't forget to free the enumerator. */
    bool c4exp_next(C4ExpiryEnumerator *e, C4Error *outError) C4API;

    /** Gets the document ID of the current doc being enumerated
        @param e The enumerator.
        @return A slice representing the doc ID (caller must free)
    */
    C4SliceResult c4exp_getDocID(const C4ExpiryEnumerator *e) C4API;
    
    /** Purges the processed entries from the expiration key value store */
    bool c4exp_purgeExpired(C4ExpiryEnumerator *e, C4Error *outError) C4API;

    /** Closes the enumerator and disallows further use */
    void c4exp_close(C4ExpiryEnumerator *e) C4API;
    
    /** Frees a C4ExpiryEnumerator handle */
    void c4exp_free(C4ExpiryEnumerator *e) C4API;


    /** @} */
#ifdef __cplusplus
    }
#endif
