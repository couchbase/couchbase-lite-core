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

    /** Translates the body of the selected revision from Fleece to JSON. */
    C4SliceResult c4doc_bodyAsJSON(C4Document *doc, C4Error *outError) C4API;

    /** Creates a Fleece encoder for creating documents for a given database. */
    struct _FLEncoder* c4db_createFleeceEncoder(C4Database*) C4API;

    /** Encodes JSON data to Fleece, to store into a document. */
    C4SliceResult c4db_encodeJSON(C4Database*, C4Slice jsonData, C4Error *outError) C4API;

    FLDictKey c4db_initFLDictKey(C4Database *db, C4Slice string) C4API;

    FLSharedKeys c4db_getFLSharedKeys(C4Database *db) C4API;

    /** @} */
    /** @} */
#ifdef __cplusplus
}
#endif
