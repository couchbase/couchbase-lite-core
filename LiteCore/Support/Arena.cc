//
// Arena.cc
//
// Copyright 2020-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "Arena.hh"
#include "Error.hh"
#include <cstring>

using namespace std;

namespace litecore {

    [[maybe_unused]] FLCONST static inline bool validAlignment(size_t alignment) {
        return alignment > 0 && (alignment & (alignment - 1)) == 0;  // alignment must be power of 2
    }

    FLCONST static inline uint8_t* align(uint8_t* ptr, size_t alignment) {
        return reinterpret_cast<uint8_t*>((uintptr_t(ptr) + alignment - 1) & ~(alignment - 1));
    }

#pragma mark - FIXED ARENA:

    FixedArena::FixedArena(size_t capacity)
        : _heap(new uint8_t[capacity]), _heapEnd(&_heap[capacity]), _free(_heap.get()), _sentinel(_heapEnd) {}

    void* FixedArena::alloc(size_t size, size_t alignment) noexcept {
        DebugAssert(validAlignment(alignment));
        uint8_t* result  = align(_free, alignment);
        uint8_t* newNext = result + size;
        if ( _usuallyFalse(newNext > _sentinel) ) return nullptr;  // overflow!
        _free = newNext;
        return result;
    }

    bool FixedArena::free(void* block) noexcept {
        DebugAssert(block == nullptr || contains(block));
        return false;
    }

    void FixedArena::freeToMark(Marker* mark) {
        Assert(contains(mark));
        _free = reinterpret_cast<uint8_t*>(mark);
    }

#pragma mark - ITERABLE:

    /*
     An IterableFixedArena saves the block sizes as a byte array that grows _downward_ from the end of the heap.
     The `_sentinel` always points to the last (lowest) size, which is that latest (highest) block's.
     */

    size_t IterableFixedArena::blockCount() const noexcept { return _heapEnd - _sentinel; }

    void* IterableFixedArena::alloc(size_t size, size_t alignment) noexcept {
        if ( _usuallyFalse(size > 0xFF) ) {
            DebugAssert(size <= 0xFF);
            return nullptr;
        }
        DebugAssert(validAlignment(alignment));

        uint8_t* result  = align(_free, alignment);
        uint8_t* newNext = result + size;
        if ( _usuallyFalse(newNext >= _sentinel) ) return nullptr;  // overflow! (leaving space for size byte)
        if ( size_t adjust = result - _free; adjust > 0 && _sentinel < _heapEnd ) {
            // increase prev block's stored size to account for the gap before the new block:
            if ( size_t(*_sentinel) + adjust > 0xFF ) return nullptr;  // can't bump its size; give up
            *_sentinel += adjust;
        }
        *--_sentinel = uint8_t(size);  // Store block size
        _free        = newNext;
        return result;
    }

    bool IterableFixedArena::free(void* block) noexcept {
        if ( _sentinel < _heapEnd && block == _free - *_sentinel ) {
            _free = static_cast<uint8_t*>(block);
            ++_sentinel;  // pop the block's size
            return true;
        } else {
            return FixedArena::free(block);
        }
    }

    void IterableFixedArena::eachBlock(fleece::function_ref<void(void*, size_t)> const& callback) {
        uint8_t* block = _free;
        for ( uint8_t* sizep = _sentinel; sizep < _heapEnd; ++sizep ) {
            block -= *sizep;
            DebugAssert(block >= _heap.get());
            callback(block, *sizep);
        }
    }

}  // namespace litecore
