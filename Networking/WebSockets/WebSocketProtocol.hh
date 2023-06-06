//
// WebSocketProtocol.hh
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//
//  Originally by Alex Hultman et al, from https://github.com/uWebSockets/uWebSockets
//  Copied from Git commit 2d7faa65270172daeb79b2616a1da82295b98007
//  Original license:
//      Copyright (c) 2016 Alex Hultman and contributors
//
//      This software is provided 'as-is', without any express or implied
//      warranty. In no event will the authors be held liable for any damages
//      arising from the use of this software.
//
//      Permission is granted to anyone to use this software for any purpose,
//      including commercial applications, and to alter it and redistribute it
//      freely, subject to the following restrictions:
//
//      1. The origin of this software must not be misrepresented; you must not
//         claim that you wrote the original software. If you use this software
//         in a product, an acknowledgement in the product documentation would be
//         appreciated but is not required.
//      2. Altered source versions must be plainly marked as such, and must not be
//         misrepresented as being the original software.
//      3. This notice may not be removed or altered from any source distribution.

#ifndef WEBSOCKETPROTOCOL_UWS_H
#define WEBSOCKETPROTOCOL_UWS_H

//COUCHBASE: Copied definitions of endian functions here, from original Networking.h
// #include "Networking.h"
#include <limits>
#ifdef __APPLE__
#    include <libkern/OSByteOrder.h>
#    define htobe64(x) OSSwapHostToBigInt64(x)
#    define be64toh(x) OSSwapBigToHostInt64(x)
#elif defined(_WIN32)
#    ifndef NOMINMAX
#        define NOMINMAX
#    endif
#    include <WinSock2.h>
#    include <Ws2tcpip.h>
#    ifdef __MINGW32__
// Windows has always been tied to LE
#        define htobe64(x) __builtin_bswap64(x)
#        define be64toh(x) __builtin_bswap64(x)
#    else
#        define htobe64(x) htonll(x)
#        define be64toh(x) ntohll(x)
#    endif
#else
#    ifndef _BSD_SOURCE
#        define _BSD_SOURCE
#    endif
#    include <endian.h>
#    include <arpa/inet.h>
#endif
//COUCHBASE: End of code adapted from Networking.h

#include <algorithm>
#include <cstring>
#include <cstdlib>
#include "SecureRandomize.hh"

namespace uWS {

    enum OpCode : uint8_t { TEXT = 1, BINARY = 2, CLOSE = 8, PING = 9, PONG = 10 };

    enum { CLIENT, SERVER };

    template <const bool isServer>
    class WebSocketProtocol {
      public:
        static constexpr int SHORT_MESSAGE_HEADER  = isServer ? 6 : 2;
        static constexpr int MEDIUM_MESSAGE_HEADER = isServer ? 8 : 4;
        static constexpr int LONG_MESSAGE_HEADER   = isServer ? 14 : 10;

      private:
        typedef uint16_t frameFormat;

        static inline bool isFin(frameFormat& frame) { return frame & 128; }

        static inline unsigned char getOpCode(frameFormat& frame) { return frame & 15; }

        static inline unsigned char payloadLength(frameFormat& frame) { return (frame >> 8) & 127; }

        static inline bool rsv23(frameFormat& frame) { return frame & 48; }

        static inline bool rsv1(frameFormat& frame) { return frame & 64; }

        static inline bool getMask(frameFormat& frame) { return frame & 32768; }

        static inline void unmaskPrecise(std::byte* dst, std::byte* src, std::byte* mask, size_t length) {
            for ( ; length >= 4; length -= 4 ) {
                *(dst++) = *(src++) ^ mask[0];
                *(dst++) = *(src++) ^ mask[1];
                *(dst++) = *(src++) ^ mask[2];
                *(dst++) = *(src++) ^ mask[3];
            }
            for ( ; length > 0; --length ) *(dst++) = *(src++) ^ *(mask++);
        }

        static inline void unmaskPreciseCopyMask(std::byte* dst, std::byte* src, std::byte* maskPtr, size_t length) {
            std::byte mask[4] = {maskPtr[0], maskPtr[1], maskPtr[2], maskPtr[3]};
            unmaskPrecise(dst, src, mask, length);
        }

        static inline void rotateMask(size_t offset, std::byte* mask) {
            std::byte originalMask[4] = {mask[0], mask[1], mask[2], mask[3]};
            mask[(0 + offset) % 4]    = originalMask[0];
            mask[(1 + offset) % 4]    = originalMask[1];
            mask[(2 + offset) % 4]    = originalMask[2];
            mask[(3 + offset) % 4]    = originalMask[3];
        }

        static inline void unmaskInplace(std::byte* data, std::byte* stop, std::byte* mask) {
            std::byte* stop1 = stop - 3;
            while ( data < stop1 ) {
                *(data++) ^= mask[0];
                *(data++) ^= mask[1];
                *(data++) ^= mask[2];
                *(data++) ^= mask[3];
            }
            while ( data < stop ) *(data++) ^= *(mask++);
        }

        enum state_t : uint8_t { READ_HEAD, READ_MESSAGE };

        enum send_state_t : uint8_t { SND_CONTINUATION = 1, SND_NO_FIN = 2, SND_COMPRESSED = 64 };

        template <const int MESSAGE_HEADER, typename T>
        inline bool consumeMessage(T payLength, std::byte*& src, size_t& length, frameFormat frame, void* user) {
            if ( getOpCode(frame) ) {
                if ( opStack == 1 || (!lastFin && getOpCode(frame) < 2) ) {
                    forceClose(user);
                    return true;
                }
                opCode[++opStack] = (OpCode)getOpCode(frame);
            } else if ( opStack == -1 ) {
                forceClose(user);
                return true;
            }
            lastFin = isFin(frame);

            if ( payLength > SIZE_MAX || refusePayloadLength(user, payLength) ) {
                forceClose(user);
                return true;
            }

            if ( int(payLength) <= int(length - MESSAGE_HEADER) ) {
                if ( isServer ) {
                    unmaskPreciseCopyMask(src, src + MESSAGE_HEADER, src + MESSAGE_HEADER - 4, payLength);
                    if ( handleFragment(src, (size_t)payLength, 0, opCode[opStack], isFin(frame), user) ) {
                        return true;
                    }
                } else {
                    if ( handleFragment(src + MESSAGE_HEADER, (size_t)payLength, 0, opCode[opStack], isFin(frame),
                                        user) ) {
                        return true;
                    }
                }

                if ( isFin(frame) ) { opStack--; }

                src += payLength + MESSAGE_HEADER;
                length -= payLength + MESSAGE_HEADER;
                spillLength = 0;
                return false;
            } else {
                spillLength    = 0;
                state          = READ_MESSAGE;
                remainingBytes = payLength + MESSAGE_HEADER - length;

                if ( isServer ) {
                    memcpy(mask, src + MESSAGE_HEADER - 4, 4);
                    unmaskPrecise(src, src + MESSAGE_HEADER, mask, length - MESSAGE_HEADER);
                    rotateMask(4 - (length - MESSAGE_HEADER) % 4, mask);
                } else {
                    src += MESSAGE_HEADER;
                }
                handleFragment(src, length - MESSAGE_HEADER, remainingBytes, opCode[opStack], isFin(frame), user);
                return true;
            }
        }

        inline bool consumeContinuation(std::byte*& src, size_t& length, void* user) {
            if ( remainingBytes <= length ) {
                if ( isServer ) {
                    int n = remainingBytes >> 2;
                    unmaskInplace(src, src + n * 4, mask);
                    for ( int i = 0, s = remainingBytes % 4; i < s; i++ ) { src[n * 4 + i] ^= mask[i]; }
                }

                if ( handleFragment(src, remainingBytes, 0, opCode[opStack], lastFin, user) ) { return false; }

                if ( lastFin ) { opStack--; }

                src += remainingBytes;
                length -= remainingBytes;
                state = READ_HEAD;
                return true;
            } else {
                if ( isServer ) { unmaskInplace(src, src + length, mask); }

                remainingBytes -= length;
                if ( handleFragment(src, length, remainingBytes, opCode[opStack], lastFin, user) ) { return false; }

                if ( isServer && length % 4 ) { rotateMask(4 - (length % 4), mask); }
                return false;
            }
        }

        // this can hold two states (1 bit)
        // this can hold length of spill (up to 16 = 4 bit)
        state_t   state       = READ_HEAD;
        size_t    spillLength = 0;     // remove this!
        int       opStack     = -1;    // remove this too
        bool      lastFin     = true;  // hold in state!
        std::byte spill[LONG_MESSAGE_HEADER - 1]{};
        size_t    remainingBytes =
                0;  // denna kan hålla spillLength om state är READ_HEAD, och remainingBytes när state är annat?
        std::byte mask[isServer ? 4 : 1]{};
        OpCode    opCode[2]{};

      public:
        WebSocketProtocol() = default;

        // Based on utf8_check.c by Markus Kuhn, 2005
        // https://www.cl.cam.ac.uk/~mgk25/ucs/utf8_check.c
        static bool isValidUtf8(unsigned char* s, size_t length) {
            for ( unsigned char* e = s + length; s != e; ) {
                while ( !(*s & 0x80) ) {
                    if ( ++s == e ) { return true; }
                }

                if ( (s[0] & 0x60) == 0x40 ) {
                    if ( s + 1 >= e || (s[1] & 0xc0) != 0x80 || (s[0] & 0xfe) == 0xc0 ) { return false; }
                    s += 2;
                } else if ( (s[0] & 0xf0) == 0xe0 ) {
                    if ( s + 2 >= e || (s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80
                         || (s[0] == 0xe0 && (s[1] & 0xe0) == 0x80) || (s[0] == 0xed && (s[1] & 0xe0) == 0xa0) ) {
                        return false;
                    }
                    s += 3;
                } else if ( (s[0] & 0xf8) == 0xf0 ) {
                    if ( s + 3 >= e || (s[1] & 0xc0) != 0x80 || (s[2] & 0xc0) != 0x80 || (s[3] & 0xc0) != 0x80
                         || (s[0] == 0xf0 && (s[1] & 0xf0) == 0x80) || (s[0] == 0xf4 && s[1] > 0x8f) || s[0] > 0xf4 ) {
                        return false;
                    }
                    s += 4;
                } else {
                    return false;
                }
            }
            return true;
        }

        struct CloseFrame {
            uint16_t   code;
            std::byte* message;
            size_t     length;
        };

        static inline CloseFrame parseClosePayload(std::byte* src, size_t length) {
            CloseFrame cf = {};
            if ( length >= 2 ) {
                memcpy(&cf.code, src, 2);
                cf = {ntohs(cf.code), src + 2, length - 2};
                if ( cf.code < 1000 || cf.code > 4999 || (cf.code > 1011 && cf.code < 4000)
                     || (cf.code >= 1004 && cf.code <= 1006) || !isValidUtf8((unsigned char*)cf.message, cf.length) ) {
                    return {};
                }
            }
            return cf;
        }

        static inline size_t formatClosePayload(std::byte* dst, uint16_t code, const char* message, size_t length) {
            if ( code ) {
                code = htons(code);
                memcpy(dst, &code, 2);
                if ( length > 0 )  // COUCHBASE: Avoid UB when message==NULL and length==0
                    memcpy(dst + 2, message, length);
                return length + 2;
            }
            return 0;
        }

        static inline size_t formatMessage(std::byte* dst, const char* src, size_t length, OpCode opCode,
                                           size_t reportedLength, bool compressed) {
            size_t messageLength;
            size_t headerLength;
            if ( reportedLength < 126 ) {
                headerLength = 2;
                dst[1]       = (std::byte)reportedLength;
            } else if ( reportedLength <= std::numeric_limits<uint16_t>::max() ) {
                headerLength          = 4;
                dst[1]                = (std::byte)126;
                *((uint16_t*)&dst[2]) = htons((uint16_t)reportedLength);
            } else {
                headerLength          = 10;
                dst[1]                = (std::byte)127;
                *((uint64_t*)&dst[2]) = htobe64(reportedLength);
            }

            int flags = 0;
            dst[0]    = (std::byte)((flags & SND_NO_FIN ? 0 : 128) | (compressed ? SND_COMPRESSED : 0));
            if ( !(flags & SND_CONTINUATION) ) { dst[0] |= (std::byte)opCode; }

            std::byte mask[4];
            if ( !isServer ) {
                ((uint8_t*)dst)[1] |= 0x80;
                uint32_t random = litecore::RandomNumber();
                memcpy(mask, &random, 4);
                memcpy(dst + headerLength, &random, 4);
                headerLength += 4;
            }

            messageLength = headerLength + length;
            memcpy(dst + headerLength, src, length);

            if ( !isServer ) {
                uint32_t mask_i32 = *reinterpret_cast<uint32_t*>(mask);

                auto*     start = reinterpret_cast<uint32_t*>(dst + headerLength);
                uint32_t* end   = start + length / 4;

                while ( start != end ) { *start++ ^= mask_i32; }

                // Handle remaining bytes (if length is not a multiple of 4)
                auto*      start_byte = reinterpret_cast<std::byte*>(start);
                std::byte* end_byte   = dst + headerLength + length;
                for ( int i = 0; start_byte != end_byte; ++i, ++start_byte ) { *start_byte ^= mask[i % 4]; }
            }
            return messageLength;
        }

        void consume(std::byte* src, size_t length, void* user) {
            while ( spillLength > 0 ) {
                // Use up any unread bytes, without letting _consume do it, because it will copy them
                // before `src`, causing memory corruption or crashes. (#531)
                std::byte buf[LONG_MESSAGE_HEADER];
                size_t    bufLen     = std::min(spillLength + length, sizeof(buf));
                size_t    lengthUsed = bufLen - spillLength;
                memcpy(buf, spill, spillLength);
                memcpy(buf + spillLength, src, lengthUsed);
                spillLength = 0;
                src += lengthUsed;
                length -= lengthUsed;
                _consume(buf, bufLen, user);
                if ( length == 0 ) return;
            }
            _consume(src, length, user);
        }

        void _consume(std::byte* src, size_t length, void* user) {
            if ( spillLength ) {
                src -= spillLength;
                length += spillLength;
                memcpy(src, spill, spillLength);
            }
            if ( state == READ_HEAD ) {
            parseNext:
                for ( frameFormat frame; length >= SHORT_MESSAGE_HEADER; ) {
                    memcpy(&frame, src, sizeof(frameFormat));

                    // invalid reserved bits / invalid opcodes / invalid control frames / set compressed frame
                    if ( (rsv1(frame) && !setCompressed(user)) || rsv23(frame)
                         || (getOpCode(frame) > 2 && getOpCode(frame) < 8) || getOpCode(frame) > 10
                         || (getOpCode(frame) > 2 && (!isFin(frame) || payloadLength(frame) > 125)) ) {
                        forceClose(user);
                        return;
                    }

                    if ( payloadLength(frame) < 126 ) {
                        if ( consumeMessage<SHORT_MESSAGE_HEADER, uint8_t>(payloadLength(frame), src, length, frame,
                                                                           user) ) {
                            return;
                        }
                    } else if ( payloadLength(frame) == 126 ) {
                        if ( length < MEDIUM_MESSAGE_HEADER ) { break; }
                        uint16_t n;
                        memcpy(&n, &src[2], sizeof(n));
                        if ( consumeMessage<MEDIUM_MESSAGE_HEADER, uint16_t>(ntohs(n), src, length, frame, user) ) {
                            return;
                        }
                    } else {
                        if ( length < LONG_MESSAGE_HEADER ) { break; }
                        uint64_t n;
                        memcpy(&n, &src[2], sizeof(n));
                        if ( consumeMessage<LONG_MESSAGE_HEADER, uint64_t>(be64toh(n), src, length, frame, user) ) {
                            return;
                        }
                    }
                }
                if ( length ) {
                    memcpy(spill, src, length);
                    spillLength = length;
                }
            } else if ( consumeContinuation(src, length, user) ) {
                goto parseNext;
            }
        }

        static constexpr int CONSUME_POST_PADDING = 18;
        static constexpr int CONSUME_PRE_PADDING  = LONG_MESSAGE_HEADER - 1;

        // events to be implemented by application (can't be inline currently)
        bool refusePayloadLength(void* user, int length);
        bool setCompressed(void* user);
        void forceClose(void* user);
        bool handleFragment(std::byte* data, size_t length, size_t remainingByteCount, uint8_t opcode, bool fin,
                            void* user);
    };

}  // namespace uWS

#endif  // WEBSOCKETPROTOCOL_UWS_H
