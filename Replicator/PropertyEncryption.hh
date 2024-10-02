//
// PropertyEncryption.hh
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "c4ReplicatorTypes.h"
#include "c4Document.hh"
#include "fleece/slice.hh"
#include "fleece/Fleece.hh"

namespace litecore::repl {

    /// The key-prefix used in the Couchbase Server SDKs to tag an encrypted property.
    /// This is added during encryption to the key of an encrypted property *in its containing
    /// dictionary*. For example, the document
    ///     `{"SSN":{"@type":"encryptable","value":"123-45-6789"}}`
    /// changes to:
    ///     `{"encrypted$SSN":{"alg":"CB_MOBILE_CUSTOM","ciphertext":"..."}}`
    constexpr fleece::slice kServerEncryptedPropKeyPrefix = "encrypted$";

    /// A heuristic to quickly weed out documents that don't need property encryption.
    /// @return  True if the JSON/Fleece data _may_ contain encryptable properties,
    ///          false if it definitely doesn't.
    inline bool MayContainPropertiesToEncrypt(fleece::slice documentData) noexcept {
        return documentData.find(C4Document::kObjectTypeProperty)
               && documentData.find(C4Document::kObjectType_Encryptable);
    }

    /// A heuristic to quickly weed out documents that don't need property decryption.
    /// @return  True if the JSON/Fleece data _may_ contain encrypted properties,
    ///          false if it definitely doesn't.
    inline bool MayContainPropertiesToDecrypt(fleece::slice documentData) noexcept {
        return documentData.find(kServerEncryptedPropKeyPrefix) != fleece::nullslice;
    }

    /// Finds encryptable properties in `doc` and encrypts them.
    /// @param docID  The ID of the document.
    /// @param doc  The document's parsed properties.
    /// @param callback  The client's encryption callback, from the `C4ReplicatorParameters`.
    /// @param callbackContext  The client's callback context value, also from the parameters.
    /// @param outError  On return, the error if any, else a zero C4Error. (May not be NULL!)
    /// @return  The mutated document, else nullptr if nothing changed or an error occurred.
    fleece::MutableDict EncryptDocumentProperties(C4CollectionSpec collection, fleece::slice docID, fleece::Dict doc,
                                                  C4ReplicatorPropertyEncryptionCallback callback,
                                                  void* callbackContext, C4Error* outError) noexcept;

    /// Finds encrypted properties in `doc` and decrypts them.
    /// @param docID  The ID of the document.
    /// @param doc  The document's parsed properties.
    /// @param callback  The client's decryption callback, from the `C4ReplicatorParameters`.
    /// @param callbackContext  The client's callback context value, also from the parameters.
    /// @param outError  On return, the error if any, else a zero C4Error. (May not be NULL!)
    /// @return  The mutated document, else nullptr if nothing changed or an error occurred.
    fleece::MutableDict DecryptDocumentProperties(C4CollectionSpec collection, fleece::slice docID, fleece::Dict doc,
                                                  C4ReplicatorPropertyDecryptionCallback callback,
                                                  void* callbackContext, C4Error* outError) noexcept;

    constexpr int kPropertyEncryptionAPIVersion = 1;
}  // namespace litecore::repl
