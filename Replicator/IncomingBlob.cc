//
// IncomingBlob.cc
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

#include "IncomingBlob.hh"
#include "Replicator.hh"
#include "StringUtil.hh"
#include "MessageBuilder.hh"
#include "c4BlobStore.h"
#include <atomic>

using namespace fleece;
using namespace litecore::blip;

namespace litecore { namespace repl {

#if DEBUG
    static std::atomic_int sNumOpenWriters {0};
    static std::atomic_int sMaxOpenWriters {0};
#endif

    IncomingBlob::IncomingBlob(Worker *parent, C4BlobStore *blobStore)
    :Worker(parent, "blob")
    ,_blobStore(blobStore)
    {
        _passive = _options.pull <= kC4Passive;
    }


    std::string IncomingBlob::loggingIdentifier() const {
        alloc_slice digest = c4blob_keyToString(_blob.key);
        return format("for doc '%.*s'%.*s [%.*s]",
                      SPLAT(_blob.docID), SPLAT(_blob.docProperty), SPLAT(digest));
    }
    

    void IncomingBlob::_start(PendingBlob blob) {
        Assert(!_writer);
        _blob = blob;
        logVerbose("Requesting blob (%" PRIu64 " bytes, compress=%d)", _blob.length, _blob.compressible);

        addProgress({0, _blob.length});

        MessageBuilder req("getAttachment"_sl);
        alloc_slice digest = c4blob_keyToString(_blob.key);
        req["digest"_sl] = digest;
        if (_blob.compressible)
            req["compress"_sl] = "true"_sl;
        sendRequest(req, [=](blip::MessageProgress progress) {
            //... After request is sent:
            if (_busy) {
                if (progress.state == MessageProgress::kDisconnected) {
                    // Set some error, so my IncomingRev will know I didn't complete [CBL-608]
                    onError({POSIXDomain, ECONNRESET});
                } else if (progress.reply) {
                    if (progress.reply->isError()) {
                        gotError(progress.reply);
                        notifyProgress(true);
                    } else {
                        bool complete = progress.state == MessageProgress::kComplete;
                        auto data = progress.reply->extractBody();
                        writeToBlob(data);
                        if (complete)
                            finishBlob();
                        if (complete || data.size > 0)
                            notifyProgress(complete);
                    }
                }
            }
        });
        _busy = true;
    }


    void IncomingBlob::writeToBlob(alloc_slice data) {
        C4Error err;
		if(_writer == nullptr) {
            _writer = c4blob_openWriteStream(_blobStore, &err);
            if (!_writer)
                return gotError(err);
#if DEBUG
            int n = ++sNumOpenWriters;
            if (n > sMaxOpenWriters) {
                sMaxOpenWriters = n;
                logInfo("There are now %d blob writers open", n);
            }
            logVerbose("Opened writer  [%d open; max %d]", n, (int)sMaxOpenWriters);
#endif
		}
        if (data.size > 0) {
            if (!c4stream_write(_writer, data.buf, data.size, &err))
                return gotError(err);
            addProgress({data.size, 0});
        }
    }


    void IncomingBlob::finishBlob() {
        alloc_slice digest = c4blob_keyToString(_blob.key);
        logVerbose("Finished receiving blob %.*s (%" PRIu64 " bytes)", SPLAT(digest), _blob.length);
        C4Error err;
        if (!c4stream_install(_writer, &_blob.key, &err))
            gotError(err);
        closeWriter();
    }


    void IncomingBlob::notifyProgress(bool always) {
        if (progressNotificationLevel() < 2)
            return;
        auto now = actor::Timer::clock::now();
        if (always || now - _lastNotifyTime > std::chrono::milliseconds(250)) {
            _lastNotifyTime = now;
            Replicator::BlobProgress prog {
                Dir::kPulling,
                _blob.docID, _blob.docProperty,
                _blob.key,
                status().progress.unitsCompleted,
                status().progress.unitsTotal};
            logVerbose("progress: %" PRIu64 " / %" PRIu64, prog.bytesCompleted, prog.bytesTotal);
            replicator()->onBlobProgress(prog);
        }
    }


    void IncomingBlob::closeWriter() {
        _writer = nullptr;
        _busy = false;
#if DEBUG
        int n = --sNumOpenWriters;
        logVerbose("Closing;  [%d open]", n);
#endif
    }


    void IncomingBlob::onError(C4Error err) {
        closeWriter();
        Worker::onError(err);
        // Bump progress to 100% so as not to mess up overall progress tracking:
        setProgress({_blob.length, _blob.length});
    }


    Worker::ActivityLevel IncomingBlob::computeActivityLevel() const {
        if (Worker::computeActivityLevel() == kC4Busy || _busy)
            return kC4Busy;
        else
            return kC4Idle;
    }

} }
