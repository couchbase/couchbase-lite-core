//
//  Checkpoint.hh
//  LiteCore
//
//  Created by Jens Alfke on 3/1/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Timer.hh"
#include "c4.h"
#include "slice.hh"
#include <chrono>
#include <mutex>

namespace litecore { namespace repl {

    class Checkpoint {
    public:
        Checkpoint()                                { }
        
        // localSeq property is thread-safe
        C4SequenceNumber localSeq() const           {return _localSeq;}
        void setLocalSeq(C4SequenceNumber s)        {set(&s, nullptr);}

        // remoteSeq property is thread-safe
        fleece::alloc_slice remoteSeq() const;
        void setRemoteSeq(fleece::slice s)          {set(nullptr, &s);}

        void decodeFrom(fleece::slice json);
        fleece::alloc_slice encode() const;

        bool validateWith(const Checkpoint&);

        using SaveCallback = std::function<void(fleece::alloc_slice jsonToSave)>;

        void autosave(std::chrono::milliseconds saveTime, SaveCallback cb);
        void stopAutosave();
        void save();

    private:
        void set(const C4SequenceNumber *local, const fleece::slice *remote);

        C4SequenceNumber _localSeq {0};
        fleece::alloc_slice _remoteSeq;
        std::mutex _mutex;

        std::unique_ptr<Timer> _timer;
        bool _changed  {false};
        SaveCallback _saveCallback;
        std::chrono::milliseconds _saveTime;
    };

} }
