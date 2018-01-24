//
//  PlatformIO.hh
//  LiteCore
//
//  Created by Jim Borden on 11/28/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once
#include "PlatformCompat.hh"


#ifdef _MSC_VER

    #include <cstdio>
    #include "asprintf.h"

    #define fdopen      ::_fdopen
    #define fseeko      fseek
    #define ftello      ftell
    #define strncasecmp _strnicmp
    #define strcasecmp  _stricmp
    #define fdclose ::_close

    struct stat;

    namespace litecore {
        int mkdir_u8(const char* const path, int mode);
        int stat_u8(const char* const filename, struct stat* const s);
        int rmdir_u8(const char* const path);
        int rename_u8(const char* const oldPath, const char* const newPath);
        int unlink_u8(const char* const filename);
        int chmod_u8(const char* const filename, int mode);
        FILE* fopen_u8(const char* const path, const char* const mode);
    }

#else

    #include <stdio.h>
    #include <sys/stat.h>
    #include <unistd.h>

    #define fdclose ::close

    namespace litecore {
        inline int mkdir_u8(const char* const path NONNULL , int mode) {
            return ::mkdir(path, (mode_t)mode);
        }

        inline int stat_u8(const char* const filename NONNULL, struct ::stat* const s) {
            return ::stat(filename, s);
        }

        inline int rmdir_u8(const char* const path NONNULL) {
            return ::rmdir(path);
        }

        inline int rename_u8(const char* const oldPath NONNULL, const char* const newPath NONNULL) {
            return ::rename(oldPath, newPath);
        }

        inline int unlink_u8(const char* const filename NONNULL) {
            return ::unlink(filename);
        }

        inline int chmod_u8(const char* const filename NONNULL, int mode) {
            return ::chmod(filename, (mode_t)mode);
        }

        inline FILE* fopen_u8(const char* const path NONNULL, const char* const mode NONNULL) {
            return ::fopen(path, mode);
        }
    }

#endif
