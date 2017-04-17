/*	$Id: compat_mkdtemp.c,v 1.2 2015/10/06 18:32:19 schwarze Exp $	*/
/*
* Copyright (c) 2015 Ingo Schwarze <schwarze@openbsd.org>
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
* WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
* MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
* ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
* WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
* ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
* OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*
* The algorithm of this function is inspired by OpenBSD mkdtemp(3)
* by Theo de Raadt and Todd Miller, but the code differs.
*/

#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <atlbase.h>

char *mkdtemp(char *path)
{
	wchar_t		*start, *cp;
	unsigned	 int tries;

	CA2WEX<256> wpath(path, CP_UTF8);
	start = wcschr(wpath, L'\0');
	while (start > wpath && start[-1] == L'X')
		start--;

	for (tries = INT_MAX; tries; tries--) {
		if (_wmktemp(wpath) == NULL) {
			errno = EEXIST;
			return NULL;
		}
		if (_wmkdir(wpath) == 0) {
			CW2AEX<256> retVal(wpath, CP_UTF8);
			strcpy(path, retVal.m_psz);
			return path;
		}
		if (errno != EEXIST)
			return NULL;
		for (cp = start; *cp != '\0'; cp++)
			*cp = 'X';
	}
	errno = EEXIST;
	return NULL;
}