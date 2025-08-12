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
#include <limits>
#include <memory>
#include <numeric>  // for std::accumulate()
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

        @note  This class is not thread-safe. If you need that, see `ConcurrentArena` in Fleece.*/
    class FixedArena {
      public:
        /// Constructs an arena with the given byte capacity.
        /// Allocates a block of that size from the default heap using ::operator new.
        explicit FixedArena(size_t capacity);

        FixedArena(FixedArena const&)                = delete;
        FixedArena& operator=(FixedArena const&)     = delete;
        FixedArena(FixedArena&&) noexcept            = default;
        FixedArena& operator=(FixedArena&&) noexcept = default;

        /// Total size occupied by the arena.
        size_t capacity() const noexcept FLPURE { return _heapEnd - _heap.get(); }

        /// The number of bytes allocated.
        size_t allocated() const noexcept FLPURE { return _free - _heap.get(); }

        /// The number of bytes available, i.e. the largest block that can be allocated.
        size_t available() const noexcept FLPURE { return _sentinel - _free; }

        /// True if `addr` points to memory allocated by this arena.
        bool contains(void* C4NULLABLE addr) const noexcept FLPURE { return addr >= _heap.get() && addr < _free; }

        ///Allocates a new block of the given size.
        /// @returns The new block, or nullptr if there's no space.
        void* C4NULLABLE alloc(size_t size, size_t alignment = sizeof(void*)) noexcept C4ALLOCSIZE(2);

        /// Possibly frees a block. (In this class it's a no-op.)
        /// @returns True if this actually freed up any space.
        bool free(void* C4NULLABLE block) noexcept;

        /// Implicitly frees all allocated blocks, resetting the arena to its empty state.
        /// (Does not free the arena heap itself!)
        void freeAll() noexcept { _free = _heap.get(); }

        class Marker {};

        /// Returns an opaque marker of the current heap state.
        Marker* mark() const { return (Marker*)_free; }

        /// Restores the arena to the state when `mark` was called, implicitly freeing all newer blocks.
        void freeToMark(Marker*);

      protected:
        std::unique_ptr<uint8_t[]> _heap;      // The heap block used for storage
        uint8_t*                   _heapEnd;   // Points just past the end of the heap
        uint8_t*                   _free;      // Points to the first free byte
        uint8_t*                   _sentinel;  // Points just past the last byte that can be allocated
    };

    /** A simple memory allocator that's mostly equivalent to FixedArena.
        Unlike FixedArena, it allows its blocks to be iterated via a callback function, which is useful if you do
        need some kind of cleanup.
        On the downside, the maximum size block it can allocate is 255 bytes. */
    class IterableFixedArena : private FixedArena {
      public:
        /// Constructs an arena with the given byte capacity.
        /// Allocates a block of that size from the default heap using ::operator new.
        explicit IterableFixedArena(size_t capacity) : FixedArena(capacity + 1) {}

        size_t capacity() const noexcept FLPURE { return FixedArena::capacity(); }

        size_t allocated() const noexcept FLPURE { return FixedArena::allocated() + blockCount(); }

        size_t available() const noexcept FLPURE { return FixedArena::available() - 1; }

        /// The number of allocated blocks.
        size_t blockCount() const noexcept FLPURE;

        void* C4NULLABLE alloc(size_t size, size_t alignment = sizeof(void*)) noexcept C4ALLOCSIZE(2);

        /// Possibly frees a block. Only the most recently-allocated block can actually be freed.
        bool free(void* C4NULLABLE block) noexcept;

        void freeAll() noexcept { _free = _heap.get(); }

        /// Calls a function with each block's address, from newest to oldest.
        void eachBlock(fleece::function_ref<void(void*, size_t)> const&);

        bool contains(void* C4NULLABLE addr) const noexcept FLPURE { return FixedArena::contains(addr); }
    };

    /** A growable arena allocator. It maintains multiple FixedArenas or IterableFixedArenas;
        when the current one runs out of space, it allocates a new one.
        @note  This class is not thread-safe. If you need that, see `ConcurrentArena` in Fleece.*/
    template <class CHUNK = FixedArena>
    class Arena {
      public:
        /// Constructs an Arena.
        /// @param chunkSize  The capacity of each fixed arena that will be allocated.
        /// @note  Doesn't actually allocate any memory until the first call to `alloc`.
        explicit Arena(size_t chunkSize);

        Arena(Arena const&)            = delete;
        Arena& operator=(Arena const&) = delete;
        Arena(Arena&&)                 = default;
        Arena& operator=(Arena&&)      = default;

        size_t capacity() const noexcept FLPURE;
        size_t allocated() const noexcept FLPURE;
        size_t available() const noexcept FLPURE;
        size_t blockCount() const noexcept FLPURE;  // only available if CHUNK is IterableFixedArena

        /// Allocates a block. If the current FixedArena has no room, it allocates a new one from the heap,
        /// with capacity `max(chunkSize, size)` to ensure the allocation will succeed.
        /// @throws std::bad_alloc if heap allocation fails (but this never happens in most "real" operating systems.)
        void* alloc(size_t size, size_t alignment = sizeof(void*)) C4ALLOCSIZE(2) RETURNS_NONNULL;
        bool  free(void* C4NULLABLE block) noexcept;
        void  freeAll() noexcept;

        /// Calls a function with each chunk, from newest to oldest.
        void eachChunk(fleece::function_ref<void(CHUNK&)> const&);

      private:
        CHUNK* C4NULLABLE  _curChunk{};
        std::vector<CHUNK> _chunks;
        size_t             _chunkSize;
    };

    /** Wrapper around Arena that can be used as a C++ Allocator, e.g. `std::vector<int, ArenaAllocator<int>>`.
        @note  See <https://en.cppreference.com/w/cpp/named_req/Allocator>
        @warning Has not been tested yet! */
    template <class T, class CHUNK = FixedArena>
    struct ArenaAllocator {
        typedef T value_type;

        explicit ArenaAllocator(size_t chunkSize) : _arena(chunkSize) {}

        ArenaAllocator(ArenaAllocator&&) noexcept            = default;
        ArenaAllocator& operator=(ArenaAllocator&&) noexcept = default;

        [[nodiscard]] T* C4NONNULL allocate(std::size_t n) {
            if ( n > std::numeric_limits<std::size_t>::max() / sizeof(T) ) throw std::bad_array_new_length();
            return static_cast<T*>(_arena.alloc(n * sizeof(T), alignof(T)));
        }

        void deallocate(T* C4NULLABLE p, std::size_t n) noexcept { _arena.free(p); }

      private:
        Arena<CHUNK> _arena;
    };

#pragma mark - IMPLEMENTATION:

    template <class CHUNK>
    Arena<CHUNK>::Arena(size_t chunkSize) : _chunkSize(chunkSize) {}

    template <class CHUNK>
    size_t Arena<CHUNK>::capacity() const noexcept {
        return std::accumulate(_chunks.begin(), _chunks.end(), 0,
                               [](auto sum, auto& chunk) { return sum + chunk.capacity(); });
    }

    template <class CHUNK>
    size_t Arena<CHUNK>::allocated() const noexcept {
        return std::accumulate(_chunks.begin(), _chunks.end(), 0,
                               [](auto sum, auto& chunk) { return sum + chunk.allocated(); });
    }

    template <class CHUNK>
    size_t Arena<CHUNK>::available() const noexcept {
        return _curChunk ? _curChunk->available() : 0;
    }

    template <class CHUNK>
    size_t Arena<CHUNK>::blockCount() const noexcept {
        return std::accumulate(_chunks.begin(), _chunks.end(), 0,
                               [](auto sum, auto& chunk) { return sum + chunk.blockCount(); });
    }

    template <class CHUNK>
    void* Arena<CHUNK>::alloc(size_t size, size_t alignment) {
        if ( _usuallyTrue(_curChunk != nullptr) ) {
            if ( void* block = _curChunk->alloc(size, alignment); _usuallyTrue(block != nullptr) ) return block;
        }
        _chunks.emplace_back(std::max(_chunkSize, size));
        _curChunk = &_chunks.back();
        return _curChunk->alloc(size, alignment);
    }

    template <class CHUNK>
    bool Arena<CHUNK>::free(void* C4NULLABLE block) noexcept {
        return _curChunk && _curChunk->free(block);
    }

    template <class CHUNK>
    void Arena<CHUNK>::freeAll() noexcept {
        if ( _chunks.empty() ) return;
        _chunks.erase(_chunks.begin(), _chunks.end() - 1);  // keep only the latest chunk
        _curChunk = &_chunks.back();
        _curChunk->freeAll();
    }

    template <class CHUNK>
    void Arena<CHUNK>::eachChunk(fleece::function_ref<void(CHUNK&)> const& callback) {
        for ( auto i = _chunks.rbegin(); i != _chunks.rend(); ++i ) callback(*i);
    }
}  // namespace litecore

C4_ASSUME_NONNULL_END
