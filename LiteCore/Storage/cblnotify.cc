//
// cblnotify.cc
//
// Copyright © 2021 Couchbase. All rights reserved.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

// This is a standalone CLI tool adapted from CrossProcessNotifier.cc.
// All it does is post a notification to the file given as its command line argument.

#include "CrossProcessNotifierData.hh"
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

using namespace litecore;


static int _check(const char *fn, int result) {
    if (result != 0) {
        fprintf(stderr, "error %d from %s\n", result, fn);
        exit(1);
    }
    return result;
}

#define check(EXPR) _check(#EXPR, EXPR)

#define LOCK(MUTEX)     CrossProcessNotifierData::Lock _lock(MUTEX)


int main(int argc, const char **argv) {
    if (argc < 1)
        return -1;
    const char *path = argv[1];

    int err;
    int fd = ::open(path, O_CREAT | O_RDWR, 0600);

    if (fd < 0) {
        err = errno;
        _check("open()", err);
        return 1;
    }
    // Ensure the file is non-empty, without deleting any existing contents:
    ::ftruncate(fd, 4096);

    // Map it read-write:
    void *mapped = ::mmap(nullptr,
                          sizeof(CrossProcessNotifierData),
                          PROT_READ | PROT_WRITE,
                          MAP_FILE | MAP_SHARED,
                          fd,
                          0);
    err = errno;
    ::close(fd);
    if (mapped == MAP_FAILED) {
        _check("mmap()", err);
        return 1;
    }
    auto _sharedData = (CrossProcessNotifierData*)mapped;
    if (_sharedData->uninitialized()) {
        fprintf(stderr, "Initializing shared data\n");
        _sharedData->initialize();
    } else if (!_sharedData->valid()) {
        fprintf(stderr, "Shared data appears invalid\n");
        return 1;
    }

    {
        int _myPID = getpid();
        printf("Posting notification (from PID %d)\n", _myPID);
        LOCK(_sharedData);
        check(_sharedData->broadcast(_myPID));
    }

    return 0;
}
