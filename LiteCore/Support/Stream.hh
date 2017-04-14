//
//  Stream.hh
//  LiteCore
//
//  Created by Jens Alfke on 9/3/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#pragma once
#include "Base.hh"
#include "FilePath.hh"
#include <stdio.h>


namespace litecore {

    /** A simple read-only seekable stream interface. */
    class ReadStream {
    public:
        virtual ~ReadStream() = default;
        virtual uint64_t getLength() const =0;
        virtual size_t read(void *dst, size_t count) =0;
        virtual void close() =0;

        alloc_slice readAll();
    };


    /** A simple output stream interface. */
    class WriteStream {
    public:
        virtual ~WriteStream() = default;
        virtual void write(slice) =0;
        virtual void close() =0;
    };


    class ReadWriteStream : public virtual ReadStream, public virtual WriteStream {
    };


    class Seekable {
    public:
        virtual ~Seekable() = default;
        virtual void seek(uint64_t pos) =0;
    };


    class SeekableReadStream : public virtual ReadStream, public virtual Seekable {
    };


    /** Concrete ReadStream that reads a file. */
    class FileReadStream : public virtual SeekableReadStream {
    public:
        FileReadStream(const FilePath& path)        :FileReadStream(path, "rb") {}
        FileReadStream(FILE *file)                  :_file(file) { }
        virtual ~FileReadStream();

        virtual uint64_t getLength() const override;
        virtual void seek(uint64_t pos) override;
        virtual size_t read(void *dst, size_t count) override;
        virtual void close() override;

    protected:
        FileReadStream(const FilePath &path, const char *mode);

        FILE* _file {nullptr};
    };

#ifdef _MSC_VER
#pragma warning(disable: 4250)
#endif

    /** Concrete WriteStream that writes to a file. (It can also read.) */
    class FileWriteStream : public virtual FileReadStream, public virtual ReadWriteStream {
    public:
        FileWriteStream(const FilePath& path, const char *mode) :FileReadStream(path, mode) {}
        FileWriteStream(FILE *file)                             :FileReadStream(file) {}

        virtual void write(slice) override;
        virtual void close() override                           {FileReadStream::close();}
    };

#ifdef _MSC_VER
#pragma warning(default: 4250)
#endif

}
