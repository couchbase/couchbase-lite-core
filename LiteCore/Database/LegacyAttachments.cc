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
#include "c4Document.hh"
#include "FleeceImpl.hh"
#include "Path.hh"
#include "fleece/Expert.hh"
#include <unordered_map>
#include <unordered_set>

namespace litecore::legacy_attachments {
    using namespace std;
    using namespace fleece;

    static inline const fleece::impl::Dict* asImpl(FLDict dict) {
        return reinterpret_cast<const fleece::impl::Dict*>(dict);
    }

    static inline fleece::impl::SharedKeys* asImpl(FLSharedKeys sk) {
        return reinterpret_cast<fleece::impl::SharedKeys*>(sk);
    }

    bool isOldMetaProperty(slice key) { return (key.size > 0 && key[0] == '_'); }

    bool hasOldMetaProperties(fleece::Dict root) { return hasOldMetaProperties(asImpl(root)); }

    // Returns true if a Fleece Dict contains any top-level keys that begin with an underscore.
    bool hasOldMetaProperties(const impl::Dict* root) {
        for ( impl::Dict::iterator i(root); i; ++i ) {
            if ( isOldMetaProperty(i.keyString()) ) return true;
        }
        return false;
    }

    fleece::alloc_slice encodeStrippingOldMetaProperties(fleece::Dict root, fleece::SharedKeys sk) {
        return encodeStrippingOldMetaProperties(asImpl(root), asImpl(sk));
    }

    fleece::alloc_slice encodeStrippingOldMetaProperties(const impl::Dict* root, impl::SharedKeys* sk) {
        if ( !root ) return {};

        unordered_set<const impl::Value*>                    removeThese;  // Values to remove from doc
        unordered_map<const impl::Value*, const impl::Dict*> fixBlobs;     // blob -> attachment

        // Flag all "_" properties (including _attachments) for removal:
        for ( impl::Dict::iterator i(root); i; ++i ) {
            if ( isOldMetaProperty(i.keyString()) ) removeThese.insert(i.value());
        }

        // Scan all legacy attachments and look for ones that are stand-ins for blobs:
        auto attachments = impl::Value::asDict(root->get(C4Blob::kLegacyAttachmentsProperty));
        if ( attachments ) {
            for ( impl::Dict::iterator i(attachments); i; ++i ) {
                slice key        = i.keyString();
                auto  attachment = impl::Value::asDict(i.value());
                if ( !attachment ) continue;
                auto              attDigest = attachment->get(C4Blob::kDigestProperty);
                const impl::Dict* blob      = nullptr;

                if ( key.hasPrefix("blob_"_sl) && attachment ) {
                    slice pointer = key.from(5);
                    // 2.0: blob_<index>
                    if ( pointer.size > 0 && isdigit(pointer[0]) ) {
                        removeThese.insert(attachment);
                        continue;
                    }
                    // 2.1: blob_/<property-pointer>
                    blob = impl::Value::asDict(impl::Path::evalJSONPointer(key.from(5), root));
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
        impl::Encoder enc;
        enc.setSharedKeys(sk);
        enc.writeValue(root, [&](const impl::Value* key, const impl::Value* value) {
            if ( removeThese.find(value) != removeThese.end() ) {
                // remove this entirely
                return true;
            }
            auto b = fixBlobs.find(value);
            if ( b != fixBlobs.end() ) {
                // Fix up this blob with the digest from the attachment:
                if ( key ) enc.writeKey(key);
                auto blob       = (impl::Dict*)value;
                auto attachment = b->second;
                enc.beginDictionary(blob->count());
                for ( impl::Dict::iterator i(blob); i; ++i ) {
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

    bool isBlobOrAttachment(FLDeepIterator i, C4BlobKey* blobKey, bool noBlobs) {
        auto dict = FLValue_AsDict(FLDeepIterator_GetValue(i));
        if ( !dict ) return false;

        // Get the digest:
        if ( auto key = C4Blob::keyFromDigestProperty(dict); key ) *blobKey = *key;
        else
            return false;

        // Check if it's a blob:
        if ( !noBlobs && C4Blob::isBlob(dict) ) {
            return true;
        } else {
            // Check if it's an old-school attachment, i.e. in a top level "_attachments" dict:
            FLPathComponent* path;
            size_t           depth;
            FLDeepIterator_GetPath(i, &path, &depth);
            return depth == 2 && path[0].key == C4Blob::kLegacyAttachmentsProperty;
        }
    }

    using FindBlobCallback = fleece::function_ref<void(FLDeepIterator, Dict blob, const C4BlobKey& key)>;

    bool hasBlobReferences(Dict root, bool noBlobs) {
        // This method is non-static because it references _disableBlobSupport, but it's
        // thread-safe.
        bool           found = false;
        FLDeepIterator i     = FLDeepIterator_New(root);
        for ( ; FLDeepIterator_GetValue(i); FLDeepIterator_Next(i) ) {
            C4BlobKey blobKey;
            if ( isBlobOrAttachment(i, &blobKey, noBlobs) ) {
                found = true;
                break;
            }
        }
        FLDeepIterator_Free(i);
        return found;
    }

    void findBlobReferences(Dict root, bool unique, bool noBlobs, const FindBlobCallback& callback) {
        // This method is non-static because it references _disableBlobSupport, but it's
        // thread-safe.
        unordered_set<string> found;
        FLDeepIterator        i = FLDeepIterator_New(root);
        for ( ; FLDeepIterator_GetValue(i); FLDeepIterator_Next(i) ) {
            C4BlobKey blobKey;
            if ( isBlobOrAttachment(i, &blobKey, noBlobs) ) {
                if ( !unique || found.emplace((const char*)&blobKey, sizeof(blobKey)).second ) {
                    auto blob = Value(FLDeepIterator_GetValue(i)).asDict();
                    callback(i, blob, blobKey);
                }
                FLDeepIterator_SkipChildren(i);
            }
        }
        FLDeepIterator_Free(i);
    }

    void encodeRevWithLegacyAttachments(Encoder& enc, Dict root, unsigned revpos) {
        enc.beginDict();

        // Write existing properties except for _attachments:
        Dict oldAttachments;
        for ( Dict::iterator i(root); i; ++i ) {
            slice key = i.keyString();
            if ( key == C4Blob::kLegacyAttachmentsProperty ) {
                oldAttachments = i.value().asDict();  // remember _attachments dict for later
            } else {
                enc.writeKey(key);
                enc.writeValue(i.value());
            }
        }

        // Now write _attachments:
        enc.writeKey(C4Blob::kLegacyAttachmentsProperty);
        enc.beginDict();
        // First pre-existing legacy attachments, if any:
        for ( Dict::iterator i(oldAttachments); i; ++i ) {
            slice key = i.keyString();
            if ( !key.hasPrefix("blob_"_sl) ) {
                // TODO: Should skip this entry if a blob with the same digest exists
                enc.writeKey(key);
                enc.writeValue(i.value());
            }
        }

        // Then entries for blobs found in the document:
        findBlobReferences(root, false, false, [&](FLDeepIterator di, FLDict blob, C4BlobKey blobKey) {
            alloc_slice path(FLDeepIterator_GetJSONPointer(di));
            if ( path.hasPrefix("/_attachments/"_sl) ) return;
            string attName = string("blob_") + string(path);
            enc.writeKey(slice(attName));
            enc.beginDict();
            for ( Dict::iterator i(blob); i; ++i ) {
                slice key = i.keyString();
                if ( key != C4Document::kObjectTypeProperty && key != "stub"_sl ) {
                    enc.writeKey(key);
                    enc.writeValue(i.value());
                }
            }
            enc.writeKey("stub"_sl);
            enc.writeBool(true);
            if ( revpos > 0 ) {
                enc.writeKey("revpos"_sl);
                enc.writeInt(revpos);
            }
            enc.endDict();
        });
        enc.endDict();

        enc.endDict();
    }

}  // namespace litecore::legacy_attachments
