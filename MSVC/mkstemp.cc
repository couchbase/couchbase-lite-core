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
#include <fcntl.h>
#include <sys/stat.h>
#include <atlbase.h>
#include "TempArray.hh"
#include "arc4random.h"
#include <Error.hh>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#ifndef HAVE_MKSTEMP

const char letter_choices[63] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";

static char* mktemp_internal(char* templ)
{
    char* start = strchr(templ, '\0');
    while(*(--start) == 'X') {
	    const uint32_t r = arc4random_uniform(62);
		Assert(r < 62);
		*start = letter_choices[r];
    }

    return templ;
}

int mkstemp(char *tmp)
{
	const size_t len = strnlen_s(tmp, MAX_PATH + 1) + 1;
	TempArray(cp, char, len);
	strcpy_s(cp, len, tmp);
    
	struct _stat64i32 buf{};
	for(int i = 0; i < INT32_MAX; i++) {
		mktemp_internal(tmp);
		const CA2WEX<256> wpath(tmp, CP_UTF8);
		// O_EXCL means trying to open an existing file is an error
		const int fd = _wopen(wpath, O_RDWR | O_CREAT | O_EXCL | O_BINARY, _S_IREAD | _S_IWRITE);
		if (fd >= 0 || errno != EEXIST) {
			return fd;
		}

		strcpy_s(tmp, len, cp);
	}

    errno = EEXIST;
    return -1;
}

#endif