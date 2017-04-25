//
//  Document.cc
//  LiteCore
//
//  Created by Jens Alfke on 4/19/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Document.hh"
#include "StringUtil.hh"
#include "Fleece.hh"

using namespace fleece;

namespace c4Internal {

    bool Document::isOldMetaProperty(slice key) {
        return (key.size > 0 && key[0] == '_' && key != "_attachments"_sl);
    }


    // Returns true if a Fleece Dict contains any top-level keys that begin with an underscore.
    bool Document::hasOldMetaProperties(const Dict* root) {
        for (Dict::iterator i(root); i; ++i) {
            if (isOldMetaProperty(i.keyString()))
                return true;
        }
        return false;
    }


    // Encodes a Dict, skipping top-level properties whose names begin with an underscore.
    alloc_slice Document::encodeStrippingOldMetaProperties(const Dict* root) {
        Encoder e;
        e.beginDictionary(root->count());
        for (Dict::iterator i(root); i; ++i) {
            slice key = i.keyString();
            if (isOldMetaProperty(key))
                continue;
            e.writeKey(key);
            e.writeValue(i.value());
        }
        e.endDictionary();
        return e.extractOutput();
    }


    // Finds blob references in a Fleece value, recursively.
    void Document::findBlobReferences(const Value *val, const FindBlobCallback &callback) {
        auto d = val->asDict();
        if (d) {
            findBlobReferences(d, callback);
            return;
        }
        auto a = val->asArray();
        if (a) {
            for (Array::iterator i(a); i; ++i)
                findBlobReferences(i.value(), callback);
        }
    }


    bool Document::dictIsBlob(const Dict *dict, blobKey &outKey) {
        auto cbltype = dict->get("_cbltype"_sl);
        if (!cbltype || cbltype->asString() != "blob"_sl)
            return false;
        auto digest = ((const Dict*)dict)->get("digest"_sl);
        if (!digest)
            return false;
        return outKey.readFromBase64(digest->asString());
    }


    // Finds blob references in a Fleece Dict, recursively.
    void Document::findBlobReferences(const Dict *dict, const FindBlobCallback &callback)
    {
        if (dict->get("_cbltype"_sl)) {
            blobKey key;
            if (dictIsBlob(dict, key)) {
                auto lengthVal = dict->get("length"_sl);
                uint64_t length = lengthVal ? lengthVal->asUnsigned() : 0;
                callback(key, length);
            }
        } else {
            for (Dict::iterator i(dict); i; ++i)
                findBlobReferences(i.value(), callback);
        }
    }


    bool Document::isValidDocID(slice docID) {
        return docID.size >= 1 && docID.size <= 240 && docID[0] != '_'
            && isValidUTF8(docID) && hasNoControlCharacters(docID);
    }

    void Document::requireValidDocID() {
        if (!isValidDocID(docID))
            error::_throw(error::BadDocID, "Invalid docID \"%.*s\"", SPLAT(docID));
    }

}
