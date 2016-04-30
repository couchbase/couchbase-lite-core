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
    
    /** Stores the metadata of the enumerator's current document into the supplied
		C4DocumentInfo struct. Unlike c4enum_getDocument(), this allocates no memory.
		@param e  The enumerator.
		@param outInfo  A pointer to a C4DocumentInfo struct that will be filled in if a document
		is found.  Only the document ID is valid and other values will be zeroed. */
    void c4exp_getInfo(C4ExpiryEnumerator *e, C4DocumentInfo *docInfo);
    
    /** Purges the processed entries from the expiration key value store */
    bool c4exp_purgeExpired(C4ExpiryEnumerator *e, C4Error *outError);
    
    /** Frees a C4DocEnumerator handle */
    void c4exp_free(C4ExpiryEnumerator *e);

        
#ifdef __cplusplus
    }
#endif

#endif /* c4ExpiryEnumerator_h */
