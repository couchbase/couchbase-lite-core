//
// C4BlobStore.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4BlobStore.hh"
#include "c4ExceptionUtils.hh"
#include "c4Document+Fleece.h"
#include "BlobStreams.hh"
#include "Base64.hh"
#include "EncryptedStream.hh"
#include "Error.hh"
#include "FilePath.hh"
#include "Logging.hh"
#include "StringUtil.hh"
#include "fleece/Fleece.hh"
#include <array>

using namespace std;
using namespace fleece;
using namespace litecore;

namespace C4Blob {
    const slice kObjectTypeProperty = kC4ObjectTypeProperty;
}

#pragma mark - C4BLOBKEY:


static constexpr slice kBlobDigestStringPrefix = "sha1-",  // prefix of ASCII form of blob key ("digest" property)
        kBlobFilenameSuffix                    = ".blob";  // suffix of blob files in the store

static constexpr size_t kBlobDigestStringLength =
                                ((sizeof(C4BlobKey::bytes) + 2) / 3) * 4,  // Length of base64 w/o prefix
        kBlobFilenameLength = kBlobDigestStringLength + kBlobFilenameSuffix.size;

static SHA1& digest(C4BlobKey& key) { return (SHA1&)key.bytes; }

static const SHA1& digest(const C4BlobKey& key) { return (const SHA1&)key.bytes; }

C4BlobKey C4BlobKey::computeDigestOfContent(slice content) noexcept {
    C4BlobKey key{};
    digest(key).computeFrom(content);
    return key;
}

string C4BlobKey::digestString() const { return string(kBlobDigestStringPrefix) + digest(*this).asBase64(); }

static std::optional<C4BlobKey> BlobKeyFromBase64(slice data) noexcept {
    if ( data.size != kBlobDigestStringLength ) return nullopt;
    // Decoder always writes a multiple of 3 bytes, so round up:
    uint8_t   buf[sizeof(C4BlobKey) + 2];
    C4BlobKey key;
    if ( !digest(key).setDigest(fleece::base64::decode(data, buf, sizeof(buf))) ) return nullopt;
    return key;
}

std::optional<C4BlobKey> C4BlobKey::withDigestString(slice base64String) noexcept {
    if ( base64String.hasPrefix(kBlobDigestStringPrefix) ) base64String.moveStart(kBlobDigestStringPrefix.size);
    else
        return nullopt;
    return BlobKeyFromBase64(base64String);
}

static string BlobKeyToFilename(const C4BlobKey& key) {
    // Change '/' characters in the base64 into '_':
    string str = digest(key).asBase64();
    replace(str.begin(), str.end(), '/', '_');
    str.append(kBlobFilenameSuffix);
    return str;
}

static optional<C4BlobKey> BlobKeyFromFilename(slice filename) noexcept {
    if ( filename.size != kBlobFilenameLength || !filename.hasSuffix(kBlobFilenameSuffix) ) return nullopt;
    // Change '_' back into '/' for base64:
    char base64buf[kBlobDigestStringLength];
    memcpy(base64buf, filename.buf, sizeof(base64buf));
    std::replace(&base64buf[0], &base64buf[sizeof(base64buf)], '_', '/');
    return BlobKeyFromBase64({base64buf, sizeof(base64buf)});
}

#pragma mark - C4BLOBSTORE METHODS:

C4BlobStore::C4BlobStore(slice dirPath, C4DatabaseFlags flags, const C4EncryptionKey& key)
    : _dirPath(dirPath), _flags(flags), _encryptionKey(key) {
    FilePath dir(_dirPath, "");
    if ( dir.exists() ) {
        dir.mustExistAsDir();
    } else {
        if ( !(flags & kC4DB_Create) ) error::_throw(error::NotFound);
        dir.mkdir();
    }
}

C4BlobStore::~C4BlobStore() = default;

void C4BlobStore::deleteStore() { dir().delRecursive(); }

FilePath C4BlobStore::dir() const { return {_dirPath, ""}; }

FilePath C4BlobStore::pathForKey(C4BlobKey key) const { return {_dirPath, BlobKeyToFilename(key)}; }

alloc_slice C4BlobStore::getFilePath(C4BlobKey key) const {
    FilePath path = pathForKey(key);
    if ( !path.exists() ) return nullslice;
    else if ( isEncrypted() )
        error::_throw(error::WrongFormat);
    else
        return alloc_slice(path);
}

int64_t C4BlobStore::getSize(C4BlobKey key) const {
    int64_t length = pathForKey(key).dataSize();
    if ( length >= 0 && isEncrypted() ) length -= EncryptedReadStream::kFileSizeOverhead;
    return length;
}

alloc_slice C4BlobStore::getContents(C4BlobKey key) const {
    auto reader = getReadStream(key);
    return reader->readAll();
}

unique_ptr<SeekableReadStream> C4BlobStore::getReadStream(C4BlobKey key) const {
    return OpenBlobReadStream(pathForKey(key), litecore::EncryptionAlgorithm(_encryptionKey.algorithm),
                              slice(&_encryptionKey.bytes, sizeof(_encryptionKey.bytes)));
}

alloc_slice C4BlobStore::getBlobData(FLDict flDict) const {
    Dict dict(flDict);
    if ( !C4Blob::isBlob(dict) ) {
        error::_throw(error::InvalidParameter, "Not a blob");
    } else if ( auto dataProp = dict.get(C4Blob::kDataProperty); dataProp ) {
        switch ( dataProp.type() ) {
            case kFLData:
                return alloc_slice(dataProp.asData());
            case kFLString:
                {
                    alloc_slice data = base64::decode(dataProp.asString());
                    if ( !data ) error::_throw(error::CorruptData, "Blob data string is not valid Base64");
                    return data;
                }
            default:
                error::_throw(error::CorruptData, "Blob data property has invalid type");
        }
    } else if ( auto key = C4Blob::keyFromDigestProperty(dict); key ) {
        return getContents(*key);
    } else {
        error::_throw(error::CorruptData, "Blob has invalid or missing digest property");
    }
}

#pragma mark - CREATING / DELETING BLOBS:

C4BlobKey C4BlobStore::createBlob(slice contents, const C4BlobKey* expectedKey) {
    auto stream = getWriteStream();
    stream->write(contents);
    return install(stream.get(), expectedKey);
}

unique_ptr<BlobWriteStream> C4BlobStore::getWriteStream() {
    return make_unique<BlobWriteStream>(_dirPath, litecore::EncryptionAlgorithm(_encryptionKey.algorithm),
                                        slice(&_encryptionKey.bytes, sizeof(_encryptionKey.bytes)));
}

C4BlobKey C4BlobStore::install(BlobWriteStream* writer, const C4BlobKey* expectedKey) {
    writer->close();
    C4BlobKey key = writer->computeKey();
    if ( expectedKey && *expectedKey != key ) error::_throw(error::CorruptData);
    writer->install(pathForKey(key));
    return key;
}

void C4BlobStore::deleteBlob(C4BlobKey key) { pathForKey(key).del(); }

#pragma mark - HOUSEKEEPING:

unsigned C4BlobStore::deleteAllExcept(const unordered_set<C4BlobKey>& inUse) {
    unsigned numDeleted = 0;
    dir().forEachFile([&](const FilePath& path) {
        const string& filename = path.fileName();
        if ( auto key = BlobKeyFromFilename(filename); key ) {
            if ( find(inUse.cbegin(), inUse.cend(), *key) == inUse.cend() ) {
                ++numDeleted;
                LogToAt(DBLog, Verbose, "Deleting unused blob '%s", filename.c_str());
                path.del();
            }
        } else {
            Warn("Skipping unknown file '%s' in Attachments directory", filename.c_str());
        }
    });
    return numDeleted;
}

void C4BlobStore::copyBlobsTo(C4BlobStore& toStore) {
    dir().forEachFile([&](const FilePath& path) {
        const string& filename = path.fileName();
        if ( auto key = BlobKeyFromFilename(filename); key ) {
            auto    src = getReadStream(*key);
            auto    dst = toStore.getWriteStream();
            uint8_t buffer[4096];
            size_t  bytesRead;
            while ( (bytesRead = src->read(buffer, sizeof(buffer))) > 0 ) { dst->write(slice(buffer, bytesRead)); }
            toStore.install(dst.get(), &*key);
        } else {
            Warn("Skipping unknown file '%s' in Attachments directory", filename.c_str());
        }
    });
}

void C4BlobStore::replaceWith(C4BlobStore& other) {
    other.dir().moveToReplacingDir(dir(), true);
    _flags         = other._flags;
    _encryptionKey = other._encryptionKey;
}

#pragma mark - STREAMS:

C4ReadStream::C4ReadStream(const C4BlobStore& store, C4BlobKey key) : _impl(store.getReadStream(key)) {}

C4ReadStream::C4ReadStream(C4ReadStream&& other) noexcept : _impl(std::move(other._impl)) {}

C4ReadStream::~C4ReadStream() = default;

// clang-format off
size_t   C4ReadStream::read(void *dst, size_t mx)    { return _impl->read(dst, mx); }
uint64_t C4ReadStream::getLength() const             { return _impl->getLength(); }
void     C4ReadStream::seek(uint64_t pos)            { _impl->seek(pos); }

// clang-format on

C4WriteStream::C4WriteStream(C4BlobStore& store) : _impl(store.getWriteStream()), _store(store) {}

C4WriteStream::C4WriteStream(C4WriteStream&& other) noexcept
    : InstanceCounted(std::move(other)), _impl(std::move(other._impl)), _store(other._store) {}

C4WriteStream::~C4WriteStream() {
    try {
        if ( _impl ) _impl->close();
    }
    catchAndWarn();
}

// clang-format off
void      C4WriteStream::write(fleece::slice data)          { _impl->write(data); }
uint64_t  C4WriteStream::getBytesWritten() const noexcept   { return _impl->bytesWritten(); }
C4BlobKey C4WriteStream::computeBlobKey()                   { return _impl->computeKey(); }
C4BlobKey C4WriteStream::install(const C4BlobKey *xk)       { return _store.install(_impl.get(), xk); }

// clang-format on

#pragma mark - BLOB UTILITIES:

optional<C4BlobKey> C4Blob::keyFromDigestProperty(FLDict dict) {
    FLValue digest = FLDict_Get(dict, C4Blob::kDigestProperty);
    return C4BlobKey::withDigestString(FLValue_AsString(digest));
}

bool C4Blob::isBlob(FLDict dict) {
    FLValue cbltype = FLDict_Get(dict, C4Document::kObjectTypeProperty);
    return cbltype && slice(FLValue_AsString(cbltype)) == C4Blob::kObjectType_Blob;
}

bool C4Blob::isAttachmentIn(FLDict dict, FLDict inDocument) {
    FLDict attachments = FLValue_AsDict(FLDict_Get(inDocument, kLegacyAttachmentsProperty));
    for ( Dict::iterator i(attachments); i; ++i ) {
        if ( FLValue(i.value()) == FLValue(dict) ) return true;
    }
    return false;
}

bool C4Blob::dictContainsBlobs(FLDict dict) noexcept {
    bool found = false;
    C4Blob::findBlobReferences(dict, [&](FLDict) {
        found = true;
        return false;  // to stop search
    });
    return found;
}

bool C4Blob::findBlobReferences(FLDict dict, const FindBlobCallback& callback) {
    if ( dict ) {
        for ( DeepIterator i = Dict(dict); i; ++i ) {
            auto d = FLDict(i.value().asDict());
            if ( d && C4Blob::isBlob(d) ) {
                if ( !callback(d) ) return false;
                i.skipChildren();
            }
        }
    }
    return true;
}

bool C4Blob::findAttachmentReferences(FLDict docRoot, const FindBlobCallback& callback) {
    if ( auto atts = Dict(docRoot).get(C4Blob::kLegacyAttachmentsProperty).asDict(); atts ) {
        for ( Dict::iterator i(atts); i; ++i ) {
            if ( auto d = i.value().asDict(); d ) {
                if ( !callback(d) ) return false;
            }
        }
    }
    return true;
}

// Heuristics for deciding whether a MIME type is compressible or not.
// See <http://www.iana.org/assignments/media-types/media-types.xhtml>

// These substrings in a MIME type mean it's definitely not compressible:
static constexpr std::array<slice, 8> kCompressedTypeSubstrings = {
        {"zip"_sl, "zlib"_sl, "pkcs"_sl, "mpeg"_sl, "mp4"_sl, "crypt"_sl, ".rar"_sl, "-rar"_sl}};

// These substrings mean it is compressible (unused for now):
//static constexpr std::array<slice, 4> kGoodTypeSubstrings = {"json"_sl, "html"_sl, "xml"_sl, "yaml"_sl};

// These prefixes mean it's not compressible, *unless* it matches the above good-types list
// (like SVG (image/svg+xml), which is compressible.)
static constexpr std::array<slice, 3> kBadTypePrefixes = {{"image/"_sl, "audio/"_sl, "video/"_sl}};

template <size_t N>
static bool containsAnyOf(slice type, const std::array<slice, N> types) {
    return std::any_of(types.begin(), types.end(), [&type](slice t) { return type.find(t); });
}

template <size_t N>
static bool startsWithAnyOf(slice type, const std::array<slice, N> types) {
    return std::any_of(types.begin(), types.end(), [&type](slice t) { return type.hasPrefix(t); });
}

bool C4Blob::isLikelyCompressible(FLDict flMeta) {
    Dict meta(flMeta);
    // Don't compress an attachment with a compressed encoding:
    auto encodingProp = meta.get("encoding"_sl);
    if ( encodingProp && containsAnyOf(encodingProp.asString(), kCompressedTypeSubstrings) ) return false;

    // Don't compress attachments with unknown MIME type:
    slice type = meta.get("content_type"_sl).asString();
    if ( !type ) return false;

    // Convert type to canonical lowercase form:
    string lc = type.asString();
    toLowercase(lc);
    type = lc;

    // Check the MIME type:
    if ( containsAnyOf(type, kCompressedTypeSubstrings) || startsWithAnyOf(type, kBadTypePrefixes) ) return false;
    else
        return true;
}
