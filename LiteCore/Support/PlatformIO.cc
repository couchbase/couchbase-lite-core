//
// PlatformIO.cc
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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

#ifdef _MSC_VER

#include "PlatformIO.hh"
#include <direct.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <atlbase.h>
#include <atlconv.h>


namespace litecore {

    #define MIGRATE_1(from, to) from(const char* const arg1) { \
        CA2WEX<256> wide(arg1, CP_UTF8); \
        return to(wide); \
    }

    #define MIGRATE_2(from, to, type1) from(const char* const arg1, type1 arg2) { \
        CA2WEX<256> wide(arg1, CP_UTF8); \
        return to(wide, arg2); \
    }

    #define MIGRATE_2S(from, to) from(const char* const arg1, const char* const arg2) { \
        CA2WEX<256> wide1(arg1, CP_UTF8); \
        CA2WEX<256> wide2(arg2, CP_UTF8); \
        return to(wide1, wide2); \
    }

    #define MIGRATE_ARG(arg, retVal) CA2WEX<256> w##arg(arg, CP_UTF8); return retVal


    int mkdir_u8(const char *path, int mode) {
        MIGRATE_ARG(path, ::_wmkdir(wpath));
    }

    int stat_u8(const char* const filename, struct stat * const s)
    {
        MIGRATE_ARG(filename, ::_wstat64i32(wfilename, (struct _stat64i32 *)s));
    }

    int MIGRATE_1(rmdir_u8, ::_wrmdir)

    int MIGRATE_2S(rename_u8, ::_wrename)

    int MIGRATE_1(unlink_u8, ::_wunlink)

    int MIGRATE_2(chmod_u8, ::_wchmod, int)

    FILE* MIGRATE_2S(fopen_u8, ::_wfopen)

}

#endif
