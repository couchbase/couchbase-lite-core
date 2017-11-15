//
//  Flater.hh
//  blip_cpp
//
//  Created by Jens Alfke on 11/16/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "slice.hh"
#include "FleeceCpp.hh"
#include <zlib.h>

namespace litecore { namespace blip {

    class Flater {
    public:
        enum class Mode : int {
            NoFlush = 0,
            PartialFlush,
            SyncFlush,
            FullFlush,
            Finish,
            Block,
            Trees
        };

    protected:
        void check(int);

        z_stream _z { };
    };


    /** Gzip/Deflate compressor. */
    class Deflater : public Flater {
    public:
        Deflater(int level =Z_DEFAULT_COMPRESSION);
        ~Deflater();

        /** Reads data from `input` and writes compressed data to `output`.
            Each slice's buf pointer is moved forwards past the consumed data.
            Returns false if the mode is Finish and there isn't enough room to write all data. */
        bool deflate(fleece::slice &input, fleece::slice &output, Mode =Mode::Finish);
    };


    /** Gzip/Deflate decompressor, that writes the output to a JSONEncoder. */
    class Inflater : public Flater {
    public:
        Inflater(fleeceapi::JSONEncoder &writer);
        ~Inflater();

        void write(fleece::slice compressedData, bool finished);

        bool eof() const    {return _eof;}

    private:
        void inflate(fleece::slice &input, fleece::slice &output, Mode);

        fleeceapi::JSONEncoder& _writer;
        bool _eof {false};
    };

} }
