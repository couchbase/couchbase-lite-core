//
// Pusher+Attachments.cc
//
// Copyright 2020-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "Pusher.hh"
#include "DBAccess.hh"
#include "BLIP.hh"
#include "Increment.hh"
#include "c4BlobStore.hh"
#include "SecureDigest.hh"
#include "StringUtil.hh"

using namespace std;
using namespace litecore::blip;

namespace litecore::repl {

    class BlobDataSource : public IMessageDataSource {
      public:
        BlobDataSource(Pusher *pusher, unique_ptr<C4ReadStream> &&blob, const Replicator::BlobProgress &progress)
            : _pusher(pusher), _repl(pusher->replicator()), _blob(move(blob)), _progress(progress) {}

        int operator()(void *buf, size_t capacity) override {
            // Callback to read bytes from the blob into the BLIP message:
            // For performance reasons this is NOT run on my actor thread, so it can't access
            // my state directly; instead it calls _attachmentSent() at the end.
            bool    done      = false;
            ssize_t bytesRead = -1;
            try {
                bytesRead = _blob->read(buf, capacity);
                _progress.bytesCompleted += bytesRead;
            } catch ( ... ) {
                _progress.error = C4Error::fromCurrentException();
                _pusher->warn("Error reading from blob: %d/%d", _progress.error.domain, _progress.error.code);
                bytesRead = -1;
            }
            if ( bytesRead < capacity ) {
                _blob = nullptr;
                _pusher->enqueue(FUNCTION_TO_QUEUE(Pusher::_attachmentSent));
                done = true;
            }
            if ( _pusher->progressNotificationLevel() >= 2 ) {
                auto now = actor::Timer::clock::now();
                if ( done || now - _lastNotifyTime > 250ms ) {
                    _lastNotifyTime = now;
                    _repl->onBlobProgress(_progress);
                }
            }
            return (int)bytesRead;
        }

      private:
        Pusher                         *_pusher;
        Retained<Replicator>            _repl;
        unique_ptr<C4ReadStream>        _blob;
        Replicator::BlobProgress        _progress;
        actor::Timer::clock::time_point _lastNotifyTime = actor::Timer::clock::now();
    };

    // Reads the "digest" property from a BLIP message and opens a read stream on that blob.
    unique_ptr<C4ReadStream> Pusher::readBlobFromRequest(MessageIn *req, slice &digestStr,
                                                         Replicator::BlobProgress &progress) {
        try {
            digestStr = req->property("digest"_sl);
            progress  = {Dir::kPushing};
            if ( auto key = C4BlobKey::withDigestString(digestStr); key ) progress.key = *key;
            else
                C4Error::raise(LiteCoreDomain, kC4ErrorInvalidParameter, "Missing or invalid 'digest'");
            auto blobStore = _db->blobStore();
            if ( int64_t size = blobStore->getSize(progress.key); size >= 0 ) progress.bytesTotal = size;
            else
                C4Error::raise(LiteCoreDomain, kC4ErrorNotFound, "No such blob");
            return make_unique<C4ReadStream>(*blobStore, progress.key);
        } catch ( ... ) {
            req->respondWithError(c4ToBLIPError(C4Error::fromCurrentException()));
            return nullptr;
        }
    }

    // Incoming request to send an attachment/blob
    void Pusher::handleGetAttachment(Retained<MessageIn> req) {
        slice                    digest;
        Replicator::BlobProgress progress;
        unique_ptr<C4ReadStream> blob = readBlobFromRequest(req, digest, progress);
        if ( !blob ) return;

        increment(_blobsInFlight);
        MessageBuilder reply(req);
        reply.compressed = req->boolProperty("compress"_sl);
        logVerbose("Sending blob %.*s (length=%" PRId64 ", compress=%d)", SPLAT(digest), blob->getLength(),
                   reply.compressed);
        Retained<Replicator> repl = replicator();
        if ( progressNotificationLevel() >= 2 ) repl->onBlobProgress(progress);

        reply.dataSource = make_unique<BlobDataSource>(this, move(blob), progress);
        req->respond(reply);
    }

    void Pusher::_attachmentSent() { decrement(_blobsInFlight); }

    // Incoming request to prove I have an attachment that I'm pushing, without sending it:
    void Pusher::handleProveAttachment(Retained<MessageIn> request) {
        slice                    digest;
        Replicator::BlobProgress progress;
        unique_ptr<C4ReadStream> blob = readBlobFromRequest(request, digest, progress);
        if ( !blob ) return;

        logVerbose("Sending proof of attachment %.*s", SPLAT(digest));
        SHA1Builder sha;

        // First digest the length-prefixed nonce:
        slice nonce = request->body();
        if ( nonce.size == 0 || nonce.size > 255 ) {
            request->respondWithError({"BLIP"_sl, 400, "Missing nonce"_sl});
            return;
        }
        sha << (nonce.size & 0xFF) << nonce;

        // Now digest the attachment itself:
        static constexpr size_t kBufSize = 8192;
        auto                    buf      = make_unique<uint8_t[]>(kBufSize);
        size_t                  bytesRead;
        while ( (bytesRead = blob->read(buf.get(), kBufSize)) > 0 ) sha << slice(buf.get(), bytesRead);
        buf.reset();
        blob = nullptr;

        // Respond with the base64-encoded digest:
        C4BlobKey proofDigest;
        sha.finish(proofDigest.bytes, sizeof(proofDigest.bytes));
        string proofStr = proofDigest.digestString();

        MessageBuilder reply(request);
        reply.write(proofStr);
        request->respond(reply);
    }

}  // namespace litecore::repl
