//
// Codec.cc
//
// Copyright 2017-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//


// For zlib API documentation, see: https://zlib.net/manual.html


#include "Codec.hh"
#include "Error.hh"
#include "Logging.hh"
#include "Endian.hh"
#include <algorithm>
#include <mutex>

namespace litecore { namespace blip {
    using namespace fleece;


    // "The windowBits parameter is the base two logarithm of the window size (the size of the
    // history buffer)." 15 is the max, and the suggested default value.
    static constexpr int kZlibWindowSize = 15;

    // True to use raw DEFLATE format, false to add the zlib header & checksum
    static constexpr bool kZlibRawDeflate = true;

    // "The memLevel parameter specifies how much memory should be allocated for the internal
    // compression state." Default is 8; we bump it to 9, which uses 256KB.
    static constexpr int kZlibDeflateMemLevel = 9;


    LogDomain Zip("Zip", LogLevel::Warning);

    Codec::Codec()
        : Logging(Zip)
        , _checksum((uint32_t)crc32(0, nullptr, 0))  // the required initial value
    {}

    void Codec::addToChecksum(slice data) {
        _checksum = (uint32_t)crc32(_checksum, (const Bytef*)data.buf, (int)data.size);
    }

    void Codec::writeChecksum(slice_ostream& output) const {
        uint32_t chk = endian::enc32(_checksum);
        Assert(output.write(&chk, sizeof(chk)));
    }

    void Codec::readAndVerifyChecksum(slice_istream& input) const {
        if ( input.size < kChecksumSize ) error::_throw(error::CorruptData, "BLIP message ends before checksum");
        uint32_t chk;
        static_assert(kChecksumSize == sizeof(chk), "kChecksumSize is wrong");
        input.readAll(&chk, sizeof(chk));
        chk = endian::dec32(chk);
        if ( chk != _checksum ) error::_throw(error::CorruptData, "BLIP message invalid checksum");
    }

    // Uncompressed write: just copies input bytes to output (updating checksum)
    void Codec::_writeRaw(slice_istream& input, slice_ostream& output) {
        logInfo("Copying %zu bytes into %zu-byte buf (no compression)", input.size, output.capacity());
        Assert(output.capacity() > 0);
        size_t count = std::min(input.size, output.capacity());
        addToChecksum({input.buf, count});
        output.write(input.buf, count);
        input.skip(count);
    }

    void ZlibCodec::check(int ret) const {
        if ( ret < 0 && ret != Z_BUF_ERROR )
            error::_throw(error::CorruptData, "zlib error %d: %s", ret, (_z.msg ? _z.msg : "???"));
    }

    void ZlibCodec::_write(const char* operation, slice_istream& input, slice_ostream& output, Mode mode,
                           size_t maxInput) {
        _z.next_in  = (Bytef*)input.buf;
        auto inSize = _z.avail_in = (unsigned)std::min(input.size, maxInput);
        _z.next_out               = (Bytef*)output.next();
        auto outSize = _z.avail_out = (unsigned)output.capacity();
        Assert(outSize > 0);
        Assert(mode > Mode::Raw);
        int result = _flate(&_z, (int)mode);
        logInfo("    %s(in %u, out %u, mode %d)-> %d; read %ld bytes, wrote %ld bytes", operation, inSize, outSize,
                (int)mode, result, (long)(_z.next_in - (uint8_t*)input.buf),
                (long)(_z.next_out - (uint8_t*)output.next()));
        if ( !kZlibRawDeflate ) _checksum = (uint32_t)_z.adler;
        input.setStart(_z.next_in);
        output.advanceTo(_z.next_out);
        check(result);
    }

#pragma mark - DEFLATER:

    Deflater::Deflater(CompressionLevel level) : ZlibCodec(::deflate) {
        check(::deflateInit2(&_z, level, Z_DEFLATED, kZlibWindowSize * (kZlibRawDeflate ? -1 : 1), kZlibDeflateMemLevel,
                             Z_DEFAULT_STRATEGY));
    }

    Deflater::~Deflater() { ::deflateEnd(&_z); }

    void Deflater::write(slice_istream& input, slice_ostream& output, Mode mode) {
        if ( mode == Mode::Raw ) return _writeRaw(input, output);

        slice  origInput      = input;
        size_t origOutputSize = output.capacity();
        logInfo("Compressing %zu bytes into %zu-byte buf", input.size, origOutputSize);

        switch ( mode ) {
            case Mode::NoFlush:
                _write("deflate", input, output, mode);
                break;
            case Mode::SyncFlush:
                _writeAndFlush(input, output);
                break;
            default:
                error::_throw(error::InvalidParameter);
        }

        if ( kZlibRawDeflate ) addToChecksum({origInput.buf, input.buf});

        logInfo("    compressed %zu bytes to %zu (%.0f%%), %u unflushed", (origInput.size - input.size),
                (origOutputSize - output.capacity()),
                (origOutputSize - output.capacity()) * 100.0 / (origInput.size - input.size), unflushedBytes());
    }

    void Deflater::_writeAndFlush(slice_istream& input, slice_ostream& output) {
        // If we try to write all of the input, and there isn't room in the output, the zlib
        // codec might end up with buffered data that hasn't been output yet (even though we
        // told it to flush.) To work around this, write the data gradually and stop before
        // the output fills up.
        static constexpr size_t kHeadroomForFlush = 12;
        static constexpr size_t kStopAtOutputSize = 100;

        Mode curMode = Mode::PartialFlush;
        while ( input.size > 0 ) {
            if ( output.capacity() >= deflateBound(&_z, (unsigned)input.size) ) {
                // Entire input is guaranteed to fit, so write it & flush:
                curMode = Mode::SyncFlush;
                _write("deflate", input, output, Mode::SyncFlush);
            } else {
                // Limit input size to what we know can be compressed into output.
                // Don't flush, because we may try to write again if there's still room.
                _write("deflate", input, output, curMode, output.capacity() - kHeadroomForFlush);
            }
            if ( output.capacity() <= kStopAtOutputSize ) break;
        }

        if ( curMode != Mode::SyncFlush ) {
            // Flush if we haven't yet (consuming no input):
            _write("deflate", input, output, Mode::SyncFlush, 0);
        }
    }

    unsigned Deflater::unflushedBytes() const {
        unsigned bytes;
        int      bits;
        check(deflatePending(&_z, &bytes, &bits));
        return bytes + (bits > 0);
    }

#pragma mark - INFLATER:

    Inflater::Inflater() : ZlibCodec(::inflate) {
        check(::inflateInit2(&_z, kZlibRawDeflate ? (-kZlibWindowSize) : (kZlibWindowSize + 32)));
    }

    Inflater::~Inflater() { ::inflateEnd(&_z); }

    void Inflater::write(slice_istream& input, slice_ostream& output, Mode mode) {
        if ( mode == Mode::Raw ) return _writeRaw(input, output);

        logInfo("Decompressing %zu bytes into %zu-byte buf", input.size, output.capacity());
        auto outStart = (uint8_t*)output.next();
        _write("inflate", input, output, mode);
        if ( kZlibRawDeflate ) addToChecksum({outStart, output.next()});

        logDebug("    decompressed %ld bytes: %.*s", (long)((uint8_t*)output.next() - outStart),
                 (int)((uint8_t*)output.next() - outStart), outStart);
    }

}}  // namespace litecore::blip
