//
// FilePath.hh
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

#include "fleece/function_ref.hh"
#include <string>
#include <tuple>  // for std::tie
#include <ctime>
#include <string_view>
#include <utility>
#include <filesystem>
#include "NumConversion.hh"

namespace fleece {
    struct slice;
    struct alloc_slice;
}  // namespace fleece

namespace litecore {

    /** A simple cross-platform class for working with files and paths.
        An instance represents a filesystem path, with the filename split out from the directory
        name. A path that is a directory has an empty filename. */
    class FilePath {
      public:
        /** Constructs a FilePath from a filesystem path. */
        FilePath(std::filesystem::path&& path);

        FilePath(const std::filesystem::path& path);

        FilePath();

        /** Returns a folder with a predefined name that serves as a location for temporary
            files.  
         */
        static FilePath sharedTempDirectory(const std::string& location);

        static const std::string kSeparator;

        //////// DIRECTORY & FILE NAMES:

        [[nodiscard]] bool isDir() const { return std::filesystem::is_directory(_path); }

        [[nodiscard]] FilePath dir() const { return isDir() ? *this : parentDir(); }

        [[nodiscard]] const std::string dirName() const {
            return isDir() ? _path.filename().string() : _path.parent_path().filename().string();
        }

        [[nodiscard]] const std::string fileName() const { return !isDir() ? _path.filename().string() : ""; }

        [[nodiscard]] std::string fileOrDirName() const { return _path.filename().string(); }

        [[nodiscard]] std::string path() const { return _path.string(); }

        /** Returns a canonical standard form of the path by resolving symbolic links, normalizing
            capitalization (in case-insensitive filesystems), etc. */
        [[nodiscard]] std::string canonicalPath() const { return _path.lexically_normal().string(); }

        explicit operator std::string() const { return path(); }

        operator std::filesystem::path() const { return _path; }

        explicit operator fleece::alloc_slice() const;

        /** Converts a string to a valid filename by escaping invalid characters,
            including the directory separator ('/') */
        static std::string sanitizedFileName(std::string);

        //////// FILENAME EXTENSIONS:

        [[nodiscard]] std::string unextendedName() const { return _path.stem().string(); }

        [[nodiscard]] std::string extension() const { return _path.extension().string(); }

        /** Adds a filename extension. `ext` may or may not start with '.'.
            Cannot be called on directories. */
        [[nodiscard]] FilePath addingExtension(const std::string& ext) const;

        /** Adds a filename extension only if there is none already. */
        [[nodiscard]] FilePath withExtensionIfNone(const std::string& ext) const;

        /** Replaces the filename extension, or removes it if `ext` is empty.
            Cannot be called on directories. */
        [[nodiscard]] FilePath withExtension(const std::string& ext) const;

        [[nodiscard]] FilePath appendingToName(const std::string& suffix) const;

        //////// NAVIGATION:

        /** Adds a path component to a directory. Can ONLY be called on directories!
            If `name` ends in a separator it is assumed to be a directory, so the result will be
            a directory. */
        FilePath operator[](const std::string& name) const;

        [[nodiscard]] FilePath fileNamed(const std::string& filename) const;
        [[nodiscard]] FilePath subdirectoryNamed(const std::string& dirname) const;

        /** Returns the parent directory. If the current path is root, the root directory
            will be returned. An exception will be thrown if the parent directory cannot
            be determined. */
        [[nodiscard]] FilePath parentDir() const;

        /////// FILESYSTEM OPERATIONS:

        [[nodiscard]] bool exists() const noexcept { return std::filesystem::exists(_path); }

        [[nodiscard]] bool existsAsDir() const noexcept { return std::filesystem::is_directory(_path); }

        void mustExistAsDir() const;

        /** Returns the size of the file in bytes, or -1 if the file does not exist. */
        [[nodiscard]] int64_t dataSize() const {
            return exists() ? fleece::narrow_cast<int64_t>(std::filesystem::file_size(_path)) : -1;
        }

        /** Returns the date at which this file was last modified, or -1 if the file does not exist */
        [[nodiscard]] time_t lastModified() const {
            return exists() ? fleece::narrow_cast<time_t>(
                           std::filesystem::last_write_time(_path).time_since_epoch().count())
                            : -1;
        }

        /**
         * The return values of mkdir(), del(), and delRecursive() are almost never used throughout our code,
         * so making them nodiscard causes a lot of compilation warnings (and thereby errors with -Werror).
         */
        // NOLINTBEGIN(modernize-use-nodiscard)

        /** Creates a directory at this path. */
        bool mkdir(int mode = 0700) const;

        /** Creates an empty temporary file by appending a random 6-letter/digit string.
            If the `outHandle` parameter is non-null, it will be set to a writeable file handle. */
        FilePath mkTempFile(FILE** outHandle = nullptr) const;

        /** Creates an empty temporary directory by appending a random 6-letter/digit string. */
        [[nodiscard]] FilePath mkTempDir() const;

        /** Deletes the file, or empty directory, at this path.
            If the item doesn't exist, returns false instead of throwing an exception. */
        bool del() const;

        /** Deletes the file or directory tree at this path. */
        bool delRecursive() const;

        // NOLINTEND(modernize-use-nodiscard)

        /** Moves this file/directory to a different path.
             An existing file or empty directory at the destination path will be replaced.
             A non-empty directory at the destination triggers an error. */
        void moveTo(const FilePath& to) const { moveTo(to.path()); }

        void moveTo(const std::string&) const;

        /** Like moveTo, but can replace a non-empty directory.
             First moves the destination dir aside to the temp directory,
             then moves the source into place,
             then deletes the moved-aside dir (asynchronously if that flag is set.) */
        void moveToReplacingDir(const FilePath& to, bool asyncCleanup) const;

        /** Copies this file (or directory, recursively) to a different path. */
        void copyTo(const FilePath& to) const { copyTo(to.path()); }

        void copyTo(const std::string&) const;

        void setReadOnly(bool readOnly) const;

        /** Calls fn for each file in this FilePath's directory. */
        void forEachFile(fleece::function_ref<void(const FilePath&)> fn) const;

        /** Calls fn for each item in the directory whose name begins with this path's name.
            If this path's name is empty (i.e. the path is a directory), fn is called for every
            item in the directory. */
        void forEachMatch(fleece::function_ref<void(const FilePath&)> fn) const;

        static std::pair<std::string, std::string> splitPath(std::string_view path);
        static std::pair<std::string, std::string> splitExtension(const std::string& filename);

      private:
        std::filesystem::path _path;
    };

}  // namespace litecore
