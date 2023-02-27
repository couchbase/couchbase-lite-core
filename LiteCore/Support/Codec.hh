//
// Codec.hh
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "fleece/slice.hh"
#include "fleece/Fleece.hh"
#include "Logging.hh"
#include "slice_stream.hh"
#include <zlib.h>

namespace litecore { namespace blip {


    /** Abstract encoder/decoder class. */
    class Codec : protected Logging {
      public:
        using slice         = fleece::slice;
        using slice_ostream = fleece::slice_ostream;
        using slice_istream = fleece::slice_istream;

        Codec();
        virtual ~Codec() = default;

        // See https://zlib.net/manual.html#Basic for info about modes
        enum class Mode : int {
            Raw     = -1,  // not a zlib mode; means copy bytes w/o compression
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
        virtual void write(slice_istream &input, slice_ostream &output, Mode = Mode::Default) = 0;

        /** Number of bytes buffered in the codec that haven't been written to
            the output yet for lack of space. */
        virtual unsigned unflushedBytes() const { return 0; }

        static constexpr size_t kChecksumSize = 4;

        /** Writes the codec's current checksum to the output slice.
            This is a CRC32 checksum of all the unencoded data processed so far. */
        void writeChecksum(slice_ostream &output) const;

        /** Reads a checksum from the input slice and compares it with the codec's current one.
            If they aren't equal, throws an exception. */
        void readAndVerifyChecksum(slice_istream &input) const;

      protected:
        void addToChecksum(slice data);
        void _writeRaw(slice_istream &input, slice_ostream &output);

        uint32_t _checksum{0};
    };

    /** Abstract base class of Zlib-based codecs Deflater and Inflater */
    class ZlibCodec : public Codec {
      protected:
        using FlateFunc = int (*)(z_stream *, int);

        ZlibCodec(FlateFunc flate) : _flate(flate) {}

        void _write(const char *operation, slice_istream &input, slice_ostream &output, Mode,
                    size_t maxInput = SIZE_MAX);
        void check(int) const;

        mutable ::z_stream _z{};
        FlateFunc const    _flate;
    };

    /** Compressing codec that performs a zlib/gzip "deflate". */
    class Deflater final : public ZlibCodec {
      public:
        enum CompressionLevel : int8_t {
            NoCompression      = 0,
            FastestCompression = 1,
            BestCompression    = 9,
            DefaultCompression = -1,
        };

        Deflater(CompressionLevel = DefaultCompression);
        ~Deflater();

        void     write(slice_istream &input, slice_ostream &output, Mode = Mode::Default) override;
        unsigned unflushedBytes() const override;

      private:
        void _writeAndFlush(slice_istream &input, slice_ostream &output);
    };

    /** Decompressing codec that performs a zlib/gzip "inflate". */
    class Inflater final : public ZlibCodec {
      public:
        Inflater();
        ~Inflater();

        void write(slice_istream &input, slice_ostream &output, Mode = Mode::Default) override;
    };

}}  // namespace litecore::blip
