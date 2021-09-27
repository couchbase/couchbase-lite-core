//
// mkstemp.cc
//
// Copyright 2018-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//


#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <atlbase.h>
#include "TempArray.hh"
#include "SecureRandomize.hh"
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
	    const uint32_t r = litecore::RandomNumber(62);
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