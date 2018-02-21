//
// mkstemp.cc
//
// Copyright (c) 2018 Couchbase, Inc All rights reserved.
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


#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <io.h>
#include <atlbase.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#ifndef HAVE_MKSTEMP

static char* mktemp_internal(char* templ)
{
    srand(time(nullptr));
    char* start = strchr(templ, '\0');
    while(*(--start) == 'X') {
        int r = rand();
        while(r < 0 || r > 61) {
            r = rand();
        }

        if(r < 26) {
            *start = r + 'a';
        } else if(r < 52) {
            r -= 26;
            *start = r + 'A';
        } else {
            r -= 52;
            *start = r + '0';
        }
    }

    return templ;
}

int mkstemp(char *tmp)
{
    mktemp_internal(tmp);
    CA2WEX<256> wpath(tmp, CP_UTF8);
	struct _stat64i32 buf;
    if(_wstat64i32(wpath, &buf) == 0) {
        errno = EEXIST;
        return -1;
    }

    int fd = _wopen(wpath, O_RDWR | O_CREAT | O_EXCL | O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd >= 0 || errno != EEXIST) {
        return fd;
    }

    errno = EEXIST;
    return -1;
}

#endif