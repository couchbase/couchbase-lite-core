//
// FilePath.hh
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

#include "PlatformCompat.hh"
#include "function_ref.hh"
#include <string>
#include <tuple> // for std::tie
#include <ctime>
#include <string_view>
#include <utility>

namespace fleece {
    struct slice;
    struct alloc_slice;
}


namespace litecore {

    /** A simple cross-platform class for working with files and paths.
        An instance represents a filesystem path, with the filename split out from the directory
        name. A path that is a directory has an empty filename. */
    class FilePath {
    public:
        /** Constructs a FilePath from a filesystem path. */
        explicit FilePath(std::string_view path)     {tie(_dir, _file) = splitPath(path);}

        FilePath();

        /** Constructs a FilePath from a directory name and a filename in that directory. */
        FilePath(std::string &&dirName, std::string &&fileName);
        FilePath(std::string_view dirName, std::string_view fileName);
        FilePath(const char *dirName, const char *fileName);

        /** Returns a folder with a predefined name that serves as a location for temporary
            files.  
         */
        static FilePath sharedTempDirectory(const std::string& location);

        static const std::string kSeparator;

        //////// DIRECTORY & FILE NAMES:
        
        bool isDir() const                  {return _file.empty();}

        FilePath dir() const                {return FilePath(_dir, "");}

        const std::string& dirName() const  {return _dir;}
        const std::string& fileName() const {return _file;}
        std::string fileOrDirName() const;
        std::string path() const            {return _dir + _file;}

        /** Returns a canonical standard form of the path by resolving symbolic links, normalizing
            capitalization (in case-insensitive filesystems), etc. */
        std::string canonicalPath() const;

        operator std::string () const       {return path();}
        operator fleece::alloc_slice() const;

        /** Converts a string to a valid filename by escaping invalid characters,
            including the directory separator ('/') */
        static std::string sanitizedFileName(std::string);

        //////// FILENAME EXTENSIONS:

        std::string unextendedName() const;
        std::string extension() const;

        /** Adds a filename extension. `ext` may or may not start with '.'.
            Cannot be called on directories. */
        FilePath addingExtension(const std::string &ext) const;

        /** Adds a filename extension only if there is none already. */
        FilePath withExtensionIfNone(const std::string &ext) const;

        /** Replaces the filename extension, or removes it if `ext` is empty.
            Cannot be called on directories. */
        FilePath withExtension(const std::string &ext) const;

        FilePath appendingToName(const std::string &suffix) const;

        //////// NAVIGATION:

        /** Adds a path component to a directory. Can ONLY be called on directories!
            If `name` ends in a separator it is assumed to be a directory, so the result will be
            a directory. */
        FilePath operator[] (const std::string &name) const;

        FilePath fileNamed(const std::string &filename) const;
        FilePath subdirectoryNamed(const std::string &dirname) const;
        
        /** Returns the parent directory. If the current path is root, the root directory
            will be returned. An exception will be thrown if the parent directory cannot
            be determined. */
        FilePath parentDir() const;

        /////// FILESYSTEM OPERATIONS:

        bool exists() const noexcept;
        bool existsAsDir() const noexcept;
        void mustExistAsDir() const;

        /** Returns the size of the file in bytes, or -1 if the file does not exist. */
        int64_t dataSize() const;

        /** Returns the date at which this file was last modified, or -1 if the file does not exist */
        time_t lastModified() const;

        /** Creates a directory at this path. */
        bool mkdir(int mode =0700) const;

        /** Creates an empty temporary file by appending a random 6-letter/digit string.
            If the `outHandle` parameter is non-null, it will be set to a writeable file handle. */
        FilePath mkTempFile(FILE* *outHandle =nullptr) const;

        /** Creates an empty temporary directory by appending a random 6-letter/digit string. */
        FilePath mkTempDir() const;

        /** Deletes the file, or empty directory, at this path.
            If the item doesn't exist, returns false instead of throwing an exception. */
        bool del() const;

        /** Deletes the file or directory tree at this path. */
        bool delRecursive() const;

        /** Moves this file/directory to a different path.
             An existing file or empty directory at the destination path will be replaced.
             A non-empty directory at the destination triggers an error. */
        void moveTo(const FilePath& to) const  {moveTo(to.path());}
        void moveTo(const std::string&) const;

        /** Like moveTo, but can replace a non-empty directory.
             First moves the destination dir aside to the temp directory,
             then moves the source into place,
             then deletes the moved-aside dir (asynchronously if that flag is set.) */
        void moveToReplacingDir(const FilePath &to, bool asyncCleanup) const;

        /** Copies this file (or directory, recursively) to a different path. */
        void copyTo(const FilePath& to) const  {copyTo(to.path());}
        void copyTo(const std::string&) const;

        void setReadOnly(bool readOnly) const;

        /** Calls fn for each file in this FilePath's directory. */
        void forEachFile(fleece::function_ref<void(const FilePath&)> fn) const;

        /** Calls fn for each item in the directory whose name begins with this path's name.
            If this path's name is empty (i.e. the path is a directory), fn is called for every
            item in the directory. */
        void forEachMatch(fleece::function_ref<void(const FilePath&)> fn) const;

        static std::pair<std::string,std::string> splitPath(std::string_view path);
        static std::pair<std::string,std::string> splitExtension(const std::string &filename);

    private:
        std::string _dir;    // Directory; always non-empty, always ends with separator ('/')
        std::string _file;   // Filename, or empty if this represents a directory
    };

}
