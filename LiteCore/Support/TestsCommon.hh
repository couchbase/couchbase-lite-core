//
// TestsCommon.hh
//
// Copyright © 2021 Couchbase. All rights reserved.
//

#pragma once

namespace litecore {
    class FilePath;
}

/** Returns the OS's temporary directory (/tmp on Unix-like systems) */
litecore::FilePath GetSystemTempDirectory();

/** Returns a temporary directory for use by this test run. */
litecore::FilePath GetTempDirectory();

/** Initializes logging for tests, both binary and console. */
void InitTestLogging();

