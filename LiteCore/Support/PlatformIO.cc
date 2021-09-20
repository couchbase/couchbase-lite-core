//
// PlatformIO.cc
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
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

    int stat_u8(const char* const filename, struct _stat64 * const s)
    {
        MIGRATE_ARG(filename, ::_wstat64(wfilename, s));
    }

    int MIGRATE_1(rmdir_u8, ::_wrmdir)

    int MIGRATE_2S(rename_u8, ::_wrename)

    int MIGRATE_1(unlink_u8, ::_wunlink)

    int MIGRATE_2(chmod_u8, ::_wchmod, int)

    int MIGRATE_2(access_u8, ::_waccess, int);

    FILE* MIGRATE_2S(fopen_u8, ::_wfopen)

}

#endif
