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

    class CrossProcessNotifierData;

    /** Implements a simple change notification system that works between any processes on the
        same computer. Any process can post a notification, which will be received by all the
        others (but not itself.)

        This is implemented with a small file that's mapped as shared memory. Notifications are
        scoped to all processes that have opened a CrossProcessNotifier on the same file. */
    class CrossProcessNotifier final : public RefCounted, Logging {
    public:
        using Callback = std::function<void()>;

        CrossProcessNotifier()                          :Logging(DBLog) { }

        /// Starts the notifier.
        /// @param path  Path where the shared-memory file should be created.
        /// @param callback  The function to be called when another process notifies.
        /// @param outError  On failure, the error will be stored here if non-null.
        /// @return  True on success, false on failure.
        bool start(const std::string &path,
                   Callback callback,
                   C4Error *outError);

        /// Posts a notification to other processes. Does not trigger a callback in this process.
        /// Has no effect if the notifier is not started or failed to start.
        void notify() const;

        /// Stops the notifier. The background task may take a moment to clean up, but no more
        /// notifications will be delivered after this method returns.
        /// @warning Notifiers cannot be restarted after stopping. Create a new instance instead.
        void stop();

    protected:
        ~CrossProcessNotifier();
        std::string loggingIdentifier() const override  {return _path;}

    private:
        void observerThread(Retained<CrossProcessNotifier>);
        void teardown();

        std::string _path;                  // Path of the file
        Callback    _callback;              // Client callback to invoke
        int         _myPID;                 // This process's pid
        CrossProcessNotifierData* _sharedData {nullptr};  // Points to the shared memory in the file
        bool        _running {false};       // True when started, set to false by `stop`
    };

}
