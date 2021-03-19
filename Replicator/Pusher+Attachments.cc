//
// Pusher+Attachments.cc
//
// Copyright © 2020 Couchbase. All rights reserved.
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

#include "Pusher.hh"
#include "DBAccess.hh"
#include "BLIP.hh"
#include "Increment.hh"
#include "c4BlobStore.h"
#include "SecureDigest.hh"
#include "StringUtil.hh"

using namespace std;
using namespace litecore::blip;

namespace litecore::repl {

    // Reads the "digest" property from a BLIP message and opens a read stream on that blob.
    C4ReadStream* Pusher::readBlobFromRequest(MessageIn *req,
                                              slice &digestStr,
                                              Replicator::BlobProgress &progress,
                                              C4Error *outError)
    {
        auto blobStore = _db->blobStore();
        digestStr = req->property("digest"_sl);
        progress = {Dir::kPushing};
        if (!c4blob_keyFromString(digestStr, &progress.key)) {
            c4error_return(LiteCoreDomain, kC4ErrorInvalidParameter, "Missing or invalid 'digest'"_sl, outError);
            return nullptr;
        }
        int64_t size = c4blob_getSize(blobStore, progress.key);
        if (size < 0) {
            c4error_return(LiteCoreDomain, kC4ErrorNotFound, "No such blob"_sl, outError);
            return nullptr;
        }
        progress.bytesTotal = size;
        return c4blob_openReadStream(blobStore, progress.key, outError);
    }


    // Incoming request to send an attachment/blob
    void Pusher::handleGetAttachment(Retained<MessageIn> req) {
        slice digest;
        Replicator::BlobProgress progress;
        C4Error err;
        C4ReadStream* blob = readBlobFromRequest(req, digest, progress, &err);
        if (!blob) {
            req->respondWithError(c4ToBLIPError(err));
            return;
        }

        increment(_blobsInFlight);
        MessageBuilder reply(req);
        reply.compressed = req->boolProperty("compress"_sl);
        logVerbose("Sending blob %.*s (length=%" PRId64 ", compress=%d)",
                   SPLAT(digest), c4stream_getLength(blob, nullptr), reply.compressed);
        Retained<Replicator> repl = replicator();
        auto lastNotifyTime = actor::Timer::clock::now();
        if (progressNotificationLevel() >= 2)
            repl->onBlobProgress(progress);
        reply.dataSource = [=](void *buf, size_t capacity) mutable {
            // Callback to read bytes from the blob into the BLIP message:
            // For performance reasons this is NOT run on my actor thread, so it can't access
            // my state directly; instead it calls _attachmentSent() at the end.
            C4Error err;
            bool done = false;
            ssize_t bytesRead = c4stream_read(blob, buf, capacity, &err);
            progress.bytesCompleted += bytesRead;
            if (bytesRead < capacity) {
                c4stream_close(blob);
                this->enqueue(FUNCTION_TO_QUEUE(Pusher::_attachmentSent));
                done = true;
            }
            if (err.code) {
                this->warn("Error reading from blob: %d/%d", err.domain, err.code);
                progress.error = {err.domain, err.code};
                bytesRead = -1;
                done = true;
            }
            if (progressNotificationLevel() >= 2) {
                auto now = actor::Timer::clock::now();
                if (done || now - lastNotifyTime > 250ms) {
                    lastNotifyTime = now;
                    repl->onBlobProgress(progress);
                }
            }
            return (int)bytesRead;
        };
        req->respond(reply);
    }


    void Pusher::_attachmentSent() {
        decrement(_blobsInFlight);
    }


    // Incoming request to prove I have an attachment that I'm pushing, without sending it:
    void Pusher::handleProveAttachment(Retained<MessageIn> request) {
        slice digest;
        Replicator::BlobProgress progress;
        C4Error err;
        c4::ref<C4ReadStream> blob = readBlobFromRequest(request, digest, progress, &err);
        if (blob) {
            logVerbose("Sending proof of attachment %.*s", SPLAT(digest));
            SHA1Builder sha;

            // First digest the length-prefixed nonce:
            slice nonce = request->body();
            if (nonce.size == 0 || nonce.size > 255) {
                request->respondWithError({"BLIP"_sl, 400, "Missing nonce"_sl});
                return;
            }
            sha << (nonce.size & 0xFF) << nonce;

            // Now digest the attachment itself:
            static constexpr size_t kBufSize = 8192;
            auto buf = make_unique<uint8_t[]>(kBufSize);
            size_t bytesRead;
            while ((bytesRead = c4stream_read(blob, buf.get(), kBufSize, &err)) > 0)
                sha << slice(buf.get(), bytesRead);
            buf.reset();
            blob = nullptr;

            if (err.code == 0) {
                // Respond with the base64-encoded digest:
                C4BlobKey proofDigest;
                sha.finish(proofDigest.bytes, sizeof(proofDigest.bytes));
                alloc_slice proofStr = c4blob_keyToString(proofDigest);

                MessageBuilder reply(request);
                reply.write(proofStr);
                request->respond(reply);
                return;
            }
        }

        // If we got here, we failed:
        request->respondWithError(c4ToBLIPError(err));
    }

}
