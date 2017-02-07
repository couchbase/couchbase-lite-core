//
//  c4Internal.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 9/15/15.
//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
//

#pragma once

#include "Base.hh"
#include "Error.hh"
#include "RefCounted.hh"
#include "PlatformCompat.hh"
#include "function_ref.hh"
#include <functional>


// Defining C4DB_THREADSAFE as 1 will make C4Database thread-safe: the same handle can be called
// simultaneously from multiple threads. Transactions will be single-threaded: once a thread has
// called c4db_beginTransaction, other threads making that call will block until the transaction
// ends.
#if C4DB_THREADSAFE
#include <mutex>
#endif


struct C4DocEnumerator;

namespace litecore {
    class Record;
}

using namespace std;
using namespace litecore;


// SLICE STUFF:

typedef struct _C4Slice {
    const void *buf;
    size_t size;

    operator slice() const { return {buf, size}; }
    explicit operator std::string() const { return std::string((const char*)buf, size); }

    bool operator==(const struct _C4Slice &s) const {
        return size == s.size &&
            memcmp(buf, s.buf, size) == 0;
    }
    bool operator!=(const struct _C4Slice &s) const { return !(*this == s); }

    _C4Slice& operator= (slice s)   {buf = s.buf; size = s.size; return *this;}
} C4Slice;

static inline C4Slice toc4slice(slice s)
{
    return {s.buf, s.size};
}

typedef struct {
    const void *buf;
    size_t size;
} C4SliceResult;


constexpr C4Slice C4SliceNull = { nullptr, 0 };
#define kC4SliceNull C4SliceNull


#define C4_IMPL // This tells c4Base.h to skip its declaration of C4Slice
#include "c4Base.h"


namespace c4Internal {

    // ERRORS & EXCEPTIONS:

    const size_t kMaxErrorMessagesToSave = 10;

    void recordError(C4ErrorDomain, int code, const char *message, C4Error* outError) noexcept;
    void recordError(C4ErrorDomain, int code, C4Error* outError) noexcept;
    void recordException(const exception &e, C4Error* outError) noexcept;
    static inline void clearError(C4Error* outError) noexcept {if (outError) outError->code = 0;}

    #define catchError(OUTERR) \
        catch (const exception &x) { \
            recordException(x, OUTERR); \
        }

    #define catchExceptions() \
        catch (const exception &x) { }

    #define checkParam(TEST, OUTERROR) \
        ((TEST) || (recordError(LiteCoreDomain, kC4ErrorInvalidParameter, OUTERROR), false))

    // Calls the function, returning its return value. If an exception is thrown, stores the error
    // into `outError`, and returns a default 0/nullptr/false value.
    template <typename RESULT>
    NOINLINE RESULT tryCatch(C4Error *outError, function_ref<RESULT()> fn) noexcept {
        try {
            return fn();
        } catchError(outError);
        return RESULT(); // this will be 0, nullptr, false, etc.
    }

    // Calls the function and returns true. If an exception is thrown, stores the error
    // into `outError`, and returns false.
    NOINLINE bool tryCatch(C4Error *error, function_ref<void()> fn) noexcept;

    // SLICES:

    C4SliceResult sliceResult(slice s);
    C4SliceResult sliceResult(alloc_slice s);
    C4SliceResult sliceResult(const char *str);

    // DOC ENUMERATORS:

    typedef function<bool(const Record&,
                          uint32_t documentFlags,  // C4DocumentFlags
                          slice docType)> EnumFilter;

    void setEnumFilter(C4DocEnumerator*, EnumFilter);


    // Internal C4EnumeratorFlags value. Includes purged docs (what ForestDB calls 'deleted'),
    // so this is equivalent to litecore::RecordEnumerator::includeDeleted.
    // Should only need to be used internally, for the view indexer's enumerator.
    static const uint16_t kC4IncludePurged = 0x8000;

}

using namespace c4Internal;
