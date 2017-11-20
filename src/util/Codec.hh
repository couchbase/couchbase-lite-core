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


    /** Abstract encoder/decoder class; base class of Inflater and Deflater. */
    class Codec {
    public:
        virtual ~Codec() { }

        // See https://zlib.net/manual.html#Basic for info about modes
        enum class Mode : int {
            NoFlush = 0,
            PartialFlush,
            SyncFlush,
            FullFlush,
            Finish,
            Block,
            Trees
        };

        /** Reads data from `input` and writes transformed data to `output`.
            Each slice's buf pointer is moved forwards past the consumed data.
            Returns false if it was unable to avoid leaving data inside the codec's buffer;
            in this case `write` needs to be called again (with empty input) to complete
            the write. */
        virtual bool write(fleece::slice &input,
                           fleece::slice &output,
                           Mode =Mode::SyncFlush) =0;

    protected:
        using FlateFunc = int (*)(z_stream*, int);

        Codec(FlateFunc flate)
        :_flate(flate)
        { }

        int _write(const char *operation, fleece::slice &input, fleece::slice &output,
                   Mode, size_t maxInput);
        void check(int) const;

        ::z_stream _z { };
        FlateFunc const _flate;
    };


    /** Compressing codec that performs a zlib/gzip "deflate". */
    class Deflater : public Codec {
    public:
        Deflater(int level =Z_DEFAULT_COMPRESSION);
        ~Deflater();

        bool write(fleece::slice &input, fleece::slice &output, Mode =Mode::SyncFlush) override;

        unsigned unflushedBytes() const;
    };


    /** Decompressing codec that performs a zlib/gzip "inflate". */
    class Inflater : public Codec {
    public:
        Inflater();
        ~Inflater();

        bool write(fleece::slice &input, fleece::slice &output, Mode =Mode::SyncFlush) override;

        bool eof() const    {return _eof;}

    private:
        bool _eof {false};
    };


    /** No-op codec that just copies without compressing. */
    class Nullflater : public Codec {
    public:
        Nullflater() :Codec(nullptr) { }
        bool write(fleece::slice &input, fleece::slice &output, Mode =Mode::SyncFlush) override;
    };

} }
