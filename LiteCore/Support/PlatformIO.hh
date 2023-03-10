//
// PlatformIO.hh
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "fleece/PlatformCompat.hh"


#ifdef _MSC_VER

#    include <cstdio>
#    include <io.h>
#    include "asprintf.h"

#    define fdopen      ::_fdopen
#    define fseeko      fseek
#    define ftello      ftell
#    define strncasecmp _strnicmp
#    define strcasecmp  _stricmp
#    define fdclose     ::_close

#    define R_OK 1
#    define W_OK 2
#    define X_OK 4

struct _stat64;

namespace litecore {
    int   mkdir_u8(const char* const path, int mode);
    int   stat_u8(const char* const filename, struct _stat64* const s);
    int   rmdir_u8(const char* const path);
    int   rename_u8(const char* const oldPath, const char* const newPath);
    int   unlink_u8(const char* const filename);
    int   chmod_u8(const char* const filename, int mode);
    int   access_u8(const char* const path, int mode);
    FILE* fopen_u8(const char* const path, const char* const mode);
}  // namespace litecore

#else

#    include <stdio.h>
#    include <sys/stat.h>
#    include <unistd.h>

#    define fdclose ::close

namespace litecore {
    inline int mkdir_u8(const char* const path NONNULL, int mode) { return ::mkdir(path, (mode_t)mode); }

    inline int stat_u8(const char* const filename NONNULL, struct ::stat* const s) { return ::stat(filename, s); }

    inline int rmdir_u8(const char* const path NONNULL) { return ::rmdir(path); }

    inline int rename_u8(const char* const oldPath NONNULL, const char* const newPath NONNULL) {
        return ::rename(oldPath, newPath);
    }

    inline int unlink_u8(const char* const filename NONNULL) { return ::unlink(filename); }

    inline int chmod_u8(const char* const filename NONNULL, int mode) { return ::chmod(filename, (mode_t)mode); }

    inline int access_u8(const char* const path NONNULL, int mode) { return ::access(path, mode); }

    inline FILE* fopen_u8(const char* const path NONNULL, const char* const mode NONNULL) {
        return ::fopen(path, mode);
    }
}  // namespace litecore

#endif
