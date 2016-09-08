//
//  Stream.cc
//  LiteCore
//
//  Created by Jens Alfke on 9/3/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "Stream.hh"
#include "Error.hh"
#include "LogInternal.hh"
#include <errno.h>
#include <memory>

namespace litecore {
    using namespace std;

    
    alloc_slice ReadStream::readAll() {
        uint64_t length = getLength();
        if (length > SIZE_MAX)    // overflow check for 32-bit
            throw bad_alloc();
        auto contents = alloc_slice((size_t)length);
        contents.size = read((void*)contents.buf, length);
        return contents;
    }

    
    static void checkErr(FILE *file) {
        int err = ferror(file);
        if (err)
            error::_throw(error::POSIX, err);
    }


    FileReadStream::FileReadStream(const FilePath &path, const char *mode) {
        _file = fopen(path.path().c_str(), mode);
        if (!_file)
            error::_throwErrno();
    }


    void FileReadStream::close() {
        auto file = _file;
        _file = nullptr;
        if (fclose(file) != 0)
            error::_throwErrno();
    }


    FileReadStream::~FileReadStream() {
        if (_file) {
            // Destructor cannot throw exceptions, so just warn if there was an error:
            if (fclose(_file) < 0)
                Warn("FileStream destructor: fclose got error %d", errno);
        }
    }


    uint64_t FileReadStream::getLength() const {
        uint64_t curPos = ftell(_file);
        fseek(_file, 0, SEEK_END);
        uint64_t fileSize = ftell(_file);
        fseek(_file, curPos, SEEK_SET);
        checkErr(_file);
        return fileSize;
    }


    void FileReadStream::seek(uint64_t pos) {
        fseek(_file, pos, SEEK_SET);
        checkErr(_file);
    }


    size_t FileReadStream::read(void *dst, size_t count) {
        size_t bytesRead = fread(dst, 1, count, _file);
        checkErr(_file);
        return bytesRead;
    }



    void FileWriteStream::write(slice data) {
        if (fwrite(data.buf, 1, data.size, _file) < data.size)
        checkErr(_file);
    }

}
