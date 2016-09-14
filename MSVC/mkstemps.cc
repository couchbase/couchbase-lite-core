#include <errno.h>
#include <io.h>
#include <fcntl.h>

int mkstemps(char *tmplate, int length)
{
	errno_t err = _mktemp_s(tmplate, length);
	if (err != 0) {
		_set_errno(err);
		return -1;
	}

	int fd = -1;
	err = _sopen_s(&fd, tmplate, _O_WRONLY | _O_CREAT | _O_TRUNC, SH_DENYNO, 0);
	if (err != 0) {
		_set_errno(err);
		return -1;
	}

	return fd;
}