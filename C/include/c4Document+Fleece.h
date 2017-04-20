//
//  c4Document+Fleece.h
//  LiteCore
//
//  Created by Jens Alfke on 10/25/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once

#include "c4Document.h"
#include "Fleece.h"


#ifdef __cplusplus
extern "C" {
#endif


    /** \defgroup Documents Documents
        @{ */


    /** \name Fleece-related
        @{ */

    /** Returns true if this is the name of a 1.x metadata property ("_id", "_rev", etc.)
     Does NOT return true for "_attachments" because that property isn't obsolete. */
    bool c4doc_isOldMetaProperty(C4Slice prop) C4API;

    /** Returns true if the document contains 1.x metadata properties at top level. */
    bool c4doc_hasOldMetaProperties(FLDict doc) C4API;

    /** Returns true if the given dictionary is a [reference to a] blob; if so, gets its key. */
    bool c4doc_dictIsBlob(FLDict dict, C4BlobKey *outKey) C4API;

    /** Re-encodes to Fleece, without any 1.x metadata properties. */
    C4SliceResult c4doc_encodeStrippingOldMetaProperties(FLDict doc) C4API;

    /** Translates the body of the selected revision from Fleece to JSON. */
    C4StringResult c4doc_bodyAsJSON(C4Document *doc, C4Error *outError) C4API;

    /** Creates a Fleece encoder for creating documents for a given database. */
    FLEncoder c4db_createFleeceEncoder(C4Database*) C4API;

    /** Encodes JSON data to Fleece, to store into a document. */
    C4SliceResult c4db_encodeJSON(C4Database*, C4String jsonData, C4Error *outError) C4API;

    FLDictKey c4db_initFLDictKey(C4Database *db, C4String string) C4API;

    FLSharedKeys c4db_getFLSharedKeys(C4Database *db) C4API;

    /** @} */
    /** @} */
#ifdef __cplusplus
}
#endif
