//
// PropertyEncryption_stub.cc
//
// Copyright (C) 2020 Jens Alfke. All Rights Reserved.
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

#include "PropertyEncryption.hh"

#ifdef COUCHBASE_ENTERPRISE
    // NOTE: PropertyEncryption.cc is not in this repo, and is not open source.
    // It is part of Couchbase Lite Enterprise Edition (EE), which can be licensed in binary form
    // from Couchbase.
    #include "../../../couchbase-lite-core-EE/Replicator/PropertyEncryption.cc"
#else

// Stubs for CE:

#include "Error.hh"
#include "fleece/Mutable.hh"

namespace litecore::repl {
    using namespace fleece;

    fleece::MutableDict EncryptDocumentProperties(fleece::slice docID,
                                                  fleece::Dict doc,
                                                  C4ReplicatorPropertyEncryptionCallback callback,
                                                  void *callbackContext,
                                                  C4Error *outError) noexcept
    {
        // In CE, prevent any encryptable property from being accidentally pushed.
        // This may happen if a database was created and used with EE, and sensitive data added,
        // and then it's opened with a CE implementation.
        if (outError)
            *outError = {};
        for (DeepIterator i(doc); i; ++i) {
            if (i.key() == C4Document::kObjectTypeProperty) {
                if (i.value().asString() == C4Document::kObjectType_Encryptable) {
                    alloc_slice path = i.pathString();
                    if (outError)
                        *outError = c4error_printf(LiteCoreDomain, kC4ErrorCrypto,
                                       "Encryptable document property `%.*s` requires"
                                       " Couchbase Lite Enterprise Edition to encrypt",
                                       FMTSLICE(path));
                    break;
                }
                i.skipChildren();
            }
        }
        return nullptr;
    }


    fleece::MutableDict DecryptDocumentProperties(fleece::slice docID,
                                                  fleece::Dict doc,
                                                  C4ReplicatorPropertyDecryptionCallback callback,
                                                  void *callbackContext,
                                                  C4Error *outError) noexcept
    {
        if (outError)
            *outError = {};
        return nullptr;
    }
}

#endif
