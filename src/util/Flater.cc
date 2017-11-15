//
//  Flater.cc
//  blip_cpp
//
//  Created by Jens Alfke on 11/16/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "Flater.hh"
#include "Error.hh"
#include "Logging.hh"

namespace litecore { namespace blip {
    using namespace fleeceapi;
    using namespace fleece;


    LogDomain Zip("Zip");


    // For zlib API documentation, see: https://zlib.net/manual.html


    void Flater::check(int ret) {
        if (ret < 0)
            error::_throw(error::CorruptData, "zlib error %d: %s",
                          ret, (_z.msg ? _z.msg : "???"));
    }


#pragma mark - DEFLATER:


    Deflater::Deflater(int level) {
        check(::deflateInit2(&_z, level, Z_DEFLATED, 15+16, 8, Z_DEFAULT_STRATEGY));
    }


    Deflater::~Deflater() {
        ::deflateEnd(&_z);
    }


    bool Deflater::deflate(slice &input, slice &output, Mode mode) {
        LogTo(Zip, "Compressing %zu bytes into %zu-byte buffer (mode=%d)",
              input.size, output.size, (int)mode);
        LogToAt(Zip, Verbose, "    compressing: %.*s", (int)input.size, input.buf);
        _z.next_in = (uint8_t*)input.buf;
        _z.avail_in = (uInt)input.size;
        _z.next_out = (uint8_t*)output.buf;
        _z.avail_out = (uInt)output.size;
        int result = ::deflate(&_z, (int)mode);
        LogTo(Zip, "    deflate-> %d; read %ld bytes, wrote %ld bytes",
            result, (_z.next_in - (uint8_t*)input.buf),
            (_z.next_out - (uint8_t*)output.buf));
        check(result);

        input.setStart(_z.next_in);
        output.setStart(_z.next_out);
        if (mode == Mode::Finish && result != Z_STREAM_END)
            return false;   // Not enough output space to finish writing
        return true;
    }


#pragma mark - INFLATER:


    Inflater::Inflater(JSONEncoder &writer)
    :_writer(writer)
    {
        // 15: log2 of window size. (15 is the suggested default value.)
        // 32: Added to windowBits to enable zlib and gzip decoding with automatic header detection
        check(::inflateInit2(&_z, 15 + 32));
    }


    Inflater::~Inflater() {
        ::inflateEnd(&_z);
    }


    void Inflater::inflate(slice &input, slice &output, Mode mode) {
        Assert(output.size > 0);
        _z.next_in = (Bytef*)input.buf;
        _z.avail_in = (unsigned)input.size;
        _z.next_out = (Bytef*)output.buf;
        _z.avail_out = (unsigned)output.size;
        int result = ::inflate(&_z, (int)mode);
        LogTo(Zip, "    inflate-> %d; read %ld bytes, wrote %ld bytes",
              result, (_z.next_in - (uint8_t*)input.buf),
              (_z.next_out - (uint8_t*)output.buf));
        input.setStart(_z.next_in);
        output.setStart(_z.next_out);

        if (result == Z_STREAM_END)
            _eof = true;
        else if (result == Z_BUF_ERROR && mode == Mode::Finish && output.size == 0)
            ; // this is ok; just means output needs to be flushed before finishing
        else
            check(result);
    }


    void Inflater::write(fleece::slice compressedData, bool finished) {
        LogTo(Zip, "Decompressing %ld bytes%s",
              compressedData.size, (finished ? " (finished)" : ""));
        uint8_t outBuf[4096];
        while (compressedData.size > 0) {
            slice output {outBuf, sizeof(outBuf)};
            inflate(compressedData, output, (finished ? Mode::Finish : Mode::NoFlush));

            // Write output to Writer:
            if (output.buf > outBuf) {
                LogToAt(Zip, Verbose, "    decompressed: %.*s", (int)(_z.next_out - outBuf), outBuf);
                _writer.writeRaw(slice(outBuf, output.buf));
            }

            if (_eof) {
                if (compressedData.size > 0)
                    Warn("Inflater didn't read all the input data (%zu bytes left)",
                         compressedData.size);
                break;
            }
        }
    }


} }
