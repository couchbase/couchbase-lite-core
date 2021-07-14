//
// PropertyEncryption.hh
//
// Copyright © 2021 Couchbase. All rights reserved.
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
    ///     `{"SSN":{"@type":"EncryptedProperty","value":"123-45-6789"}}`
    /// changes to:
    ///     `{"encrypted$SSN":{"alg":"CB_MOBILE_CUSTOM","ciphertext":"..."}}`
    constexpr fleece::slice kServerEncryptedPropKeyPrefix = "encrypted$";


    /// A heuristic to quickly weed out documents that don't need property encryption.
    /// @return  True if the JSON/Fleece data _may_ contain encryptable properties,
    ///          false if it definitely doesn't.
    static inline bool MayContainPropertiesToEncrypt(fleece::slice documentData) noexcept {
        return documentData.find(C4Document::kObjectTypeProperty)
            && documentData.find(C4Document::kObjectType_Encryptable);
    }

    /// A heuristic to quickly weed out documents that don't need property decryption.
    /// @return  True if the JSON/Fleece data _may_ contain encrypted properties,
    ///          false if it definitely doesn't.
    static inline bool MayContainPropertiesToDecrypt(fleece::slice documentData) noexcept {
        return documentData.find(kServerEncryptedPropKeyPrefix) != fleece::nullslice;
    }

    /// Finds encryptable properties in `doc` and encrypts them.
    /// @param docID  The ID of the document.
    /// @param doc  The document's parsed properties.
    /// @param callback  The client's encryption callback, from the `C4ReplicatorParameters`.
    /// @param callbackContext  The client's callback context value, also from the parameters.
    /// @param outError  On return, the error will be stored here, or 
    /// @return  The mutated document, else nullptr if nothing changed.
    fleece::MutableDict EncryptDocumentProperties(fleece::slice docID,
                                                  fleece::Dict doc,
                                                  C4ReplicatorPropertyEncryptionCallback callback,
                                                  void *callbackContext,
                                                  C4Error *outError) noexcept;

    /// Finds encrypted properties in `doc` and decrypts them.
    /// @return  The mutated document, else nullptr if nothing changed.
    fleece::MutableDict DecryptDocumentProperties(fleece::slice docID,
                                                  fleece::Dict doc,
                                                  C4ReplicatorPropertyDecryptionCallback callback,
                                                  void *callbackContext,
                                                  C4Error *outError) noexcept;

    constexpr int kPropertyEncryptionAPIVersion = 1;
}
