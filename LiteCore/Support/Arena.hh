//
// Arena.hh
//
// Copyright 2020-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "Base.hh"
#include "fleece/function_ref.hh"
#include <memory>
#include <vector>

C4_ASSUME_NONNULL_BEGIN

#if __has_attribute(alloc_size) && __has_attribute(malloc)
#    define C4ALLOCSIZE(PARAM) __attribute__((alloc_size(PARAM))) __attribute__((malloc))
#else
#    define C4ALLOCSIZE(PARAM)
#endif

namespace litecore {

    /** A simple memory allocator that carves blocks out of a pre-allocated fixed-size heap block.
        To allocate a new block it simply bumps a pointer forward by the size requested.
        Obviously all blocks are freed/invalidated when the Arena itself is destructed.

        Most likely you won't use this directly, instead preferring `Arena`, which is growable.

        @warning  The current implementation only supports block sizes up to 255 bytes, because that's all the
            QueryTranslator needs. Raising this limit would take a little bit of work.

        @note  This class is not thread-safe. If you need that, see `ConcurrentArena` in Fleece.*/
    class FixedArena {
      public:
        /// Constructs an arena with the given byte capacity. This allocates a block of that size
        /// from the default heap using ::operator new.
        explicit FixedArena(size_t capacity, size_t alignment = sizeof(void*));

        FixedArena(FixedArena const&)            = delete;
        FixedArena& operator=(FixedArena const&) = delete;
        FixedArena(FixedArena&&)                 = default;
        FixedArena& operator=(FixedArena&&)      = default;

        size_t blockCount() const FLPURE { return _sizes - _heapEnd; }

        size_t capacity() const FLPURE { return _heapEnd - _heap.get(); }

        size_t allocated() const FLPURE { return _nextBlock - _heap.get(); }

        size_t available() const FLPURE { return _heapEnd - _nextBlock - 1; }

        ///Allocates a new block of the given size.
        /// @return The new block, or nullptr if there's no space.
        void* C4NULLABLE alloc(size_t size) C4ALLOCSIZE(2);

        /// Allocates and zeroes a new block of the given size.
        /// @return The new block, or nullptr if there's no space.
        void* C4NULLABLE calloc(size_t size) C4ALLOCSIZE(2);

        /// Possibly frees a block. This is a no-op unless this is the most recently-allocated block.
        bool free(void* C4NULLABLE block);

        /// Frees all allocated blocks, resetting the arena to its empty state.
        /// (Does not free the arena heap itself!)
        void freeAll() { _nextBlock = _heap.get(); }

        /// Calls a function with each block's address.
        void eachBlock(fleece::function_ref<void(void*, size_t)> const&);

      protected:
        friend class Arena;

        bool contains(void* C4NULLABLE addr) const FLPURE { return addr >= _heap.get() && addr < _nextBlock; }

        void* lastBlock() const FLPURE;

      private:
        std::unique_ptr<uint8_t[]> _heap;       // The heap block used for storage
        uint8_t*                   _sizes;      // Points just past the end of the heap
        uint8_t*                   _heapEnd;    // Points just past the last byte that can be allocated
        uint8_t*                   _nextBlock;  // Points to the next available byte
        size_t                     _alignment;  // Alignment of blocks
    };

    /** A growable arena. It maintains multiple FixedArenas, and when there's no space for a new
        block, it allocates a new FixedArena.
        @note  This class is not thread-safe. If you need that, see `ConcurrentArena` in Fleece.*/
    class Arena {
      public:
        explicit Arena(size_t chunkSize, size_t alignment = alignof(void*));

        Arena(Arena const&)            = delete;
        Arena& operator=(Arena const&) = delete;
        Arena(Arena&&)                 = default;
        Arena& operator=(Arena&&)      = default;

        size_t blockCount() const FLPURE;
        size_t capacity() const FLPURE;
        size_t allocated() const FLPURE;
        size_t available() const FLPURE;

        void* alloc(size_t size) C4ALLOCSIZE(2);
        void* calloc(size_t size) C4ALLOCSIZE(2);
        bool  free(void* C4NULLABLE block);
        void  freeAll(bool freeChunks = false);
        void  eachBlock(fleece::function_ref<void(void*, size_t)> const&);

      private:
        std::vector<FixedArena> _chunks;
        FixedArena* C4NULLABLE  _lastAllocedChunk = nullptr;
        size_t                  _chunkSize, _alignment;
    };
}  // namespace litecore

C4_ASSUME_NONNULL_END
