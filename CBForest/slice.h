//
//  slice.h
//  CBForest
//
//  Created by Jens Alfke on 4/20/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//

#ifndef CBForest_slice_h
#define CBForest_slice_h

#include <stddef.h>
#include <stdlib.h>


#define offsetby(PTR,OFFSET) (void*)((uint8_t*)(PTR)+(OFFSET))


/** A bounded region of memory. */
typedef struct {
    const void* buf;
    size_t      size;
} slice;


/** Copies the slice into a newly malloced buffer, and returns a new slice pointing to it. */
slice slicecopy(slice buf);

/** Basic binary comparison of two slices, returning -1, 0 or 1. */
int slicecmp(slice a, slice b);


#ifdef __cplusplus

#include <string>
#include <memory>

namespace forestdb {

    /** A simple range of memory. No ownership implied. */
    struct slice {
        const void* buf;
        size_t      size;

        slice()                                   :buf(NULL), size(0) {}
        slice(const void* b, size_t s)            :buf(b), size(s) {}
        slice(const void* start, const void* end) :buf(start), size((uint8_t*)end-(uint8_t*)start){}
        slice(const std::string& str)             :buf(&str[0]), size(str.length()) {}

        explicit slice(const char* str)           :buf(str), size(strlen(str)) {}

        const void* offset(size_t o) const          {return (uint8_t*)buf + o;}
        size_t offsetOf(const void* ptr) const      {return (uint8_t*)ptr - (uint8_t*)buf;}
        const void* end() const                     {return offset(size);}

        const uint8_t& operator[](unsigned i) const {return ((const uint8_t*)buf)[i];}
        slice operator()(unsigned i, unsigned n) const {return slice(offset(i), n);}

        slice read(size_t nBytes);
        bool readInto(slice dst);

        const void* findByte(uint8_t byte) const      {return ::memchr(buf, byte, size);}

        int compare(slice) const;
        bool equal(slice s) const {return compare(s)==0;}

        slice copy() const;

        void free();

        void moveStart(ptrdiff_t delta)         {buf = offsetby(buf, delta); size -= delta;}

        class _none;
        slice& operator=(_none*)                {buf = NULL; size = 0; return *this;}
        slice(_none*)                           :buf(NULL), size(0) {}
        operator const _none*() const           {return (const _none*)buf;}

        explicit operator std::string() const;

        operator ::slice()                      {return ::slice{buf, size};}
        slice(::slice s)                        :buf(s.buf), size(s.size) {}

        static const slice null;

#ifdef __OBJC__
        slice(NSData* data)                     :buf(data.bytes), size(data.length) {}
        slice(NSString* str);

        explicit operator NSData*() const;
        explicit operator NSString*() const;
#endif
    };

    /** An allocated range of memory. Constructors allocate, destructor frees. */
    struct alloc_slice : std::shared_ptr<void>, public slice {
        alloc_slice()
            :std::shared_ptr<void>(NULL), slice() {}
        explicit alloc_slice(size_t s)
            :std::shared_ptr<void>(malloc(s),::free), slice(get(),s) {}
        explicit alloc_slice(slice s)
            :std::shared_ptr<void>((void*)s.copy().buf,::free), slice(get(),s.size) {}
        alloc_slice(const void* b, size_t s)
            :std::shared_ptr<void>(alloc(b,s),::free), slice(get(),s) {}
        alloc_slice(const void* start, const void* end)
            :std::shared_ptr<void>(alloc(start,(uint8_t*)end-(uint8_t*)start),::free),
             slice(get(),(uint8_t*)end-(uint8_t*)start) {}
        alloc_slice(std::string str)
            :std::shared_ptr<void>(alloc(&str[0], str.length()),::free), slice(get(), str.length()) {}

    private:
        static void* alloc(const void* src, size_t size);
    };
}


#endif // !__cplusplus

#endif
