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
#include "StringUtil.hh"
#include "MessageBuilder.hh"

using namespace fleece;
using namespace litecore::blip;

namespace litecore { namespace repl {

    IncomingBlob::IncomingBlob(Worker *parent, C4BlobStore *blobStore)
    :Worker(parent, "blob")
    ,_blobStore(blobStore)
    { }


    void IncomingBlob::_start(C4BlobKey key, uint64_t size, bool compress) {
        _key = key;
        _size = size;
        alloc_slice digest = c4blob_keyToString(_key);
        logVerbose("Requesting blob %.*s (%llu bytes, compress=%d)",
                   SPLAT(digest), _size, compress);
        C4Error err;
        _writer = c4blob_openWriteStream(_blobStore, &err);
        if (!_writer)
            return gotError(err);

        addProgress({0, _size});

        MessageBuilder req("getAttachment"_sl);
        req["digest"_sl] = digest;
        if (compress)
            req["compress"_sl] = "true"_sl;
        sendRequest(req, asynchronize([=](blip::MessageProgress progress) {
            //... After request is sent:
            if (_writer) {
                if (progress.state == MessageProgress::kDisconnected) {
                    closeWriter();
                } else if (progress.reply) {
                    if (progress.reply->isError()) {
                        gotError(progress.reply);
                    } else {
                        writeToBlob(progress.reply->extractBody());
                        if (progress.state == MessageProgress::kComplete)
                            finishBlob();
                    }
                }
            }
        }));
    }


    void IncomingBlob::writeToBlob(alloc_slice data) {
        C4Error err;
        if (!c4stream_write(_writer, data.buf, data.size, &err))
            return gotError(err);
        addProgress({data.size, 0});
    }


    void IncomingBlob::finishBlob() {
        alloc_slice digest = c4blob_keyToString(_key);
        logVerbose("Finished receiving blob %.*s (%llu bytes)", SPLAT(digest), _size);
        C4Error err;
        if (!c4stream_install(_writer, &_key, &err))
            gotError(err);
        closeWriter();
    }


    void IncomingBlob::closeWriter() {
        c4stream_closeWriter(_writer);
        _writer = nullptr;
    }


    void IncomingBlob::onError(C4Error err) {
        closeWriter();
        Worker::onError(err);
        // Bump progress to 100% so as not to mess up overall progress tracking:
        setProgress({_size, _size});
    }


    Worker::ActivityLevel IncomingBlob::computeActivityLevel() const {
        if (Worker::computeActivityLevel() == kC4Busy || _writer != nullptr)
            return kC4Busy;
        else
            return kC4Stopped;
    }

} }
