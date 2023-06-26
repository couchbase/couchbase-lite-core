//
// Stream.hh
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#pragma once
#include "Base.hh"
#include "FilePath.hh"
#include <cstdio>

namespace litecore {

    /** A simple read-only seekable stream interface. */
    class ReadStream {
      public:
        virtual ~ReadStream()                                                = default;
        [[nodiscard]] virtual uint64_t getLength() const                     = 0;
        virtual size_t                 read(void* dst NONNULL, size_t count) = 0;
        virtual void                   close()                               = 0;

        alloc_slice readAll();
    };

    /** A simple output stream interface. */
    class WriteStream {
      public:
        virtual ~WriteStream()    = default;
        virtual void write(slice) = 0;
        virtual void close()      = 0;
    };

    class ReadWriteStream
        : public virtual ReadStream
        , public virtual WriteStream {};

    class Seekable {
      public:
        virtual ~Seekable()             = default;
        virtual void seek(uint64_t pos) = 0;
    };

    class SeekableReadStream
        : public virtual ReadStream
        , public virtual Seekable {};

    /** Concrete ReadStream that reads a file. */
    class FileReadStream : public virtual SeekableReadStream {
      public:
        explicit FileReadStream(const FilePath& path) : FileReadStream(path, "rb") {}

        explicit FileReadStream(FILE* file NONNULL) : _file(file) {}

        ~FileReadStream() override;

        [[nodiscard]] uint64_t getLength() const override;
        void                   seek(uint64_t pos) override;
        size_t                 read(void* dst NONNULL, size_t count) override;
        void                   close() override;

      protected:
        FileReadStream(const FilePath& path, const char* mode NONNULL);

        FILE* _file{nullptr};
    };

#ifdef _MSC_VER
#    pragma warning(disable : 4250)
#endif

    /** Concrete WriteStream that writes to a file. (It can also read.) */
    class FileWriteStream final
        : public virtual FileReadStream
        , public virtual ReadWriteStream {
      public:
        FileWriteStream(const FilePath& path, const char* mode NONNULL) : FileReadStream(path, mode) {}

        explicit FileWriteStream(FILE* file NONNULL) : FileReadStream(file) {}

        void write(slice) override;

        void close() override { FileReadStream::close(); }
    };

#ifdef _MSC_VER
#    pragma warning(default : 4250)
#endif

}  // namespace litecore
