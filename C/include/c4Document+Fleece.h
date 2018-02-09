//
// c4Document+Fleece.h
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
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
    C4SliceResult c4doc_encodeStrippingOldMetaProperties(FLDict doc) C4API;

    /** Returns true if the given dictionary is a [reference to a] blob; if so, gets its key.
        (This function cannot recognize child dictionaries of "_attachments", because it's not
        possible to look at the parent of a Fleece value.) */
    bool c4doc_dictIsBlob(FLDict dict C4NONNULL,
                          FLSharedKeys sk,
                          C4BlobKey *outKey C4NONNULL) C4API;

    bool c4doc_dictContainsBlobs(FLDict dict C4NONNULL, FLSharedKeys sk) C4API;

    /** Given a dictionary that's a reference to a blob, determines whether it's worth trying to
        compress the blob's data. This is done by examining the "encoding" and "content_type"
        properties and using heuristics to detect types that are already compressed, like gzip
        or JPEG. If no warning flags are found it will return true. */
    bool c4doc_blobIsCompressible(FLDict blobDict C4NONNULL, FLSharedKeys sk);

    /** Translates the body of the selected revision from Fleece to JSON. */
    C4StringResult c4doc_bodyAsJSON(C4Document *doc C4NONNULL,
                                    bool canonical,
                                    C4Error *outError) C4API;

    /** Creates a Fleece encoder for creating documents for a given database. */
    FLEncoder c4db_createFleeceEncoder(C4Database* db C4NONNULL) C4API;

    /** Returns a shared Fleece encoder for creating documents for a given database.
        DO NOT FREE THIS ENCODER. Instead, call FLEncoder_Reset() when finished. */
    FLEncoder c4db_getSharedFleeceEncoder(C4Database* db) C4API;

    /** Encodes JSON data to Fleece, to store into a document. */
    C4SliceResult c4db_encodeJSON(C4Database* C4NONNULL, C4String jsonData, C4Error *outError) C4API;

    /** Returns the FLSharedKeys object used by the given database. */
    FLSharedKeys c4db_getFLSharedKeys(C4Database *db C4NONNULL) C4API;

    /** Returns an initialized FLDictKey for the given key string, taking into account the shared
        keys of the given database.

        Warning: the input string's memory MUST remain valid for as long as the FLDictKey is in
        use! (The FLDictKey stores a pointer to the string, but does not copy it.) */
    FLDictKey c4db_initFLDictKey(C4Database *db C4NONNULL, C4String string) C4API;

    /** @} */
    /** @} */
#ifdef __cplusplus
}
#endif
