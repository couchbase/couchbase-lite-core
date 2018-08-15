//
// Document.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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

#include "Document.hh"
#include "c4Document+Fleece.h"
#include "LegacyAttachments.hh"
#include "StringUtil.hh"
#include "FleeceImpl.hh"
#include "DeepIterator.hh"

using namespace fleece;
using namespace fleece::impl;

namespace c4Internal {

    Retained<Doc> Document::fleeceDoc() {
        if (!selectedRev.body.buf)
            return nullptr;
        return new Doc(selectedRev.body, Doc::kTrusted, _db->documentKeys());
    }

    alloc_slice Document::bodyAsJSON(bool canonical) {
        if (!selectedRev.body.buf)
            error::_throw(error::NotFound);
        auto doc = fleeceDoc();
        if (!doc)
            error::_throw(error::CorruptRevisionData);
        return doc->asDict()->toJSON(canonical);
    }


    bool Document::getBlobKey(const Dict *dict, blobKey &outKey) {
        const Value* digest = ((const Dict*)dict)->get("digest"_sl);
        return digest && outKey.readFromBase64(digest->asString());
    }


    bool Document::dictIsBlob(const Dict *dict) {
        const Value* cbltype= dict->get(C4STR(kC4ObjectTypeProperty));
        return cbltype && cbltype->asString() == slice(kC4ObjectType_Blob);
    }


    bool Document::dictIsBlob(const Dict *dict, blobKey &outKey) {
        return dictIsBlob(dict) && getBlobKey(dict, outKey);
    }


    // Finds blob references in a Fleece Dict, recursively.
    bool Document::findBlobReferences(const Dict *dict, const FindBlobCallback &callback)
    {
        for (DeepIterator i(dict); i; ++i) {
            auto d = i.value()->asDict();
            if (d && dictIsBlob(d)) {
                if (!callback(d))
                    return false;
                i.skipChildren();
            }
        }
        return true;
    }


    bool Document::isValidDocID(slice docID) {
        return docID.size >= 1 && docID.size <= 240 && docID[0] != '_'
            && isValidUTF8(docID) && hasNoControlCharacters(docID);
    }

    void Document::requireValidDocID() {
        if (!isValidDocID(docID))
            error::_throw(error::BadDocID, "Invalid docID \"%.*s\"", SPLAT(docID));
    }


    // Heuristics for deciding whether a MIME type is compressible or not.
    // See <http://www.iana.org/assignments/media-types/media-types.xhtml>

    // These substrings in a MIME type mean it's definitely not compressible:
    static const slice kCompressedTypeSubstrings[] = {
        "zip"_sl,
        "zlib"_sl,
        "pkcs"_sl,
        "mpeg"_sl,
        "mp4"_sl,
        "crypt"_sl,
        ".rar"_sl,
        "-rar"_sl,
        {}
    };

    // These substrings mean it is compressible:
    static const slice kGoodTypeSubstrings[] = {
        "json"_sl,
        "html"_sl,
        "xml"_sl,
        "yaml"_sl,
        {}
    };

    // These prefixes mean it's not compressible, unless it matches the above good-types list
    // (like SVG (image/svg+xml), which is compressible.)
    static const slice kBadTypePrefixes[] = {
        "image/"_sl,
        "audio/"_sl,
        "video/"_sl,
        {}
    };

    static bool containsAnyOf(slice type, const slice types[]) {
        for (const slice *t = &types[0]; *t; ++t)
            if (type.find(*t))
                return true;
        return false;
    }


    static bool startsWithAnyOf(slice type, const slice types[]) {
        for (const slice *t = &types[0]; *t; ++t)
            if (type.hasPrefix(*t))
                return true;
        return false;
    }

    bool Document::blobIsCompressible(const Dict *meta) {
        // Don't compress an attachment with a compressed encoding:
        auto encodingProp = meta->get("encoding"_sl);
        if (encodingProp && containsAnyOf(encodingProp->asString(), kCompressedTypeSubstrings))
            return false;

        // Don't compress attachments with unknown MIME type:
        auto typeProp = meta->get("content_type"_sl);
        if (!typeProp)
            return false;
        slice type = typeProp->asString();
        if (!type)
            return false;

        // Check the MIME type:
        string lc = type.asString();
        toLowercase(lc);
        type = lc;
        if (containsAnyOf(type, kCompressedTypeSubstrings))
            return false;
        else if (type.hasPrefix("text/"_sl) || containsAnyOf(type, kGoodTypeSubstrings))
            return true;
        else if (startsWithAnyOf(type, kBadTypePrefixes))
            return false;
        else
            return true;
    }

}
