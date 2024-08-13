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
#include <numeric>

using namespace std;

namespace litecore {

    /*
     A FixedArena allocates a single block from the malloc heap.
     - Blocks are allocated starting from the lowest address.
     - For every block, a single byte is reserved starting from the highest address.
       The block's size is stored in this byte. (Yes, this does limit blocks to 255 bytes...)
     - Blocks can be iterated by walking forwards from the start of the arena, while reading the sizes starting from
       the back.

     The Arena class simply creates one or more FixedArenas. When it runs out of space it allocates a new one.
     */

#pragma mark - FIXED ARENA:

    FixedArena::FixedArena(size_t capacity, size_t alignment, bool iterable)
        : _heap(new uint8_t[capacity])
        , _sizes(&_heap[capacity])
        , _heapEnd(_sizes)
        , _nextBlock(&_heap[0])
        , _alignment(alignment)
        , _iterable(iterable) {
        DebugAssert(alignment > 0 && (alignment & (alignment - 1)) == 0);  // alignment must be power of 2
        Assert((uintptr_t(_heap.get()) & (alignment - 1)) == 0, "FixedArena heap isn't aligned");
        // NOTE: There are ways to allocate `_heap` and enforce the right alignment,
        // but none of the ways I tried (std::aligned_alloc, std::align_val_t) worked with MSVC.
    }

    size_t FixedArena::blockCount() const {
        Assert(_iterable);
        return _sizes - _heapEnd;
    }

    void* FixedArena::alloc(size_t size) {
        size = (size + _alignment - 1) & ~(_alignment - 1);  // Bump size to ensure next block will be aligned
        uint8_t* result  = _nextBlock;
        uint8_t* newNext = result + size;
        if ( _iterable ) {
            if ( size > 0xFF ) {
                DebugAssert(size <= 0xFF);
                return nullptr;
            }
            if ( _usuallyFalse(newNext >= _heapEnd) ) return nullptr;  // overflow!
            *--_heapEnd = uint8_t(size);                               // Store block size
        } else {
            if ( _usuallyFalse(newNext > _heapEnd) ) return nullptr;  // overflow!
        }
        _nextBlock = newNext;
        return result;
    }

    void* FixedArena::calloc(size_t size) {
        auto block = alloc(size);
        if ( _usuallyTrue(block != nullptr) ) memset(block, 0, size);
        return block;
    }

    void* FixedArena::lastBlock() const {
        Assert(_iterable);
        if ( _usuallyFalse(_nextBlock == _heap.get()) ) return nullptr;
        return _nextBlock - *_heapEnd;
    }

    bool FixedArena::free(void* block) {
        if ( _iterable && block == lastBlock() ) {
            _nextBlock = static_cast<uint8_t*>(block);
            ++_heapEnd;  // pop the block's size
            return true;
        } else {
            DebugAssert(block == nullptr || contains(block));
            return false;
        }
    }

    void FixedArena::eachBlock(fleece::function_ref<void(void*, size_t)> const& callback) {
        Assert(_iterable);
        uint8_t* block = _heap.get();
        for ( uint8_t* sizep = _sizes - 1; sizep >= _heapEnd; --sizep ) {
            DebugAssert(block + *sizep <= _heapEnd);
            callback(block, *sizep);
            block += *sizep;
        }
    }

#pragma mark - ARENA:

    Arena::Arena(size_t chunkSize, size_t alignment, bool iterable)
        : _chunkSize(chunkSize), _alignment(alignment), _iterable(iterable) {
        _chunks.emplace_back(_chunkSize, _alignment, _iterable);
    }

    void* Arena::alloc(size_t size) {
        for ( auto i = _chunks.rbegin(); i != _chunks.rend(); ++i ) {
            if ( i->available() >= size ) {
                if ( void* result = i->alloc(size) ) return result;
            }
        }
        _chunks.emplace_back(std::max(_chunkSize, size + 1), _alignment);
        return _chunks.back().alloc(size);
    }

    bool Arena::free(void* block) {
        for ( auto i = _chunks.rbegin(); i != _chunks.rend(); ++i ) {
            if ( i->contains(block) ) return i->free(block);
        }
        return false;
    }

    void Arena::freeAll(bool freeChunks) {
        if ( freeChunks ) _chunks.erase(_chunks.begin() + 1, _chunks.end());
        for ( auto& chunk : _chunks ) chunk.freeAll();
    }

    void Arena::eachBlock(fleece::function_ref<void(void*, size_t)> const& callback) {
        for ( auto& chunk : _chunks ) chunk.eachBlock(callback);
    }

    size_t Arena::blockCount() const {
        return std::accumulate(_chunks.begin(), _chunks.end(), 0,
                               [](auto sum, auto& chunk) { return sum + chunk.blockCount(); });
    }

    size_t Arena::capacity() const {
        return std::accumulate(_chunks.begin(), _chunks.end(), 0,
                               [](auto sum, auto& chunk) { return sum + chunk.capacity(); });
    }

    size_t Arena::allocated() const {
        return std::accumulate(_chunks.begin(), _chunks.end(), 0,
                               [](auto sum, auto& chunk) { return sum + chunk.allocated(); });
    }

    size_t Arena::available() const {
        return std::accumulate(_chunks.begin(), _chunks.end(), 0,
                               [](auto sum, auto& chunk) { return sum + chunk.available(); });
    }

}  // namespace litecore
