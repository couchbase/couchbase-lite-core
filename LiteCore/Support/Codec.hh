//
// Codec.hh
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

#pragma once
#include "fleece/slice.hh"
#include "fleece/Fleece.hh"
#include "Logging.hh"
#include <zlib.h>

namespace litecore { namespace blip {


    /** Abstract encoder/decoder class. */
    class Codec : protected Logging {
    public:
        using slice = fleece::slice;

        Codec();
        virtual ~Codec() =default;

        // See https://zlib.net/manual.html#Basic for info about modes
        enum class Mode : int {
            Raw = -1,               // not a zlib mode; means copy bytes w/o compression
            NoFlush = 0,
            PartialFlush,
            SyncFlush,
            FullFlush,
            Finish,
            Block,
            Trees,

            Default = SyncFlush
        };

        /** Reads data from `input` and writes transformed data to `output`.
            Each slice's buf pointer is moved forwards past the consumed data. */
        virtual void write(slice &input,
                           slice &output,
                           Mode =Mode::Default) =0;

        /** Number of bytes buffered in the codec that haven't been written to
            the output yet for lack of space. */
        virtual unsigned unflushedBytes() const         {return 0;}

        static constexpr size_t kChecksumSize = 4;

        /** Writes the codec's current checksum to the output slice.
            This is a CRC32 checksum of all the unencoded data processed so far. */
        void writeChecksum(slice &output) const;

        /** Reads a checksum from the input slice and compares it with the codec's current one.
            If they aren't equal, throws an exception. */
        void readAndVerifyChecksum(slice &input) const;

    protected:
        void addToChecksum(slice data);
        void _writeRaw(slice &input, slice &output);

        uint32_t _checksum {0};
    };


    /** Abstract base class of Zlib-based codecs Deflater and Inflater */
    class ZlibCodec : public Codec {
    protected:
        using FlateFunc = int (*)(z_stream*, int);

        ZlibCodec(FlateFunc flate)
        :_flate(flate)
        { }

        void _write(const char *operation, slice &input, slice &output,
                   Mode, size_t maxInput =SIZE_MAX);
        void check(int) const;

        mutable ::z_stream _z { };
        FlateFunc const _flate;
    };


    /** Compressing codec that performs a zlib/gzip "deflate". */
    class Deflater final : public ZlibCodec {
    public:
        enum CompressionLevel : int8_t {
            NoCompression       =  0,
            FastestCompression  =  1,
            BestCompression     =  9,
            DefaultCompression  = -1,
        };
        Deflater(CompressionLevel = DefaultCompression);
        ~Deflater();

        void write(slice &input, slice &output, Mode =Mode::Default) override;
        unsigned unflushedBytes() const override;

    private:
        void _writeAndFlush(slice &input, slice &output);
    };


    /** Decompressing codec that performs a zlib/gzip "inflate". */
    class Inflater final : public ZlibCodec {
    public:
        Inflater();
        ~Inflater();

        void write(slice &input, slice &output, Mode =Mode::Default) override;
    };

} }
