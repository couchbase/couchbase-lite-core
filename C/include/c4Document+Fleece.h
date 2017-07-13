//
//  c4Document+Fleece.h
//  LiteCore
//
//  Created by Jens Alfke on 10/25/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once

#include "c4Document.h"
#include "c4BlobStore.h"
#include "Fleece.h"


#ifdef __cplusplus
extern "C" {
#endif


    /** \defgroup Documents Documents
        @{ */


    /** \name Fleece-related
        @{ */

    /** The sub-document property that identifies it as a special type of object.
        For example, a blob is represented as `{"@type":"blob", "digest":"xxxx", ...}` */
    #define kC4ObjectTypeProperty "@type"

    /** Value of kC4ObjectTypeProperty that denotes a blob. */
    #define kC4ObjectType_Blob "blob"

    /** Top-level document property whose value is a CBL 1.x / CouchDB attachments container. */
    #define kC4LegacyAttachmentsProperty "_attachments"


    /** Returns true if this is the name of a 1.x metadata property ("_id", "_rev", "_deleted".)
        Does NOT return true for "_attachments" because that property isn't obsolete. */
    bool c4doc_isOldMetaProperty(C4String prop) C4API;

    /** Returns true if the document contains 1.x metadata properties at top level.
        Does NOT return true for "_attachments" because that property isn't obsolete. */
    bool c4doc_hasOldMetaProperties(FLDict doc C4NONNULL) C4API;

    /** Re-encodes to Fleece, without any 1.x metadata properties. */
    C4SliceResult c4doc_encodeStrippingOldMetaProperties(FLDict doc C4NONNULL) C4API;

    /** Returns true if the given dictionary is a [reference to a] blob; if so, gets its key.
        (This function cannot recognize child dictionaries of "_attachments", because it's not
        possible to look at the parent of a Fleece value.) */
    bool c4doc_dictIsBlob(FLDict dict C4NONNULL, C4BlobKey *outKey C4NONNULL) C4API;

    /** Translates the body of the selected revision from Fleece to JSON. */
    C4StringResult c4doc_bodyAsJSON(C4Document *doc C4NONNULL, C4Error *outError) C4API;

    /** Creates a Fleece encoder for creating documents for a given database. */
    FLEncoder c4db_createFleeceEncoder(C4Database* db C4NONNULL) C4API;

    /** Encodes JSON data to Fleece, to store into a document. */
    C4SliceResult c4db_encodeJSON(C4Database* C4NONNULL, C4String jsonData, C4Error *outError) C4API;

    /** Returns the FLSharedKeys object used by the given database. */
    FLSharedKeys c4db_getFLSharedKeys(C4Database *db C4NONNULL) C4API;

    /** Returns an initialized FLDictKey for the given key string, taking into account the shared
        keys of the given database. */
    FLDictKey c4db_initFLDictKey(C4Database *db C4NONNULL, C4String string) C4API;

    /** @} */
    /** @} */
#ifdef __cplusplus
}
#endif
