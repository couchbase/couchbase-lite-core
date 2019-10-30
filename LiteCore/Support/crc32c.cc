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
//  - See http://stackoverflow.com/a/17646775
//  - See above license.
//  - This module provides a software only crc32c
//    and cpuid code to enable HW assist.
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
// Modified for mobile: Combine crc32c.h and crc32c_private.h

#include "crc32c.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

 // select header file for cpuid.
#if defined(WIN32)
#include <intrin.h>
#elif (defined(__clang__) || defined(__GNUC__)) && (defined(__x86_64__) || defined(_M_X64) || defined(_M_IX86))
#include <cpuid.h>
#endif

static bool setup_tables();
static bool tables_setup = setup_tables();

const uint32_t CRC32C_POLYNOMIAL_REV = 0x82F63B78;
const int TABLE_X = 8, TABLE_Y = 256;
static uint32_t crc32c_sw_lookup_table[TABLE_X][TABLE_Y];
/* Tables for hardware crc that shift a crc by LONG and SHORT zeros. */
uint32_t crc32c_long[SHIFT_TABLE_X][SHIFT_TABLE_Y];
uint32_t crc32c_short[SHIFT_TABLE_X][SHIFT_TABLE_Y];

/* Multiply a matrix times a vector over the Galois field of two elements,
   GF(2).  Each element is a bit in an unsigned integer.  mat must have at
   least as many entries as the power of two for most significant one bit in
   vec. */
static inline uint32_t gf2_matrix_times(const uint32_t *mat, uint32_t vec) {
    uint32_t sum = 0;

    while (vec > 0) {
        if (vec & 1) {
            sum ^= *mat;
        }
        vec >>= 1;
        mat++;
    }
    return sum;
}

/* Multiply a matrix by itself over GF(2).  Both mat and square must have 32
   rows. The result is written to 'square' */
static inline void gf2_matrix_square(uint32_t *square, const uint32_t *mat) {
    int n = 0;

    for (n = 0; n < 32; n++) {
        square[n] = gf2_matrix_times(mat, mat[n]);
    }
}

/* Construct an operator to apply len zeros to a crc.  len must be a power of
   two.  If len is not a power of two, then the result is the same as for the
   largest power of two less than len.  The result for len == 0 is the same as
   for len == 1.  A version of this routine could be easily written for any
   len, but that is not needed for this application. */
static void crc32c_zeros_op(uint32_t *even, size_t len) {
    int n = 0;
    uint32_t row = 1;
    uint32_t odd[32];       /* odd-power-of-two zeros operator */

    /* put operator for one zero bit in odd */
    odd[0] = CRC32C_POLYNOMIAL_REV;              /* CRC-32C polynomial */

    for (n = 1; n < 32; n++) {
        odd[n] = row;
        row <<= 1;
    }

    /* put operator for two zero bits in even */
    gf2_matrix_square(even, odd);

    /* put operator for four zero bits in odd */
    gf2_matrix_square(odd, even);

    /* first square will put the operator for one zero byte (eight zero bits),
       in even -- buf square puts operator for two zero bytes in odd, and so
       on, until len has been rotated down to zero */
    do {
        gf2_matrix_square(even, odd);
        len >>= 1;
        if (len == 0) {
            return;
        }
        gf2_matrix_square(odd, even);
        len >>= 1;
    } while (len > 0);

    /* answer ended up in odd -- copy to even */
    for (n = 0; n < 32; n++) {
        even[n] = odd[n];
    }
}

/* Take a length and build four lookup tables for applying the zeros operator
   for that length, byte-by-byte on the operand. */
static void crc32c_zeros(uint32_t zeros[SHIFT_TABLE_X][SHIFT_TABLE_Y], size_t len) {
    uint32_t op[32];

    crc32c_zeros_op(op, len);
    for (uint32_t n = 0; n < 256; n++) {
        zeros[0][n] = gf2_matrix_times(op, n);
        zeros[1][n] = gf2_matrix_times(op, n << 8);
        zeros[2][n] = gf2_matrix_times(op, n << 16);
        zeros[3][n] = gf2_matrix_times(op, n << 24);
    }
}

// single CRC in software
static inline uint64_t crc32c_sw_inner(uint64_t crc, const uint8_t* buffer) {
    crc ^= *reinterpret_cast<const uint64_t*>(buffer);
    crc = crc32c_sw_lookup_table[7][crc & 0xff] ^
        crc32c_sw_lookup_table[6][(crc >> 8) & 0xff] ^
        crc32c_sw_lookup_table[5][(crc >> 16) & 0xff] ^
        crc32c_sw_lookup_table[4][(crc >> 24) & 0xff] ^
        crc32c_sw_lookup_table[3][(crc >> 32) & 0xff] ^
        crc32c_sw_lookup_table[2][(crc >> 40) & 0xff] ^
        crc32c_sw_lookup_table[1][(crc >> 48) & 0xff] ^
        crc32c_sw_lookup_table[0][crc >> 56];
        return crc;
}

//
// CRC32-C implementation using software
// No optimisation
//
static uint32_t crc32c_sw_1way(const uint8_t* buf, size_t len, uint32_t crc_in) {
    uint64_t crc = static_cast<uint64_t>(~crc_in);

    while ((reinterpret_cast<uintptr_t>(buf) & ALIGN64_MASK) != 0 && len > 0) {
        crc = crc32c_sw_lookup_table[0][(crc ^ *buf) & 0xff] ^ (crc >> 8);
        buf += sizeof(uint8_t);
        len -= sizeof(uint8_t);
    }

    while (len >= sizeof(uint64_t)) {
        crc  = crc32c_sw_inner(crc, buf);
        buf += sizeof(uint64_t);
        len -= sizeof(uint64_t);
    }

    while (len > 0) {
        crc = crc32c_sw_lookup_table[0][(crc ^ *buf) & 0xff] ^ (crc >> 8);
        buf += sizeof(uint8_t);
        len -= sizeof(uint8_t);
    }

    return static_cast<uint32_t>(crc ^ std::numeric_limits<uint32_t>::max());
}

//
// Partially optimised CRC32C which divides the data into 3 blocks
// allowing some free CPU pipelining/parallelisation.
//
static uint32_t crc32c_sw_short_block(const uint8_t* buf, size_t len, uint32_t crc_in) {
    // If len is less the 3 x SHORT_BLOCK just use the 1-way sw version
    if (len < (3 * SHORT_BLOCK)) {
        return crc32c_sw_1way(buf, len, crc_in);
    }

    uint64_t crc = static_cast<uint64_t>(~crc_in), crc1 = 0, crc2 = 0;

    while ((reinterpret_cast<uintptr_t>(buf) & ALIGN64_MASK) != 0 && len > 0) {
        crc = crc32c_sw_lookup_table[0][(crc ^ *buf) & 0xff] ^ (crc >> 8);
        buf += sizeof(uint8_t);
        len -= sizeof(uint8_t);
    }

    // process the data in 3 blocks and combine the crc's using the shift trick
    while (len >= (3 * SHORT_BLOCK)) {
        crc1 = 0;
        crc2 = 0;
        const uint8_t* end = buf + SHORT_BLOCK;
        do
        {
            crc  = crc32c_sw_inner(crc, buf);
            crc1 = crc32c_sw_inner(crc1, (buf + SHORT_BLOCK));
            crc2 = crc32c_sw_inner(crc2, (buf + (2 * SHORT_BLOCK)));
            buf += sizeof(uint64_t);
        } while (buf < end);
        crc = crc32c_shift(crc32c_short, static_cast<uint32_t>(crc)) ^ crc1;
        crc = crc32c_shift(crc32c_short, static_cast<uint32_t>(crc)) ^ crc2;
        buf += 2 * SHORT_BLOCK;
        len -= 3 * SHORT_BLOCK;
    }

    // swallow any remaining longs.
    while (len >= sizeof(uint64_t)) {
        crc = crc32c_sw_inner(crc, buf);
        buf += sizeof(uint64_t);
        len -= sizeof(uint64_t);
    }

    // swallow the remaining bytes.
    while (len > 0) {
        crc = crc32c_sw_lookup_table[0][(crc ^ *buf) & 0xff] ^ (crc >> 8);
        buf += sizeof(uint8_t);
        len -= sizeof(uint8_t);
    }

    return static_cast<uint32_t>(crc ^ std::numeric_limits<uint32_t>::max());
}

//
// CRC32-C software implementation.
//
uint32_t crc32c_sw (const uint8_t* buf, size_t len, uint32_t crc_in) {
    // If len is less than the 3 x LONG_BLOCK it's faster to use the short-block only.
    if (len < (3 * LONG_BLOCK)) {
        return crc32c_sw_short_block(buf, len, crc_in);
    }

    uint64_t crc = static_cast<uint64_t>(~crc_in), crc1 = 0, crc2 = 0;

    while ((reinterpret_cast<uintptr_t>(buf) & ALIGN64_MASK) != 0 && len > 0) {
        crc = crc32c_sw_lookup_table[0][(crc ^ *buf) & 0xff] ^ (crc >> 8);
        buf += sizeof(uint8_t);
        len -= sizeof(uint8_t);
    }

    // process the data in 3 blocks and combine the crc's using the shift trick
    while (len >= (3 * LONG_BLOCK)) {
        crc1 = 0;
        crc2 = 0;
        const uint8_t* end = buf + LONG_BLOCK;
        do
        {
            crc  = crc32c_sw_inner(crc, buf);
            crc1 = crc32c_sw_inner(crc1, (buf + LONG_BLOCK));
            crc2 = crc32c_sw_inner(crc2, (buf + (2 * LONG_BLOCK)));
            buf += sizeof(uint64_t);
        } while (buf < end);
        crc = crc32c_shift(crc32c_long, static_cast<uint32_t>(crc)) ^ crc1;
        crc = crc32c_shift(crc32c_long, static_cast<uint32_t>(crc)) ^ crc2;
        buf += 2 * LONG_BLOCK;
        len -= 3 * LONG_BLOCK;
    }

    // process the data in 3 blocks and combine the crc's using the shift trick
    while (len >= (3 * SHORT_BLOCK)) {
        crc1 = 0;
        crc2 = 0;
        const uint8_t* end = buf + SHORT_BLOCK;
        do
        {
            crc  = crc32c_sw_inner(crc, buf);
            crc1 = crc32c_sw_inner(crc1, (buf + SHORT_BLOCK));
            crc2 = crc32c_sw_inner(crc2, (buf + (2 * SHORT_BLOCK)));
            buf += sizeof(uint64_t);
        } while (buf < end);
        crc = crc32c_shift(crc32c_short, static_cast<uint32_t>(crc)) ^ crc1;
        crc = crc32c_shift(crc32c_short, static_cast<uint32_t>(crc)) ^ crc2;
        buf += 2 * SHORT_BLOCK;
        len -= 3 * SHORT_BLOCK;
    }

    // swallow any remaining longs.
    while (len >= sizeof(uint64_t)) {
        crc = crc32c_sw_inner(crc, buf);
        buf += sizeof(uint64_t);
        len -= sizeof(uint64_t);
    }

    // swallow any remaining bytes.
    while (len > 0) {
        crc = crc32c_sw_lookup_table[0][(crc ^ *buf) & 0xff] ^ (crc >> 8);
        buf += sizeof(uint8_t);
        len -= sizeof(uint8_t);
    }

    return static_cast<uint32_t>(crc ^ std::numeric_limits<uint32_t>::max());
}



//
// Initialise tables for software and hardware functions.
//
bool setup_tables() {
    uint32_t crc = 0;
    for (int ii = 0; ii < TABLE_Y; ii++) {
        crc = ii;
        for (int jj = 0; jj < TABLE_X; jj++) {
            crc = crc & 1 ? (crc >> 1) ^ CRC32C_POLYNOMIAL_REV : crc >> 1;
        }
        crc32c_sw_lookup_table[0][ii] = crc;
    }

    for (int ii = 0; ii < TABLE_Y; ii++) {
        crc = crc32c_sw_lookup_table[0][ii];
        for (int jj = 1; jj < TABLE_X; jj++) {
            crc = crc32c_sw_lookup_table[jj][ii] =
                crc32c_sw_lookup_table[0][crc & 0xff] ^ (crc >> 8);
        }
    }

    crc32c_zeros(crc32c_long, LONG_BLOCK);
    crc32c_zeros(crc32c_short, SHORT_BLOCK);

    (void) tables_setup;
    return true;
}

typedef uint32_t (*crc32c_function)(const uint8_t* buf,
                                    size_t len,
                                    uint32_t crc_in);

//
// Return the appropriate function for the platform.
// If SSE4.2 is available then hardware acceleration is used.
//
static crc32c_function setup_crc32c() {
#if defined(__x86_64__) || defined(_M_X64) || defined(_M_IX86)
    const uint32_t SSE42 = 0x00100000;

    crc32c_function f = crc32c_sw;

#if defined(WIN32)
    std::array<int, 4> registers = {{0,0,0,0}};
    __cpuid(registers.data(), 1);
#else
    std::array<uint32_t, 4> registers = {{0,0,0,0}};
    __get_cpuid(1, &registers[0], &registers[1], &registers[2],&registers[3]);
#endif

    if (registers[2] & SSE42) {
        f = crc32c_hw;
    }

    return f;
#elif defined(__ARM_FEATURE_CRC32)
    return crc32c_hw;
#else
    // No hardware support
    return crc32c_sw;
#endif
}

static crc32c_function safe_crc32c = setup_crc32c();

//
// The exported crc32c method uses the function setup_crc32 decided
// is safe for the platform.
//
uint32_t crc32c (const uint8_t* buf, size_t len, uint32_t crc_in) {
    return safe_crc32c(buf, len, crc_in);
}
