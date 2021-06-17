//
// Stream.hh
//
// Copyright (c) 2016 Couchbase, Inc All rights reserved.
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
#include "Base.hh"
#include "FilePath.hh"
#include <stdio.h>


namespace litecore {

    /** A simple read-only seekable stream interface. */
    class ReadStream {
    public:
        virtual ~ReadStream() = default;
        virtual uint64_t getLength() const =0;
        virtual size_t read(void *dst NONNULL, size_t count) =0;
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
        FileReadStream(FILE *file NONNULL)          :_file(file) { }
        virtual ~FileReadStream();

        virtual uint64_t getLength() const override;
        virtual void seek(uint64_t pos) override;
        virtual size_t read(void *dst NONNULL, size_t count) override;
        virtual void close() override;

    protected:
        FileReadStream(const FilePath &path, const char *mode NONNULL);

        FILE* _file {nullptr};
    };

#ifdef _MSC_VER
#pragma warning(disable: 4250)
#endif

    /** Concrete WriteStream that writes to a file. (It can also read.) */
    class FileWriteStream final : public virtual FileReadStream, public virtual ReadWriteStream {
    public:
        FileWriteStream(const FilePath& path, const char *mode NONNULL) :FileReadStream(path, mode) {}
        FileWriteStream(FILE *file NONNULL)                             :FileReadStream(file) {}

        virtual void write(slice) override;
        virtual void close() override                           {FileReadStream::close();}
    };

#ifdef _MSC_VER
#pragma warning(default: 4250)
#endif

}
