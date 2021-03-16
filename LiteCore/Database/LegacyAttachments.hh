//
// LegacyAttachments.hh
//
// Copyright © 2018 Couchbase. All rights reserved.
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
#include "c4Compat.h"
#include "fleece/slice.hh"

C4_ASSUME_NONNULL_BEGIN

namespace fleece::impl {
    class Dict;
    class SharedKeys;
}

namespace litecore {

    /** Utilities for dealing with 'legacy' properties like _id, _rev, _deleted, _attachments. */
    namespace legacy_attachments {

        /** Returns true if this is the name of a 1.x metadata property ("_id", "_rev", etc.) */
        bool isOldMetaProperty(fleece::slice key);

        /** Returns true if the document contains 1.x metadata properties (at top level). */
        bool hasOldMetaProperties(const fleece::impl::Dict* root);

        /** Encodes to Fleece, without any 1.x metadata properties.
            The _attachments property is treated specially, in that any entries in it that don't
            appear elsewhere in the dictionary as blobs are preserved. */
        fleece::alloc_slice encodeStrippingOldMetaProperties(const fleece::impl::Dict* root,
                                                             fleece::impl::SharedKeys* C4NULLABLE);
    }

}

C4_ASSUME_NONNULL_END
