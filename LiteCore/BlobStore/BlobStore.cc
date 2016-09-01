//
//  BlobStore.cpp
//  LiteCore
//
//  Created by Jens Alfke on 8/31/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "BlobStore.hh"
#include "FilePath.hh"
#include "Error.hh"
#include "Writer.hh"
#include "SecureDigest.hh"
#include <libb64/encode.h>
#include <libb64/decode.h>
#include <stdio.h>


namespace litecore {
    using namespace std;


#pragma mark - BLOBKEY:


    static const size_t kBlobKeyStringLength = 5 + ((sizeof(blobKey::bytes) + 2) / 3) * 4;


    blobKey::blobKey(slice s) {
        if (s.size != sizeof(bytes))
            error::_throw(error::WrongFormat);
        memcpy(bytes, s.buf, sizeof(bytes));
    }

    blobKey::blobKey(const string &str) {
        const char *data = str.data();
        if (str.size() == kBlobKeyStringLength && 0 == memcmp(data, "sha1-", 5)) {
            base64::decoder dec;
            uint8_t buf[33];    // have to use intermediate buf because decoder writes to 33rd byte :(
            auto decoded = dec.decode(&data[5], str.size() - 5, buf);
            CBFDebugAssert(decoded <= sizeof(bytes));
            if (decoded == sizeof(bytes)) {
                memcpy(bytes, buf, sizeof(bytes));
                return;
            }
        }
        error::_throw(error::WrongFormat);
    }


    string blobKey::base64String() const {
        // Result is "sha1-" plus base64-encoded key bytes:
        string result = "sha1-";
        result.resize(kBlobKeyStringLength);
        char *dst = &result[5];
        base64::encoder enc;
        enc.set_chars_per_line(0);
        size_t written = enc.encode(bytes, sizeof(bytes), dst);
        written += enc.encode_end(dst + written);
        CBFAssert(written == kBlobKeyStringLength - 5);
        return result;
}


    /*static*/ blobKey blobKey::computeFrom(slice data) {
#if SECURE_DIGEST_AVAILABLE
        blobKey key;
        sha1Context ctx;
        sha1_begin(&ctx);
        sha1_add(&ctx, data.buf, data.size);
        sha1_end(&ctx, &key.bytes);
        return key;
#else
        error::_throw(error::Unimplemented);
#endif
    }


#pragma mark - BLOB:
    
    
    Blob::Blob(const BlobStore &store, const blobKey &key)
    :_path(store.dir(), key.hexString() + ".blob"),
     _key(key)
    { }


    bool Blob::exists() const {
        return path().exists();
    }
    

    alloc_slice Blob::contents() const {
        fleece::Writer out;
        char buf[32768];
        auto in = read();
        size_t n;
        do {
            n = fread(buf, 1, sizeof(buf), in);
            if (n > 0)
                out.write(buf, n);
        } while (n == sizeof(buf));
        int err = ferror(in);
        fclose(in);
        if (err)
            error::_throw(error::POSIX, err);
        return out.extractOutput();
    }


    FILE* Blob::read() const {
        FILE *in = fopen(path().path().c_str(), "r");
        if (!in)
            error::_throwErrno();
        return in;
    }


#pragma mark - BLOBSTORE:


    const BlobStore::Options BlobStore::Options::defaults = {true, true};


    BlobStore::BlobStore(const FilePath &dir, const Options *options)
    :_dir(dir),
     _options(options ? *options : Options::defaults)
    {
        if (_options.encryptionAlgorithm != kNoEncryption)
            error::_throw(error::UnsupportedEncryption);        //TODO: Implement encryption

        if (_dir.exists()) {
            _dir.mustExistAsDir();
        } else {
            if (!_options.create)
                error::_throw(error::NotFound);
            _dir.mkdir();
        }
    }


    Blob BlobStore::put(slice data) {
        auto key = blobKey::computeFrom(data);
        Blob blob(*this, key);
        if (blob.exists())
            return blob;
        FILE *out;
        FilePath tmp = _dir["incoming_"].mkTempFile("tmp", &out);
        try {
            auto n = fwrite(data.buf, 1, data.size, out);
            int err = ferror(out);
            fclose(out);
            if (n < data.size)
                error::_throw(error::POSIX, err);
            tmp.moveTo(blob.path());
            return blob;
        } catch (...) {
            tmp.del();
            throw;
        }
    }

}
