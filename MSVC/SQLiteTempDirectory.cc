//
// SQLiteTempDirectory.cc
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

#include "PlatformCompat.hh"

#if defined(_MSC_VER) && !WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
#include <sqlite3.h>
#include <wtypes.h>

extern "C" void setSqliteTempDirectory() {
    LPCWSTR zPath = Windows::Storage::ApplicationData::Current->
        TemporaryFolder->Path->Data();
    char zPathBuf[MAX_PATH + 1];
    memset(zPathBuf, 0, sizeof(zPathBuf));
    WideCharToMultiByte(CP_UTF8, 0, zPath, -1, zPathBuf, MAX_PATH + 1,
        NULL, NULL);
    sqlite3_temp_directory = sqlite3_mprintf("%s", zPathBuf);
}

#endif