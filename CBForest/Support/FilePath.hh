//
//  FilePath.hh
//  CBForest
//
//  Created by Jens Alfke on 8/19/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#ifndef FilePath_hh
#define FilePath_hh

#include <string>
#include <functional>

namespace cbforest {
    using namespace std;

    /** A simple cross-platform class for working with files and paths.
        An instance represents a filesystem path, with the filename split out from the directory
        name. A path that is a directory has an empty filename. */
    class FilePath {
    public:
        /** Constructs a FilePath from a filesystem path. */
        FilePath(const string &path)        {tie(_dir, _file) = splitPath(path);}
        FilePath(const char *path)          {tie(_dir, _file) = splitPath(string(path));}

        /** Constructs a FilePath from a directory name and a filename in that directory. */
        FilePath(const string &dirName, const string &fileName);

        /** Returns the system's temporary-files directory. */
        static FilePath tempDirectory();

        //////// DIRECTORY & FILE NAMES:
        
        bool isDir() const                  {return _file.empty();}

        FilePath dir() const                {return FilePath(_dir, "");}

        string dirName() const              {return _dir;}
        string fileName() const             {return _file;}
        string path() const                 {return _dir + _file;}

        operator string () const            {return path();}

        /** Simple equality test that requires paths to be identical. */
        bool operator== (const FilePath&) const;

        //////// FILENAME EXTENSIONS:

        string unextendedName() const       {return splitExtension(_file).first;}
        string extension() const            {return splitExtension(_file).second;}

        /** Adds a filename extension. `ext` may or may not start with '.'.
            Cannot be called on directories. */
        FilePath addingExtension(const string &ext) const;

        /** Adds a filename extension only if there is none already. */
        FilePath withExtensionIfNone(const string &ext) const;

        /** Replaces the filename extension, or removes it if `ext` is empty.
            Cannot be called on directories. */
        FilePath withExtension(const string &ext) const;

        /** Adds a path component to a directory. Can ONLY be called on directories!
            If `name` ends in a separator it is assumed to be a directory, so the result will be
            a directory. */
        FilePath operator[] (const string &name) const;

        /////// FILESYSTEM OPERATIONS:

        bool exists() const;
        bool existsAsDir() const;
        void mustExistAsDir() const;
        
        bool mkdir(int mode =0700) const;

        /** Deletes the file, or empty directory, at this path. */
        void del() const;

        /** Deletes the file or directory tree at this path. */
        void delRecursive() const;

        /** Moves this file to a different path. */
        void moveTo(const FilePath& to) const  {moveTo(to.path());}
        void moveTo(const string&) const;

        /** Calls fn for each file in this FilePath's directory. */
        void forEachFile(function<void(const FilePath&)> fn) const;

        /** Calls fn for each item in the directory whose name begins with this path's name.
            If this path's name is empty (i.e. the path is a directory), fn is called for every
            item in the directory. */
        void forEachMatch(function<void(const FilePath&)> fn) const;

    private:
        static pair<string,string> splitPath(const string &path);
        static pair<string,string> splitExtension(const string &filename);

        string _dir;    // Directory; always non-empty, always ends with separator ('/')
        string _file;   // Filename, or empty if this represents a directory
    };

}

#endif /* FilePath_hh */
