//
// ThreadUtil.cc
//
// Copyright 2021-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "ThreadUtil.hh"

#if defined(CMAKE)
#    include "config_thread.h"
#elif defined(__APPLE__)
#    define HAVE_PTHREAD_GETNAME_NP
#    define HAVE_PTHREAD_THREADID_NP
#endif

#include <thread>
#include <string>
#include <sstream>


#ifndef _MSC_VER
#    pragma mark - UNIX:

#    include <pthread.h>
#    include <sys/types.h>
#    include <unistd.h>
#    ifndef HAVE_PTHREAD_THREADID_NP
#        include <sys/syscall.h>
#    endif
#    ifndef HAVE_PTHREAD_GETNAME_NP
#        include <sys/prctl.h>
#    endif

namespace litecore {

    void SetThreadName(const char* name) {
#    ifdef __APPLE__
        pthread_setname_np(name);
#    else
        pthread_setname_np(pthread_self(), name);
#    endif
    }

    std::string GetThreadName() {
        std::string       retVal;
        std::stringstream s;
        char              name[256];
#    if defined(HAVE_PTHREAD_GETNAME_NP)
        if ( pthread_getname_np(pthread_self(), name, 255) == 0 && name[0] != 0 ) { s << name << " "; }
#    elif defined(HAVE_PRCTL)
        if ( prctl(PR_GET_NAME, name, 0, 0, 0) == 0 ) { s << name << " "; }
#    else
        s << "<unknown thread name> ";
#    endif

        pid_t tid;
#    if defined(HAVE_PTHREAD_THREADID_NP)
        // FreeBSD only pthread call, cannot use with glibc, and conversely syscall
        // is deprecated in macOS 10.12+
        uint64_t tmp;
        pthread_threadid_np(pthread_self(), &tmp);
        tid = (pid_t)tmp;
#    elif defined(HAVE_SYS_GETTID)
        tid = syscall(SYS_gettid);
#    elif defined(HAVE_NR_GETTID)
        tid = syscall(__NR_gettid);
#    endif

        s << "(" << tid << ")";
        return s.str();
    }
}  // namespace litecore


#else  // i.e. ifdef _MSC_VER
#    pragma mark - WINDOWS:

#    include <Windows.h>
#    include <atlbase.h>

namespace litecore {

    void SetThreadName(const char* name) {
        CA2WEX<256> wide(name, CP_UTF8);
        SetThreadDescription(GetCurrentThread(), wide);
    }

    std::string GetThreadName() {
        std::string       retVal;
        std::stringstream s;

        wchar_t* buf;
        HRESULT  r = GetThreadDescription(GetCurrentThread(), &buf);
        if ( SUCCEEDED(r) ) {
            CW2AEX<256> mb(buf, CP_UTF8);
            retVal = mb;
            LocalFree(buf);
        }

        if ( retVal.empty() ) {
            s << std::this_thread::get_id();
            retVal = s.str();
        }

        return retVal;
    }
}  // namespace litecore

#endif  // _MSC_VER
