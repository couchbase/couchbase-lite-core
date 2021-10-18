//
// SQLiteTempDirectory.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "PlatformCompat.hh"

#if defined(_MSC_VER) && !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#include <sqlite3.h>
#include <wtypes.h>
#include <stdlib.h>

extern "C" void setSqliteTempDirectory() {
    LPCWSTR zPath = Windows::Storage::ApplicationData::Current->
        TemporaryFolder->Path->Data();
    char* zPathBuf = (char *)malloc(32767 * 4 + 1);
    memset(zPathBuf, 0, sizeof(zPathBuf));
    WideCharToMultiByte(CP_UTF8, 0, zPath, -1, zPathBuf, 32767 * 4 + 1,
        NULL, NULL);
    sqlite3_temp_directory = sqlite3_mprintf("%s", zPathBuf);
    free(zPathBuf);
}

#endif