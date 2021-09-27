//
// c4DocEnumerator.h
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

#include "c4DocEnumeratorTypes.h"

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS

    /** \defgroup DocEnumerator  Document Enumeration
        @{ */


    /** Closes an enumeration. This is optional, but can be used to free up resources if the
        enumeration has not reached its end, but will not be freed for a while. */
    void c4enum_close(C4DocEnumerator* C4NULLABLE e) C4API;

    /** Frees a C4DocEnumerator handle. */
    void c4enum_free(C4DocEnumerator* C4NULLABLE e) C4API;

#ifndef C4_STRICT_COLLECTION_API
    /** Creates an enumerator ordered by sequence.
        Caller is responsible for freeing the enumerator when finished with it.
        @param database  The database.
        @param since  The sequence number to start _after_. Pass 0 to start from the beginning.
        @param options  Enumeration options (NULL for defaults).
        @param outError  Error will be stored here on failure.
        @return  A new enumerator, or NULL on failure. */
    C4DocEnumerator* c4db_enumerateChanges(C4Database *database,
                                           C4SequenceNumber since,
                                           const C4EnumeratorOptions* C4NULLABLE options,
                                           C4Error* C4NULLABLE outError) C4API;

    /** Creates an enumerator ordered by docID.
        Options have the same meanings as in Couchbase Lite.
        There's no 'limit' option; just stop enumerating when you're done.
        Caller is responsible for freeing the enumerator when finished with it.
        @param database  The database.
        @param options  Enumeration options (NULL for defaults).
        @param outError  Error will be stored here on failure.
        @return  A new enumerator, or NULL on failure. */
    C4DocEnumerator* c4db_enumerateAllDocs(C4Database *database,
                                           const C4EnumeratorOptions* C4NULLABLE options,
                                           C4Error* C4NULLABLE outError) C4API;
#endif

    /** Creates an enumerator ordered by sequence.
        Caller is responsible for freeing the enumerator when finished with it.
        @param collection  The collection.
        @param since  The sequence number to start _after_. Pass 0 to start from the beginning.
        @param options  Enumeration options (NULL for defaults).
        @param outError  Error will be stored here on failure.
        @return  A new enumerator, or NULL on failure. */
    C4DocEnumerator* c4coll_enumerateChanges(C4Collection *collection,
                                             C4SequenceNumber since,
                                             const C4EnumeratorOptions* C4NULLABLE options,
                                             C4Error* C4NULLABLE outError) C4API;

    /** Creates an enumerator ordered by docID.
        Options have the same meanings as in Couchbase Lite.
        There's no 'limit' option; just stop enumerating when you're done.
        Caller is responsible for freeing the enumerator when finished with it.
        @param collection  The collection.
        @param options  Enumeration options (NULL for defaults).
        @param outError  Error will be stored here on failure.
        @return  A new enumerator, or NULL on failure. */
    C4DocEnumerator* c4coll_enumerateAllDocs(C4Collection *collection,
                                             const C4EnumeratorOptions* C4NULLABLE options,
                                             C4Error* C4NULLABLE outError) C4API;

    /** Advances the enumerator to the next document.
        Returns false at the end, or on error; look at the C4Error to determine which occurred,
        and don't forget to free the enumerator. */
    bool c4enum_next(C4DocEnumerator *e, C4Error* C4NULLABLE outError) C4API;

    /** Returns the current document, if any, from an enumerator.
        @param e  The enumerator.
        @param outError  Error will be stored here on failure.
        @return  The document, or NULL if there is none or if an error occurred reading its body.
                 Caller is responsible for calling c4document_free when done with it. */
    C4Document* c4enum_getDocument(C4DocEnumerator *e,
                                   C4Error* C4NULLABLE outError) C4API;

    /** Stores the metadata of the enumerator's current document into the supplied
        C4DocumentInfo struct. Unlike c4enum_getDocument(), this allocates no memory.
        @param e  The enumerator.
        @param outInfo  A pointer to a C4DocumentInfo struct that will be filled in if a document
                        is found.
        @return  True if the info was stored, false if there is no current document. */
    bool c4enum_getDocumentInfo(C4DocEnumerator *e,
                                C4DocumentInfo *outInfo) C4API;

    /** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
