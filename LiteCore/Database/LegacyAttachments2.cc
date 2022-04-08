//
// LegacyAttachments2.cc
//
// Copyright © 2022 Couchbase. All rights reserved.
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

#include "LegacyAttachments.hh"
#include "c4BlobStore.hh"
#include "c4Document.hh"
#include "fleece/Fleece.hh"
#include <unordered_set>

// This source file is separate from LegacyAttachments.cc because, for historical reasons, those
// functions are implemented with the fleece::impl API while the ones here use the public Fleece
// API, and the two APIs don't mix well in a single source file.

namespace litecore::legacy_attachments {
    using namespace std;
    using namespace fleece;


    static bool isBlobOrAttachment(FLDeepIterator i, C4BlobKey *blobKey, bool attachmentsOnly) {
        auto dict = FLValue_AsDict(FLDeepIterator_GetValue(i));
        if (!dict)
            return false;

        // Get the digest:
        if (auto key = C4Blob::keyFromDigestProperty(dict); key)
            *blobKey = *key;
        else
            return false;

        // Check if it's a blob:
        if (!attachmentsOnly && C4Blob::isBlob(dict)) {
            return true;
        } else {
            // Check if it's an old-school attachment, i.e. in a top level "_attachments" dict:
            FLPathComponent* path;
            size_t depth;
            FLDeepIterator_GetPath(i, &path, &depth);
            return depth == 2 && path[0].key == C4Blob::kLegacyAttachmentsProperty;
        }
    }


    void findBlobReferences(FLDict root,
                            bool unique,
                            const FindBlobCallback &callback,
                            bool attachmentsOnly)
    {
        unordered_set<string> found;
        FLDeepIterator i = FLDeepIterator_New(FLValue(root));
        for (; FLDeepIterator_GetValue(i); FLDeepIterator_Next(i)) {
            C4BlobKey blobKey;
            if (isBlobOrAttachment(i, &blobKey, attachmentsOnly)) {
                if (!unique || found.emplace((const char*)&blobKey, sizeof(blobKey)).second) {
                    auto blob = Value(FLDeepIterator_GetValue(i)).asDict();
                    callback(i, blob, blobKey);
                }
                FLDeepIterator_SkipChildren(i);
            }
        }
        FLDeepIterator_Free(i);
    }


    void encodeRevWithLegacyAttachments(FLEncoder fl_enc, FLDict root, unsigned revpos) {
        SharedEncoder enc(fl_enc);
        enc.beginDict();

        // Write existing properties except for _attachments:
        Dict oldAttachments;
        for (Dict::iterator i(root); i; ++i) {
            slice key = i.keyString();
            if (key == C4Blob::kLegacyAttachmentsProperty) {
                oldAttachments = i.value().asDict();    // remember _attachments dict for later
            } else {
                enc.writeKey(key);
                enc.writeValue(i.value());
            }
        }

        // Now write _attachments:
        enc.writeKey(C4Blob::kLegacyAttachmentsProperty);
        enc.beginDict();
        // First pre-existing legacy attachments, if any:
        for (Dict::iterator i(oldAttachments); i; ++i) {
            slice key = i.keyString();
            if (!key.hasPrefix("blob_"_sl)) {
                // TODO: Should skip this entry if a blob with the same digest exists
                enc.writeKey(key);
                enc.writeValue(i.value());
            }
        }

        // Then entries for blobs found in the document:
        findBlobReferences(root, false, [&](FLDeepIterator di, FLDict blob, C4BlobKey blobKey) {
            alloc_slice path(FLDeepIterator_GetJSONPointer(di));
            if (path.hasPrefix("/_attachments/"_sl))
                return;
            string attName = string("blob_") + string(path);
            enc.writeKey(slice(attName));
            enc.beginDict();
            for (Dict::iterator i(blob); i; ++i) {
                slice key = i.keyString();
                if (key != C4Document::kObjectTypeProperty && key != "stub"_sl) {
                    enc.writeKey(key);
                    enc.writeValue(i.value());
                }
            }
            enc.writeKey("stub"_sl);
            enc.writeBool(true);
            enc.writeKey("revpos"_sl);
            enc.writeInt(revpos);
            enc.endDict();
        });
        enc.endDict();

        enc.endDict();
    }

}
