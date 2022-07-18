//
// IncomingBlob.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "IncomingRev.hh"
#include "Replicator.hh"
#include "DBAccess.hh"
#include "StringUtil.hh"
#include "MessageBuilder.hh"
#include "c4BlobStore.hh"
#include <atomic>

using namespace fleece;
using namespace litecore::blip;

namespace litecore { namespace repl {

#if DEBUG
    static std::atomic_int sNumOpenWriters {0};
    static std::atomic_int sMaxOpenWriters {0};
#endif

    // Looks for another blob to download; when they're all done, finishes up the revision.
    void IncomingRev::fetchNextBlob() {
        while (_blob != _pendingBlobs.end()) {
            if (startBlob())
                return;
            ++_blob;
        }

        // All blobs completed, now finish:
        if (_rev->error.code == 0) {
            logVerbose("All blobs received, now inserting revision");
            insertRevision();
        } else {
            finish();
        }
    }


    // If the blob described by `_blob` exists locally, returns false.
    // Else sends a request for its data.
    bool IncomingRev::startBlob() {
        Assert(!_writer);
        if (_db->blobStore()->getSize(_blob->key) >= 0)
            return false;  // already have it

        logVerbose("Requesting blob (%" PRIu64 " bytes, compress=%d)", _blob->length, _blob->compressible);

        addProgress({0, _blob->length});
        _blobBytesWritten = 0;

        MessageBuilder req("getAttachment"_sl);
        
        if (_options->collectionAware) {
            // TODO: Use Worker::kCollectionProperty
            req["collection"_sl] = collectionIndex();
        }
        
        req["digest"_sl] = _blob->key.digestString();
        req["docID"] = _blob->docID;
        if (_blob->compressible)
            req["compress"_sl] = "true"_sl;
        sendRequest(req, [=](blip::MessageProgress progress) {
            //... After request is sent:
            if (_blob != _pendingBlobs.end()) {
                if (progress.state == MessageProgress::kDisconnected) {
                    // Set some error, so my IncomingRev will know I didn't complete [CBL-608]
                    blobGotError({POSIXDomain, ECONNRESET});
                } else if (progress.reply) {
                    if (progress.reply->isError()) {
                        auto err = progress.reply->getError();
                        logError("Got error response: %.*s %d '%.*s'",
                                 SPLAT(err.domain), err.code, SPLAT(err.message));
                        blobGotError(blipToC4Error(err));
                    } else {
                        bool complete = progress.state == MessageProgress::kComplete;
                        auto data = progress.reply->extractBody();
                        writeToBlob(data);
                        if (complete || data.size > 0)
                            notifyBlobProgress(complete);
                        if (complete)
                            finishBlob();
                    }
                }
            }
        });
        return true;
    }


    // Writes data to the blob on disk.
    void IncomingRev::writeToBlob(alloc_slice data) {
        try {
            if(_writer == nullptr) {
                _writer = make_unique<C4WriteStream>(*_db->blobStore());
    #if DEBUG
                int n = ++sNumOpenWriters;
                if (n > sMaxOpenWriters) {
                    sMaxOpenWriters = n;
                    logInfo("There are now %d blob writers open", n);
                }
                logVerbose("Opened blob writer  [%d open; max %d]", n, (int)sMaxOpenWriters);
    #endif
            }
            if (data.size > 0) {
                _writer->write(data);
                _blobBytesWritten += data.size;
                addProgress({data.size, 0});
            }
        } catch(...) {
            blobGotError(C4Error::fromCurrentException());
        }
    }


    // Saves the blob to the database, and starts working on the next one (if any).
    void IncomingRev::finishBlob() {
        logVerbose("Finished receiving blob %s (%" PRIu64 " bytes)",
                   _blob->key.digestString().c_str(), _blob->length);
        try {
            _writer->install(&_blob->key);
        } catch(...) {
            blobGotError(C4Error::fromCurrentException());
            return;
        }
        closeBlobWriter();

        ++_blob;
        fetchNextBlob();
    }


    void IncomingRev::blobGotError(C4Error err) {
        closeBlobWriter();
        // Bump bytes-completed to end so as not to mess up overall progress:
        addProgress({_blob->length - _blobBytesWritten, 0});
        failWithError(err);
    }


    // Sends periodic notifications to the Replicator if desired.
    void IncomingRev::notifyBlobProgress(bool always) {
        if (progressNotificationLevel() < 2)
            return;
        auto now = actor::Timer::clock::now();
        if (always || now - _lastNotifyTime > 250ms) {
            _lastNotifyTime = now;
            Replicator::BlobProgress prog {
                Dir::kPulling,
                nullslice,     // TODO: Collection support
                _blob->docID, _blob->docProperty,
                _blob->key,
                status().progress.unitsCompleted,
                status().progress.unitsTotal};
            logVerbose("blob progress: %" PRIu64 " / %" PRIu64, prog.bytesCompleted, prog.bytesTotal);
            replicator()->onBlobProgress(prog);
        }
    }


    void IncomingRev::closeBlobWriter() {
#if DEBUG
        if (_writer) {
            int n = --sNumOpenWriters;
            logVerbose("Closed blob writer  [%d open]", n);
        }
#endif
        _writer = nullptr;
    }

} }
