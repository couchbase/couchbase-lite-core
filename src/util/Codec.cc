//
//  Flater.cc
//  blip_cpp
//
//  Created by Jens Alfke on 11/16/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Codec.hh"
#include "Error.hh"
#include "Logging.hh"
#include <algorithm>

namespace litecore { namespace blip {
    using namespace fleeceapi;
    using namespace fleece;


    LogDomain Zip("Zip", LogLevel::Warning);


    // For zlib API documentation, see: https://zlib.net/manual.html


    void Codec::check(int ret) const {
        if (ret < 0 && ret != Z_BUF_ERROR)
            error::_throw(error::CorruptData, "zlib error %d: %s",
                          ret, (_z.msg ? _z.msg : "???"));
    }


    int Codec::_write(const char *operation, slice &input, slice &output, Mode mode,
                      size_t maxInput) {
        _z.next_in = (Bytef*)input.buf;
        auto inSize = _z.avail_in = (unsigned)std::min(input.size, maxInput);
        _z.next_out = (Bytef*)output.buf;
        auto outSize = _z.avail_out = (unsigned)output.size;
        Assert(output.size > 0);
        int result = _flate(&_z, (int)mode);
        LogTo(Zip, "    %s(in %u, out %u, mode %d)-> %d; read %ld bytes, wrote %ld bytes",
              operation, inSize, outSize, mode, result,
              (_z.next_in - (uint8_t*)input.buf),
              (_z.next_out - (uint8_t*)output.buf));
        input.setStart(_z.next_in);
        output.setStart(_z.next_out);
        return result;
    }


    bool Nullflater::write(slice &input, slice &output, Mode mode) {
        Assert(output.size > 0);
        size_t count = std::min(input.size, output.size);
        memcpy((void*)output.buf, input.buf, count);
        input.moveStart(count);
        output.moveStart(count);
        if (mode == Mode::Finish && input.size > 0)
            return false;   // Not enough output space to finish writing
        return true;
    }


#pragma mark - DEFLATER:


    Deflater::Deflater(int level)
    :Codec(::deflate)
    {
        check(::deflateInit2(&_z, level, Z_DEFLATED, 15+16, 8, Z_DEFAULT_STRATEGY));
    }


    Deflater::~Deflater() {
        ::deflateEnd(&_z);
    }


    bool Deflater::write(slice &input, slice &output, Mode mode) {
        LogToAt(Zip, Info, "Compressing %zu bytes into %zu-byte buf",
                input.size, output.size/*, (int)input.size, input.buf*/);
        Mode curMode = Mode::PartialFlush;
        while (input.size > 0 && output.size > 0) {
            if (output.size >= deflateBound(&_z, (unsigned)input.size))
                curMode = mode;
            check(_write("deflate", input, output, curMode,
                         output.size));     // max amount of input to consume
            if (mode == Mode::Finish && output.size == 0)
                return false;
            if (output.size < 1024)
                break;
        }

        if (curMode != mode && output.size > 0)
            check(_write("deflate", input, output, mode, 0));

        return unflushedBytes() == 0;
    }


    unsigned Deflater::unflushedBytes() const {
        unsigned bytes;
        int bits;
        check(deflatePending(const_cast<z_stream*>(&_z), &bytes, &bits));
        return bytes + (bits > 0);
    }


#pragma mark - INFLATER:


    Inflater::Inflater()
    :Codec(::inflate)
    {
        // 15: log2 of window size. (15 is the suggested default value.)
        // 32: Added to windowBits to enable zlib and gzip decoding with automatic header detection
        check(::inflateInit2(&_z, 15 + 32));
    }


    Inflater::~Inflater() {
        ::inflateEnd(&_z);
    }


    bool Inflater::write(slice &input, slice &output, Mode mode) {
        LogToAt(Zip, Info, "Decompressing %zu bytes into %zu-byte buf",
                input.size, output.size);
        auto outStart = (uint8_t*)output.buf;
        int result = _write("inflate", input, output, mode, input.size);
        LogToAt(Zip, Verbose, "    decompressed %zu bytes: %.*s",
                ((uint8_t*)output.buf - outStart),
                (int)((uint8_t*)output.buf - outStart), outStart);
        check(result);
        if (result == Z_STREAM_END)
            _eof = true;
        return true;
    }

} }
