//
// RingBuffer.hh
//
// Copyright 2025-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "Base.hh"
#include "fleece/slice.hh"
#include <memory>
#include <stdexcept>

namespace litecore {

    /** A classic FIFO buffer of bytes. */
    class RingBuffer {
    public:
        /// Constructs a RingBuffer that can hold up to `capacity` bytes.
        explicit RingBuffer(size_t capacity)
        :_capacity{capacity}
        ,_buffer{std::make_unique<std::byte[]>(capacity)}
        { }

        [[nodiscard]] size_t capacity() const FLPURE {return _capacity;}
        [[nodiscard]] size_t size() const FLPURE {return _size;}
        [[nodiscard]] bool empty() const FLPURE {return _size == 0;}

        /// The number of bytes that can be written, i.e. amount of empty space.
        [[nodiscard]] size_t available() const FLPURE {return _capacity - _size;}

        void clear() {_start = _size = 0;}

        /// Grows or shrinks the capacity.
        /// @throws std::invalid_argument if the new capacity is smaller than the current size.
        void setCapacity(size_t newCapacity) {
            if (newCapacity != _capacity) {
                if (newCapacity < _size) [[unlikely]]
                    throw std::invalid_argument("capacity is too small for RingBuffer's contents");
                RingBuffer newBuffer(newCapacity);
                (void) newBuffer.write(this->readSome(_size));
                (void) newBuffer.write(this->readSome(_size));
                std::swap(*this, newBuffer);
            }
        }

        /// Adds data to the end of the buffer, up to its capacity.
        /// @returns the number of bytes added. */
        [[nodiscard]] size_t write(slice data) {
            size_t n = std::min(data.size, available());
            if (n == 0) [[unlikely]]
                return 0;
            size_t end = _start + _size;
            if (end >= _capacity)
                end -= _capacity;
            size_t n1 = std::min(n, _capacity - end);
            ::memcpy(&_buffer[end], data.buf, n1);
            if (n1 > n)
                ::memcpy(&_buffer[0], &data[n1], n - n1);
            _size += n;
            return n;
        }

        /// Adds all of `data` to the end of the buffer, increasing capacity if necessary. */
        void growAndWrite(slice data) {
            if (size_t cap = _size + data.size; cap > _capacity)
                setCapacity(std::max(cap, 2 * _capacity));
            (void) write(data);
        }

        /// Returns a slice pointing to contiguous bytes from the start of the buffer.
        /// @warning May not return all the bytes, if they wrap around.
        [[nodiscard]] slice peek() const FLPURE {
            size_t n = std::min(_size, _capacity - _start);
            return slice(&_buffer[_start], n);
        }

        /// Removes up to `size` contiguous bytes from the _start_ of the buffer.
        void discard(size_t nBytes) {
            (void)readSome(nBytes);
        }

        /// Removes up to `size` contiguous bytes from the _start_ of the buffer.
        /// @returns a slice pointing to the bytes read.
        /// @note  Data in the buffer is often non-contiguous, so it may take two calls to this
        ///        method to get as many bytes as you want.
        /// @warning  The bytes are invalidated when the RingBuffer is modified.
        [[nodiscard]] slice readSome(size_t size) {
            size_t n = std::min({size, _size, _capacity - _start});
            slice result(&_buffer[_start], n);
            _size -= n;
            _start += n;
            if (_size == 0 || _start == _capacity)
                _start = 0;
            return result;
        }

        /// Copies up to `size` bytes from the _start_ of the buffer to `dst`
        /// and removes them from the buffer.
        /// @returns the number of bytes read.
        [[nodiscard]] size_t read(void* dst, size_t size) {
            slice bytes1 = readSome(size);
            bytes1.copyTo(dst);
            if (empty())
                return bytes1.size;
            slice bytes2 = readSome(size - bytes1.size);
            bytes2.copyTo(fleece::offsetby(dst, bytes1.size));
            return bytes1.size + bytes2.size;
        }


    private:
        size_t                       _capacity;     // Total size of buffer
        size_t                       _start = 0;    // Index in _buffer of first byte
        size_t                       _size = 0;     // Number of bytes currently stored
        std::unique_ptr<std::byte[]> _buffer;       // Heap-allocated data buffer
    };

}
