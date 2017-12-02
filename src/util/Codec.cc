//
//  Codec.cc
//  blip_cpp
//
//  Created by Jens Alfke on 11/16/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//


// For zlib API documentation, see: https://zlib.net/manual.html


#include "Codec.hh"
#include "Error.hh"
#include "Logging.hh"
#include "Endian.hh"
#include <algorithm>

namespace litecore { namespace blip {
    using namespace fleeceapi;
    using namespace fleece;


    // "The windowBits parameter is the base two logarithm of the window size (the size of the
    // history buffer)." 15 is the max, and the suggested default value.
    static constexpr int kZlibWindowSize = 15;

    // "The memLevel parameter specifies how much memory should be allocated for the internal
    // compression state." Default is 8; we bump it to 9, which uses 256KB.
    static constexpr int kZlibDeflateMemLevel = 9;


    LogDomain Zip("Zip", LogLevel::Warning);


    Codec::Codec()
    :Logging(Zip)
    { }

    void Codec::writeChecksum(slice &output) {
        uint32_t chk = _enc32(_checksum);
        output.writeFrom(slice(&chk, sizeof(chk)));
    }


    void Codec::readAndVerifyChecksum(slice &input) {
        if (input.size != 4)
            error::_throw(error::CorruptData, "BLIP message ends before checksum");
        uint32_t chk;
        input.readInto(slice(&chk, sizeof(chk)));
        chk = _dec32(chk);
        if (chk != _checksum)
            error::_throw(error::CorruptData, "BLIP message invalid checksum");
    }


    void ZlibCodec::check(int ret) const {
        if (ret < 0 && ret != Z_BUF_ERROR)
            error::_throw(error::CorruptData, "zlib error %d: %s",
                          ret, (_z.msg ? _z.msg : "???"));
    }


    void ZlibCodec::_write(const char *operation,
                           slice &input, slice &output,
                           Mode mode,
                           size_t maxInput)
    {
        _z.next_in = (Bytef*)input.buf;
        auto inSize = _z.avail_in = (unsigned)std::min(input.size, maxInput);
        _z.next_out = (Bytef*)output.buf;
        auto outSize = _z.avail_out = (unsigned)output.size;
        Assert(output.size > 0);
        int result = _flate(&_z, (int)mode);
        log("    %s(in %u, out %u, mode %d)-> %d; read %ld bytes, wrote %ld bytes",
            operation, inSize, outSize, mode, result,
            (_z.next_in - (uint8_t*)input.buf),
            (_z.next_out - (uint8_t*)output.buf));
        _checksum = (uint32_t)_z.adler;
        input.setStart(_z.next_in);
        output.setStart(_z.next_out);
        check(result);
    }


#pragma mark - DEFLATER:


    Deflater::Deflater(int level)
    :ZlibCodec(::deflate)
    {
        check(::deflateInit2(&_z, level, Z_DEFLATED,
                             kZlibWindowSize, kZlibDeflateMemLevel,
                             Z_DEFAULT_STRATEGY));
    }


    Deflater::~Deflater() {
        ::deflateEnd(&_z);
    }


    bool Deflater::write(slice &input, slice &output, Mode mode) {
        size_t origInputSize = input.size, origOutputSize = output.size;
        log("Compressing %zu bytes into %zu-byte buf", origInputSize, origOutputSize);

        if (mode == Mode::NoFlush) {
            _write("deflate", input, output, mode, input.size);
        } else {
            // If we try to write all of the input, and there isn't room in the output, the zlib
            // codec might end up with buffered data that hasn't been output yet (even though we
            // told it to flush.) To work around this, write the data gradually and stop before
            // the output fills up.
            Mode curMode = Mode::PartialFlush;
            while (input.size > 0) {
                if (output.size >= deflateBound(&_z, (unsigned)input.size))
                    curMode = mode;      // It's safe to flush, we know we have room to
                _write("deflate", input, output, curMode,
                       output.size);     // limit max amount of input to consume
                if (mode == Mode::Finish && output.size == 0)
                    return false;
                if (output.size < 1024)
                    break;
            }

            if (curMode != mode && output.size > 0) {
                // Flush if we haven't yet:
                _write("deflate", input, output, mode, 0);
            }
        }

        unsigned unflushed = unflushedBytes();
        log("    compressed %zu bytes to %zu (%.0f%%), %u unflushed",
            (origInputSize-input.size), (origOutputSize-output.size),
            (origOutputSize-output.size) * 100.0 / (origInputSize-input.size), unflushed);
        return unflushed == 0;
    }


    unsigned Deflater::unflushedBytes() const {
        unsigned bytes;
        int bits;
        check(deflatePending(const_cast<z_stream*>(&_z), &bytes, &bits));
        return bytes + (bits > 0);
    }


#pragma mark - INFLATER:


    Inflater::Inflater()
    :ZlibCodec(::inflate)
    {
        // 32 is added "to enable zlib and gzip decoding with automatic header detection"
        check(::inflateInit2(&_z, kZlibWindowSize + 32));
    }


    Inflater::~Inflater() {
        ::inflateEnd(&_z);
    }


    bool Inflater::write(slice &input, slice &output, Mode mode) {
        log("Decompressing %zu bytes into %zu-byte buf",
            input.size, output.size);
        auto outStart = (uint8_t*)output.buf;
        _write("inflate", input, output, mode, input.size);
        logVerbose("    decompressed %zu bytes: %.*s",
                   ((uint8_t*)output.buf - outStart),
                   (int)((uint8_t*)output.buf - outStart), outStart);
        return input.size == 0;
    }


#pragma mark - NOFLATER:


    Noflater::Noflater() {
        // Initialize _checksum to required initial value for Adler32:
        _checksum = (uint32_t)adler32(0, nullptr, 0);
    }


    bool Noflater::write(slice &input, slice &output, Mode mode) {
        Assert(output.size > 0);
        size_t count = std::min(input.size, output.size);

        _checksum = (uint32_t)adler32(_checksum, (const Bytef*)input.buf, (int)count);

        memcpy((void*)output.buf, input.buf, count);
        input.moveStart(count);
        output.moveStart(count);
        // Return false if not enough output space to finish writing
        return mode != Mode::Finish || input.size == 0;
    }

} }
