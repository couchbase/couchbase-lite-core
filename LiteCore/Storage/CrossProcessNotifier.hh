//
// CrossProcessNotifier.hh
//
// Copyright © 2021 Couchbase. All rights reserved.
//

#pragma once
#include "Base.hh"
#include "Logging.hh"
#include <functional>

struct C4Error;

namespace litecore {
    class FilePath;

    /** Implements a simple change notification system that works between any processes that have
        opened the same database file. */
    class CrossProcessNotifier final : public RefCounted, Logging {
    public:
        using Callback = std::function<void()>;

        CrossProcessNotifier();

        bool start(const FilePath &databaseDir,
                   Callback callback,
                   C4Error *outError);
        void stop();

        void notify() const;

    protected:
        ~CrossProcessNotifier();
        std::string loggingIdentifier() const override;

    private:
        struct SharedData;
        static constexpr const char* kSharedMemFilename = "cblite_mem";

        void observe(Retained<CrossProcessNotifier>);
        void teardown();
        int _check(const char *fn, int result) const;

        std::string _path;
        Callback    _callback;
        int         _myPID;
        SharedData* _sharedData {nullptr};
        bool        _running {false};
    };

}
