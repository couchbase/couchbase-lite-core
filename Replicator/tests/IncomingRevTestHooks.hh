//
// IncomingRevTestHooks.hh
//
// Copyright 2026-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt. As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#ifdef LITECORE_CPPTEST
#    include <functional>
#    include <memory>

namespace litecore::repl {
    class IncomingRev;

    namespace testing {
        enum class IncomingRevEvent {
            BeforeAfterEvent,
            AfterAfterEvent,
            BeforeInsertNotification,
            AfterInsertNotification,
            BeforeRevWasHandled
        };
        using IncomingRevHook = std::function<void(IncomingRev*, IncomingRevEvent)>;

        // A callback keeps its captured state alive even if the test unregisters it during cleanup.
        inline std::shared_ptr<const IncomingRevHook> incomingRevHook;

        inline void notifyIncomingRev(IncomingRev* rev, IncomingRevEvent event) {
            if ( auto hook = std::atomic_load(&incomingRevHook) ) (*hook)(rev, event);
        }
    }  // namespace testing
}  // namespace litecore::repl
#endif
