/* crc32c.c -- compute CRC-32C using the Intel crc32 instruction
  * Copyright (C) 2013 Mark Adler
  * Version 1.1  1 Aug 2013  Mark Adler
  */

/*
  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the author be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Mark Adler
  madler@alumni.caltech.edu
 */

//
// Software and Hardware Assisted CRC32-C function.
//
// This is an altered/adapted version of Mark Adler's crc32c.c
//  - see http://stackoverflow.com/a/17646775
//  - see above license.
//  - This module provides the HW support and is built
//    with -msse4.2 where applicable. We only execute inside
//    this module if SSE4.2 is detected.
//
// Changes from orginal version include.
//  a) Compiler intrinsics instead of inline asm.
//  b) Some re-styling, commenting and code style safety.
//    i) no if or loops without braces.
//    ii) variable initialisation.
//  c) GCC/CLANG/MSVC safe.
//  d) C++ casting and limits.
//  e) Benchmarked and tuned.
//    i) The 3way optimised version is slower for data sizes < 3xSHORT_BLOCK
//       so fall back to a SHORT_BLOCK only mode or a single issue version.
//    ii) See crc32c_bench.cc for testing
//  f) Validated with IETF test vectors.
//    i) See crc32c_test.cc.
//  g) Use of GCC4.8 attributes to select SSE4.2 vs SW version/
//  h) Custom cpuid code works for GCC(<4.8), CLANG and MSVC.
//  i) Use static initialistion instead of pthread_once.
//
// Changes for mobile: Don't make lack of defines an error, instead just decline to compile
#if defined(__x86_64__) || defined(_M_X64) || defined(_M_IX86)

#include "crc32c.h"

// select header file for crc instructions.
#if defined(WIN32)
#include <nmmintrin.h>
#elif defined(__clang__) || defined(__GNUC__)
#include <smmintrin.h>
#endif

#include <limits>

#if defined(__i386) || defined(_M_IX86)
typedef uint32_t crc_max_size_t;
#define _mm_crc32_max_size _mm_crc32_u32
#else
typedef uint64_t crc_max_size_t;
#define _mm_crc32_max_size _mm_crc32_u64
#endif

//
// CRC32-C implementation using SSE4.2 acceleration
// no pipeline optimisation.
//
uint32_t crc32c_hw_1way(const uint8_t* buf, size_t len, uint32_t crc_in) {
    crc_max_size_t crc = static_cast<crc_max_size_t>(~crc_in);
    // use crc32-byte instruction until the buf pointer is 8-byte aligned
    while ((reinterpret_cast<uintptr_t>(buf) & ALIGN64_MASK) != 0 && len > 0) {
        crc = _mm_crc32_u8(static_cast<uint32_t>(crc), *buf);
        buf += sizeof(uint8_t);
        len -= sizeof(uint8_t);
    }

    // Use crc32_max size until there's no more u32/u64 to process.
    while (len >= sizeof(crc_max_size_t)) {
        crc = _mm_crc32_max_size(crc, *reinterpret_cast<const crc_max_size_t*>(buf));
        buf += sizeof(crc_max_size_t);
        len -= sizeof(crc_max_size_t);
    }

    // finish the rest using the byte instruction
    while (len > 0) {
        crc = _mm_crc32_u8(static_cast<uint32_t>(crc), *buf);
        buf += sizeof(uint8_t);
        len -= sizeof(uint8_t);
    }

    return static_cast<uint32_t>(crc ^ std::numeric_limits<uint32_t>::max());
}

//
// HW assisted crc32c that processes as much data in parallel using 3xSHORT_BLOCKs
//
uint32_t crc32c_hw_short_block(const uint8_t* buf, size_t len, uint32_t crc_in) {
    // If len is less the 3xSHORT_BLOCK just use the 1-way hw version
    if (len < (3*SHORT_BLOCK)) {
        return crc32c_hw_1way(buf, len, crc_in);
    }

    crc_max_size_t crc0 = static_cast<crc_max_size_t>(~crc_in), crc1 = 0, crc2 = 0;

    // use crc32-byte instruction until the buf pointer is 8-byte aligned
    while ((reinterpret_cast<uintptr_t>(buf) & ALIGN64_MASK) != 0 && len > 0) {

        crc0 = _mm_crc32_u8(static_cast<uint32_t>(crc0), *buf);
        buf += sizeof(uint8_t);
        len -= sizeof(uint8_t);
    }

    // process the data using 3 pipelined crc working on 3 blocks of SHORT_BLOCK
    while (len >= (3 * SHORT_BLOCK)) {
        crc1 = 0;
        crc2 = 0;
        const uint8_t* end = buf + SHORT_BLOCK;
        do
        {
            crc0 = _mm_crc32_max_size(crc0, *reinterpret_cast<const crc_max_size_t*>(buf));
            crc1 = _mm_crc32_max_size(crc1, *reinterpret_cast<const crc_max_size_t*>(buf + SHORT_BLOCK));
            crc2 = _mm_crc32_max_size(crc2, *reinterpret_cast<const crc_max_size_t*>(buf + (2 * SHORT_BLOCK)));
            buf += sizeof(crc_max_size_t);
        } while (buf < end);
        crc0 = crc32c_shift(crc32c_short, static_cast<uint32_t>(crc0)) ^ crc1;
        crc0 = crc32c_shift(crc32c_short, static_cast<uint32_t>(crc0)) ^ crc2;
        buf += 2 * SHORT_BLOCK;
        len -= 3 * SHORT_BLOCK;
    }

    // Use crc32_max size until there's no more u32/u64 to process.
    while (len >= sizeof(crc_max_size_t)) {
        crc0 = _mm_crc32_max_size(crc0, *reinterpret_cast<const crc_max_size_t*>(buf));
        buf += sizeof(crc_max_size_t);
        len -= sizeof(crc_max_size_t);
    }

    // finish the rest using the byte instruction
    while (len > 0) {
        crc0 = _mm_crc32_u8(static_cast<uint32_t>(crc0), *buf);
        buf += sizeof(uint8_t);
        len -= sizeof(uint8_t);
    }

    return static_cast<uint32_t>(crc0 ^ std::numeric_limits<uint32_t>::max());
}


//
// A parallelised crc32c issuing 3 crc at once.
// Generally 3 crc instructions can be issued at once.
//
uint32_t crc32c_hw(const uint8_t* buf, size_t len, uint32_t crc_in) {
    // if len is less than the long block it's faster to just process using 3way short-block
    if (len < 3*LONG_BLOCK) {
        return crc32c_hw_short_block(buf, len, crc_in);
    }

    crc_max_size_t crc0 = static_cast<crc_max_size_t>(~crc_in), crc1 = 0, crc2 = 0;

    // use crc32-byte instruction until the buf pointer is 8-byte aligned
    while ((reinterpret_cast<uintptr_t>(buf) & ALIGN64_MASK) != 0 && len > 0) {

        crc0 = _mm_crc32_u8(static_cast<uint32_t>(crc0), *buf);
        buf += sizeof(uint8_t);
        len -= sizeof(uint8_t);
    }

    /* compute the crc on sets of LONG_BLOCK*3 bytes, executing three independent crc
       instructions, each on LONG_BLOCK bytes -- this is optimized for the Nehalem,
       Westmere, Sandy Bridge, and Ivy Bridge architectures, which have a
       throughput of one crc per cycle, but a latency of three cycles */
    while (len >= (3 * LONG_BLOCK)) {
        crc1 = 0;
        crc2 = 0;
        const uint8_t* end = buf + LONG_BLOCK;
        do
        {
            crc0 = _mm_crc32_max_size(crc0, *reinterpret_cast<const crc_max_size_t*>(buf));
            crc1 = _mm_crc32_max_size(crc1, *reinterpret_cast<const crc_max_size_t*>(buf + LONG_BLOCK));
            crc2 = _mm_crc32_max_size(crc2, *reinterpret_cast<const crc_max_size_t*>(buf + (2 * LONG_BLOCK)));
            buf += sizeof(crc_max_size_t);
        } while (buf < end);
        crc0 = crc32c_shift(crc32c_long, static_cast<uint32_t>(crc0)) ^ crc1;
        crc0 = crc32c_shift(crc32c_long, static_cast<uint32_t>(crc0)) ^ crc2;
        buf += 2 * LONG_BLOCK;
        len -= 3 * LONG_BLOCK;
    }

    /* do the same thing, but now on SHORT_BLOCK*3 blocks for the remaining data less
       than a LONG_BLOCK*3 block */
    while (len >= (3 * SHORT_BLOCK)) {
        crc1 = 0;
        crc2 = 0;
        const uint8_t* end = buf + SHORT_BLOCK;
        do
        {
            crc0 = _mm_crc32_max_size(crc0, *reinterpret_cast<const crc_max_size_t*>(buf));
            crc1 = _mm_crc32_max_size(crc1, *reinterpret_cast<const crc_max_size_t*>(buf + SHORT_BLOCK));
            crc2 = _mm_crc32_max_size(crc2, *reinterpret_cast<const crc_max_size_t*>(buf + (2 * SHORT_BLOCK)));
            buf += sizeof(crc_max_size_t);
        } while (buf < end);
        crc0 = crc32c_shift(crc32c_short, static_cast<uint32_t>(crc0)) ^ crc1;
        crc0 = crc32c_shift(crc32c_short, static_cast<uint32_t>(crc0)) ^ crc2;
        buf += 2 * SHORT_BLOCK;
        len -= 3 * SHORT_BLOCK;
    }

    // Use crc32_max size until there's no more u32/u64 to process.
    while (len >= sizeof(crc_max_size_t)) {
        crc0 = _mm_crc32_max_size(crc0, *reinterpret_cast<const crc_max_size_t*>(buf));
        buf += sizeof(crc_max_size_t);
        len -= sizeof(crc_max_size_t);
    }

    // finish the rest using the byte instruction
    while (len > 0) {
        crc0 = _mm_crc32_u8(static_cast<uint32_t>(crc0), *buf);
        buf += sizeof(uint8_t);
        len -= sizeof(uint8_t);
    }

    return static_cast<uint32_t>(crc0 ^ std::numeric_limits<uint32_t>::max());
}

#endif
