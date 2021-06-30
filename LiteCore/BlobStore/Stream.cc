//
// Stream.cc
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

#include "Stream.hh"
#include "Error.hh"
#include "Logging.hh"
#include "PlatformIO.hh"
#include <errno.h>
#include <memory>

namespace litecore {
    using namespace std;

    
    alloc_slice ReadStream::readAll() {
        uint64_t length = getLength();
        if (length > SIZE_MAX)    // overflow check for 32-bit
            throw bad_alloc();
        auto contents = alloc_slice((size_t)length);
        contents.shorten(read((void*)contents.buf, contents.size));
        return contents;
    }

    
    static void checkErr(FILE *file) {
        int err = ferror(file);
        if (_usuallyFalse(err != 0)) {
            error::convertErrno(err)._throw();
        }
    }


    FileReadStream::FileReadStream(const FilePath &path, const char *mode) {
        _file = fopen_u8(path.path().c_str(), mode);
        if (!_file)
            error::_throwErrno();
    }


    void FileReadStream::close() {
        auto file = _file;
        _file = nullptr;
        if (file && fclose(file) != 0)
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
		if(!_file) {
			return 0;
		}

        uint64_t curPos = ftello(_file);
        fseeko(_file, 0, SEEK_END);
        uint64_t fileSize = ftello(_file);
        fseeko(_file, curPos, SEEK_SET);
        checkErr(_file);
        return fileSize;
    }


    void FileReadStream::seek(uint64_t pos) {
		if(!_file) {
			return;
		}

        fseeko(_file, pos, SEEK_SET);
        checkErr(_file);
    }


    size_t FileReadStream::read(void *dst, size_t count) {
		if(!_file) {
			return 0;
		}

        size_t bytesRead = fread(dst, 1, count, _file);
        checkErr(_file);
        return bytesRead;
    }



    void FileWriteStream::write(slice data) {
		if(_file) {
			if (fwrite(data.buf, 1, data.size, _file) < data.size)
				checkErr(_file);
		}
    }

}
