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
#include "fleece/Fleece.h"

C4_ASSUME_NONNULL_BEGIN
C4API_BEGIN_DECLS

    /** \defgroup Documents Documents
        @{ */


    /** \name Fleece-related
        @{ */

    /** The sub-document property that identifies it as a special type of object.
        For example, a blob is represented as `{"@type":"blob", "digest":"xxxx", ...}` */
    #define kC4ObjectTypeProperty "@type"


    /** Value of `kC4ObjectTypeProperty` that denotes a blob. */
    #define kC4ObjectType_Blob "blob"

    /** Blob dict property containing a digest of the data. (Required if "data" is absent) */
    #define kC4BlobDigestProperty "digest"

    /** Blob dict property containing the data itself. (Required if "digest" is absent) */
    #define kC4BlobDataProperty "data"

    /** Top-level document property whose value is a CBL 1.x / CouchDB attachments container. */
    #define kC4LegacyAttachmentsProperty "_attachments"


    /** Value of `kC4ObjectTypeProperty` that denotes an encryptable value. */
    #define kC4ObjectType_Encryptable "EncryptedProperty"

    /** Encryptable-value property containing the actual value; may be any JSON/Fleece type.
        Required if `ciphertext` is absent. */
    #define kC4EncryptableValueProperty "value"

    /** Encryptable-value property containing the already-encrypted data as a Base64-encoded string.
        Required if `value` is absent. */
    #define kC4EncryptedCiphertextProperty "ciphertext"


    /** Returns the properties of the selected revision, i.e. the root Fleece Dict. */
    FLDict c4doc_getProperties(C4Document* C4NONNULL) C4API;

    /** Returns a Fleece document reference created from the selected revision.
        Caller must release the reference! */
    FLDoc c4doc_createFleeceDoc(C4Document*);

    /** Resolves a conflict between two leaf revisions.
        Identical to `c4doc_resolveConflict` except that it takes the merged body as a Fleece Dict,
        instead of pre-encoded Fleece data. */
    bool c4doc_resolveConflict2(C4Document *doc,
                                C4String winningRevID,
                                C4String losingRevID,
                                FLDict C4NULLABLE mergedProperties,
                                C4RevisionFlags mergedFlags,
                                C4Error* C4NULLABLE error) C4API;

    /** Returns the C4Document, if any, that contains the given Fleece value. */
    C4Document* c4doc_containingValue(FLValue value);

    /** Returns true if this is the name of a 1.x metadata property ("_id", "_rev", "_deleted".)
        Does NOT return true for "_attachments" because that property isn't obsolete. */
    bool c4doc_isOldMetaProperty(C4String prop) C4API;

    /** Returns true if the document contains 1.x metadata properties at top level.
        Does NOT return true for "_attachments" because that property isn't obsolete. */
    bool c4doc_hasOldMetaProperties(FLDict doc) C4API;

    /** Re-encodes to Fleece, without any 1.x metadata properties. Old-style attachments that
        _don't_ refer to blobs will be removed; others are kept. */
    C4SliceResult c4doc_encodeStrippingOldMetaProperties(FLDict doc,
                                                         FLSharedKeys sk,
                                                         C4Error* C4NULLABLE outError) C4API;

    /** Decodes the dict's "digest" property to a C4BlobKey.
        Returns false if there is no such property or it's not a valid blob key. */
    bool c4doc_getDictBlobKey(FLDict dict,
                              C4BlobKey *outKey);

    /** Returns true if the given dictionary is a [reference to a] blob; if so, gets its key.
        (This function cannot recognize child dictionaries of "_attachments", because it's not
        possible to look at the parent of a Fleece value.) */
    bool c4doc_dictIsBlob(FLDict dict,
                          C4BlobKey *outKey) C4API;

    bool c4doc_dictContainsBlobs(FLDict dict) C4API;

    /** Returns the contents of a blob dictionary, whether they're inline in the "data" property,
        or indirectly referenced via the "digest" property.
        @note  You can omit the C4BlobStore, but if the blob has no inline data the function will
            give up and return a null slice (and clear the error, since this isn't a failure.)
        @param dict  A blob dictionary.
        @param blobStore  The database's BlobStore, or NULL to suppress loading blobs from disk.
        @param outError  On failure, the error will be written here.
        @return  The blob data, or null on failure. */
    C4SliceResult c4doc_getBlobData(FLDict dict,
                                    C4BlobStore* C4NULLABLE blobStore,
                                    C4Error* C4NULLABLE outError) C4API;

    /** Given a dictionary that's a reference to a blob, determines whether it's worth trying to
        compress the blob's data. This is done by examining the "encoding" and "content_type"
        properties and using heuristics to detect types that are already compressed, like gzip
        or JPEG. If no warning flags are found it will return true. */
    bool c4doc_blobIsCompressible(FLDict blobDict);

    /** Translates the body of the selected revision from Fleece to JSON. */
    C4StringResult c4doc_bodyAsJSON(C4Document *doc,
                                    bool canonical,
                                    C4Error* C4NULLABLE outError) C4API;

    /** Creates a Fleece encoder for creating documents for a given database. */
    FLEncoder c4db_createFleeceEncoder(C4Database* db) C4API;

    /** Returns a shared Fleece encoder for creating documents for a given database.
        DO NOT FREE THIS ENCODER. Instead, call FLEncoder_Reset() when finished. */
    FLEncoder c4db_getSharedFleeceEncoder(C4Database* db) C4API;

    /** Encodes JSON data to Fleece, to store into a document. */
    C4SliceResult c4db_encodeJSON(C4Database*, C4String jsonData, C4Error* C4NULLABLE outError) C4API;

    /** Returns the FLSharedKeys object used by the given database. */
    FLSharedKeys c4db_getFLSharedKeys(C4Database *db) C4API;

    /** @} */
    /** @} */

C4API_END_DECLS
C4_ASSUME_NONNULL_END
