//
// LegacyAttachments.cc
//
// Copyright 2018-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "LegacyAttachments.hh"
#include "c4BlobStore.hh"
#include "FleeceImpl.hh"
#include "Path.hh"
#include <unordered_map>
#include <unordered_set>

namespace litecore::legacy_attachments {
    using namespace std;
    using namespace fleece;
    using namespace fleece::impl;

    bool isOldMetaProperty(slice key) { return (key.size > 0 && key[0] == '_'); }

    // Returns true if a Fleece Dict contains any top-level keys that begin with an underscore.
    bool hasOldMetaProperties(const Dict* root) {
        for ( Dict::iterator i(root); i; ++i ) {
            if ( isOldMetaProperty(i.keyString()) ) return true;
        }
        return false;
    }

    alloc_slice encodeStrippingOldMetaProperties(const Dict* root, SharedKeys* sk) {
        if ( !root ) return {};

        unordered_set<const Value*>              removeThese;  // Values to remove from doc
        unordered_map<const Value*, const Dict*> fixBlobs;     // blob -> attachment

        // Flag all "_" properties (including _attachments) for removal:
        for ( Dict::iterator i(root); i; ++i ) {
            if ( isOldMetaProperty(i.keyString()) ) removeThese.insert(i.value());
        }

        // Scan all legacy attachments and look for ones that are stand-ins for blobs:
        auto attachments = Value::asDict(root->get(C4Blob::kLegacyAttachmentsProperty));
        if ( attachments ) {
            for ( Dict::iterator i(attachments); i; ++i ) {
                slice key        = i.keyString();
                auto  attachment = Value::asDict(i.value());
                if ( !attachment ) continue;
                auto        attDigest = attachment->get(C4Blob::kDigestProperty);
                const Dict* blob      = nullptr;

                if ( key.hasPrefix("blob_"_sl) && attachment ) {
                    slice pointer = key.from(5);
                    // 2.0: blob_<index>
                    if ( pointer.size > 0 && isdigit(pointer[0]) ) {
                        removeThese.insert(attachment);
                        continue;
                    }
                    // 2.1: blob_/<property-pointer>
                    blob = Value::asDict(Path::evalJSONPointer(key.from(5), root));
                }

                if ( attDigest && blob && C4Blob::isBlob(FLDict(blob)) ) {
                    // OK, this is a stand-in; remove it. But has its digest changed?
                    removeThese.insert(attachment);
                    auto blobDigest = blob->get(C4Blob::kDigestProperty);
                    if ( blobDigest && blobDigest->asString() != attDigest->asString() ) {
                        // The digest is different, so remember to copy that to the blob:
                        fixBlobs.insert({blob, attachment});
                    }
                } else {
                    // Preserve this attachment, so don't remove _attachments itself:
                    removeThese.erase(attachments);
                }
            }
        }

        // Now re-encode, substituting the contents of the altered blobs:
        Encoder enc;
        enc.setSharedKeys(sk);
        enc.writeValue(root, [&](const Value* key, const Value* value) {
            if ( removeThese.contains(value) ) {
                // remove this entirely
                return true;
            }
            auto b = fixBlobs.find(value);
            if ( b != fixBlobs.end() ) {
                // Fix up this blob with the digest from the attachment:
                if ( key ) enc.writeKey(key);
                auto blob       = (Dict*)value;
                auto attachment = b->second;
                enc.beginDictionary(blob->count());
                for ( Dict::iterator i(blob); i; ++i ) {
                    // Write each blob property, substituting value from attachment if any:
                    slice blobKey   = i.keyString();
                    auto  blobValue = i.value();
                    auto  attValue  = attachment->get(blobKey);
                    if ( attValue || blobKey == "length"_sl || blobKey == "content_type"_sl ) blobValue = attValue;
                    if ( blobValue ) {
                        enc.writeKey(i.key());
                        enc.writeValue(blobValue);
                    }
                }
                enc.endDictionary();
                return true;
            }
            return false;
        });
        return enc.finish();
    }

}  // namespace litecore::legacy_attachments
