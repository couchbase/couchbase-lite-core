//
//  c4Internal.hh
//  CBForest
//
//  Created by Jens Alfke on 9/15/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#ifndef c4Internal_h
#define c4Internal_h

#include "Base.hh"
#include "Error.hh"
#include <functional>


// Defining C4DB_THREADSAFE as 1 will make C4Database thread-safe: the same handle can be called
// simultaneously from multiple threads. Transactions will be single-threaded: once a thread has
// called c4db_beginTransaction, other threads making that call will block until the transaction
// ends.
#if C4DB_THREADSAFE
#include <mutex>
#endif


struct C4DocEnumerator;

namespace cbforest {
    class Database;
    class Document;
}

using namespace cbforest;


// SLICE STUFF:


// Predefine C4Slice as a typedef of slice so we can use the richer slice API:

typedef slice C4Slice;

typedef struct {
    const void *buf;
    size_t size;
} C4SliceResult;


#define kC4SliceNull slice::null


#define C4_IMPL // This tells c4Base.h to skip its declaration of C4Slice
#include "c4Base.h"


namespace c4Internal {

    // ERRORS & EXCEPTIONS:

    void recordError(C4ErrorDomain domain, int code, C4Error* outError);
    void recordException(const std::exception &e, C4Error* outError);
    void recordUnknownException(C4Error* outError);
    static inline void clearError(C4Error* outError) {if (outError) outError->code = 0;}

    #define catchError(OUTERR) \
        catch (const std::exception &x) { \
            recordException(x, OUTERR); \
        }


    // DOC ENUMERATORS:

    typedef std::function<bool(const Document&,
                               uint32_t documentFlags,  // C4DocumentFlags
                               slice docType)> EnumFilter;

    void setEnumFilter(C4DocEnumerator*, EnumFilter);


    // Internal C4EnumeratorFlags value. Includes purged docs (what ForestDB calls 'deleted'),
    // so this is equivalent to cbforest::DocEnumerator::includeDeleted.
    // Should only need to be used internally, for the view indexer's enumerator.
    static const uint16_t kC4IncludePurged = 0x8000;


    // BASE CLASSES:

    /** Base class that keeps track of the total instance count of all subclasses,
        which is returned by c4_getObjectCount(). */
    class InstanceCounted {
    public:
        static std::atomic_int gObjectCount;
        InstanceCounted()   {++gObjectCount;}
        ~InstanceCounted()  {--gObjectCount;}
    };


    /** Simple thread-safe ref-counting implementation.
        Note: The ref-count starts at 0, so you must call retain() on an instance right after
        constructing it. */
    template <typename SELF>
    struct RefCounted : InstanceCounted {

        int refCount() const { return _refCount; }

        SELF* retain() {
            ++_refCount;
            return (SELF*)this;
        }

        void release() {
            int newref = --_refCount;
            CBFAssert(newref >= 0);
            if (newref == 0) {
                delete this;
            }
        }

    protected:
        /** Destructor is accessible only so that it can be overridden.
            Never call delete, only release! */
        virtual ~RefCounted() {
            CBFAssert(_refCount == 0);
        }

    private:
        std::atomic_int _refCount {0};
    };
}

using namespace c4Internal;


#endif /* c4Internal_h */
