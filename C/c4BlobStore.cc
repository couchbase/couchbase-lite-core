//
// C4BlobStore.cc
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

#include "c4BlobStore.hh"
#include "c4Database.hh"
#include "c4Document+Fleece.h"
#include "c4ExceptionUtils.hh"
#include "c4Internal.hh"
#include "BlobStore.hh"
#include "DatabaseImpl.hh"
#include "Base64.hh"
#include "Error.hh"
#include "StringUtil.hh"
#include "fleece/Fleece.hh"

using namespace std;
using namespace fleece;
using namespace litecore;


#pragma mark - C4BLOBSTORE METHODS:


C4BlobStore::C4BlobStore(slice dirPath,
                         C4DatabaseFlags flags,
                         const C4EncryptionKey* key)
{
    BlobStore::Options options = {};
    options.create = (flags & kC4DB_Create) != 0;
    options.writeable = !(flags & kC4DB_ReadOnly);
    if (key) {
        options.encryptionAlgorithm = (EncryptionAlgorithm)key->algorithm;
        options.encryptionKey = alloc_slice(key->bytes, sizeof(key->bytes));
    }
    _impl = make_unique<BlobStore>(FilePath(dirPath), &options);
}


C4BlobStore::C4BlobStore(std::unique_ptr<litecore::BlobStore> store)
:_impl(std::move(store))
{ }


C4BlobStore::~C4BlobStore() = default;


void C4BlobStore::deleteStore() {
    _impl->deleteStore();
}


int64_t C4BlobStore::getSize(C4BlobKey key) const {
    return _impl->get(asInternal(key)).contentLength();
}


alloc_slice C4BlobStore::getContents(C4BlobKey key) const {
    return _impl->get(asInternal(key)).contents();
}


alloc_slice C4BlobStore::getFilePath(C4BlobKey key) const {
    FilePath path = _impl->get(asInternal(key)).path();
    if (!path.exists())
        return nullslice;
    else if (_impl->isEncrypted())
        error::_throw(error::WrongFormat);
    else
        return alloc_slice(path);
}


C4BlobKey C4BlobStore::createBlob(slice contents, const C4BlobKey *expectedKey) {
    Blob blob = _impl->put(contents, asInternal(expectedKey));
    return external(blob.key());
}


void C4BlobStore::deleteBlob(C4BlobKey key) {
    _impl->get(asInternal(key)).del();
}


alloc_slice C4BlobStore::getBlobData(FLDict flDict, BlobStore *store) {
    Dict dict(flDict);
    if (!C4Blob::isBlob(dict))
        error::_throw(error::InvalidParameter, "Not a blob");
    auto dataProp = dict.get(C4Blob::kDataProperty);
    if (dataProp) {
        switch (dataProp.type()) {
            case kFLData:
                return alloc_slice(dataProp.asData());
            case kFLString: {
                alloc_slice data = base64::decode(dataProp.asString());
                if (!data)
                    error::_throw(error::CorruptData, "Blob data string is not valid Base64");
                return data;
            }
            default:
                error::_throw(error::CorruptData, "Blob data property has invalid type");
        }
    }
    if (auto key = C4Blob::getKey(dict); key)
        return store->get((blobKey&)*key).contents();
    else
        error::_throw(error::CorruptData, "Blob has invalid or missing digest property");
}


alloc_slice C4BlobStore::getBlobData(FLDict flDict) {
    return getBlobData(flDict, _impl.get());
}


#pragma mark - STREAMS:


C4ReadStream::C4ReadStream(const C4BlobStore &store, C4BlobKey key)
:_impl(store._impl->get(asInternal(key)).read())
{ }

C4ReadStream::C4ReadStream( C4ReadStream &&other)
:_impl(move(other._impl))
{ }

C4ReadStream::~C4ReadStream() = default;
size_t C4ReadStream::read(void *dst, size_t mx)     {return _impl->read(dst, mx);}
int64_t C4ReadStream::getLength() const             {return _impl->getLength();}
void C4ReadStream::seek(int64_t pos)                {_impl->seek(pos);}


C4WriteStream::C4WriteStream(C4BlobStore &store)
:_impl(new BlobWriteStream(*store._impl))
,_store(store)
{ }

C4WriteStream::C4WriteStream( C4WriteStream &&other)
:_impl(move(other._impl))
,_store(other._store)
{ }

C4WriteStream::~C4WriteStream() {
    try {
        if (_impl)
            _impl->close();
    } catchAndIgnore();
}

void C4WriteStream::write(fleece::slice data)         {_impl->write(data);}
uint64_t C4WriteStream::bytesWritten() const noexcept {return _impl->bytesWritten();}
C4BlobKey C4WriteStream::computeBlobKey()             {return external(_impl->computeKey());}
C4BlobKey C4WriteStream::install(const C4BlobKey *xk) {_impl->install(asInternal(xk));
                                                       return computeBlobKey();}


#pragma mark - BLOB UTILITIES:


C4BlobKey C4Blob::computeKey(slice contents) noexcept {
    return external(blobKey::computeFrom(contents));
}


alloc_slice C4Blob::keyToString(C4BlobKey key) {
    return alloc_slice(asInternal(key).base64String());
}


std::optional<C4BlobKey> C4Blob::keyFromString(slice str) noexcept {
    try {
        if (str.buf)
            return external(blobKey::withBase64(str));
    } catchAndIgnore()
    return nullopt;
}


optional<C4BlobKey> C4Blob::getKey(FLDict dict) {
    if (isBlob(dict)) {
        if (FLValue digest = FLDict_Get(dict, C4Blob::kDigestProperty); digest) {
            blobKey key;
            if (key.readFromBase64(FLValue_AsString(digest)))
                return external(key);
        }
    }
    return nullopt;
}


bool C4Blob::isBlob(FLDict dict) {
    FLValue cbltype= FLDict_Get(dict, C4Blob::kObjectTypeProperty);
    return cbltype && slice(FLValue_AsString(cbltype)) == C4Blob::kObjectType_Blob;
}


bool C4Blob::dictContainsBlobs(FLDict dict) noexcept {
    bool found = false;
    C4Blob::findBlobReferences(dict, [&](FLDict) {
        found = true;
        return false; // to stop search
    });
    return found;
}


bool C4Blob::findBlobReferences(FLDict dict, const FindBlobCallback &callback) {
    if (!dict)
        return true;
    for (DeepIterator i((FLValue)dict); i; ++i) {
        auto d = FLDict(i.value().asDict());
        if (d && C4Blob::isBlob(d)) {
            if (!callback(d))
                return false;
            i.skipChildren();
        }
    }
    return true;
}


// Heuristics for deciding whether a MIME type is compressible or not.
// See <http://www.iana.org/assignments/media-types/media-types.xhtml>

// These substrings in a MIME type mean it's definitely not compressible:
static constexpr slice kCompressedTypeSubstrings[] = {
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
static constexpr slice kGoodTypeSubstrings[] = {
    "json"_sl,
    "html"_sl,
    "xml"_sl,
    "yaml"_sl,
    {}
};

// These prefixes mean it's not compressible, unless it matches the above good-types list
// (like SVG (image/svg+xml), which is compressible.)
static constexpr slice kBadTypePrefixes[] = {
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

bool C4Blob::isCompressible(FLDict flMeta) {
    Dict meta(flMeta);
    // Don't compress an attachment with a compressed encoding:
    auto encodingProp = meta.get("encoding"_sl);
    if (encodingProp && containsAnyOf(encodingProp.asString(), kCompressedTypeSubstrings))
        return false;

    // Don't compress attachments with unknown MIME type:
    auto typeProp = meta.get("content_type"_sl);
    if (!typeProp)
        return false;
    slice type = typeProp.asString();
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
