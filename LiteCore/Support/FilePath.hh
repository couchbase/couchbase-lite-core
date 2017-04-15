//
//  FilePath.hh
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 8/19/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#pragma once

#include <string>
#include <tuple> // for std::tie
#include "function_ref.hh"

namespace litecore {

    /** A simple cross-platform class for working with files and paths.
        An instance represents a filesystem path, with the filename split out from the directory
        name. A path that is a directory has an empty filename. */
    class FilePath {
    public:
        /** Constructs a FilePath from a filesystem path. */
        FilePath(const std::string &path)   {tie(_dir, _file) = splitPath(path);}
        FilePath(const char *path)          {tie(_dir, _file) = splitPath(std::string(path));}

        FilePath();

        /** Constructs a FilePath from a directory name and a filename in that directory. */
        FilePath(const std::string &dirName, const std::string &fileName);

        /** Returns the system's temporary-files directory. */
        static FilePath tempDirectory();

        //////// DIRECTORY & FILE NAMES:
        
        bool isDir() const                  {return _file.empty();}

        FilePath dir() const                {return FilePath(_dir, "");}

        std::string dirName() const         {return _dir;}
        std::string fileName() const        {return _file;}
        std::string fileOrDirName() const;
        std::string path() const            {return _dir + _file;}

        operator std::string () const       {return path();}

        /** Simple equality test that requires paths to be identical. */
        bool operator== (const FilePath&) const;

        /** Converts a string to a valid filename by escaping invalid characters,
            including the directory separator ('/') */
        static std::string sanitizedFileName(std::string);

        //////// FILENAME EXTENSIONS:

        std::string unextendedName() const  {return splitExtension(_file).first;}
        std::string extension() const       {return splitExtension(_file).second;}

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

        /////// FILESYSTEM OPERATIONS:

        bool exists() const;
        bool existsAsDir() const;
        void mustExistAsDir() const;

        /** Returns the size of the file in bytes, or -1 if the file does not exist. */
        int64_t dataSize() const;

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

        bool delWithAllExtensions(char separator ='.') const;

        /** Moves this file/directory to a different path. */
        void moveTo(const FilePath& to) const  {moveTo(to.path());}
        void moveTo(const std::string&) const;

        /** Copies this file (or directory, recursively) to a different path. */
        void copyTo(const FilePath& to) const  {copyTo(to.path());}
        void copyTo(const std::string&) const;

        void setReadOnly(bool readOnly) const;

        /** Calls fn for each file in this FilePath's directory. */
        void forEachFile(function_ref<void(const FilePath&)> fn) const;

        /** Calls fn for each item in the directory whose name begins with this path's name.
            If this path's name is empty (i.e. the path is a directory), fn is called for every
            item in the directory. */
        void forEachMatch(function_ref<void(const FilePath&)> fn) const;

    private:
        static std::pair<std::string,std::string> splitPath(const std::string &path);
        static std::pair<std::string,std::string> splitExtension(const std::string &filename);

        std::string _dir;    // Directory; always non-empty, always ends with separator ('/')
        std::string _file;   // Filename, or empty if this represents a directory
    };

}
