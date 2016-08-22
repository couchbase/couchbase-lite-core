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
        FilePath(const string &path)        {tie(_dir, _file) = splitPath(path);}

        FilePath(const string &dirName, const string &fileName);
        
        bool isDir() const                  {return _file.empty();}

        FilePath dir() const                {return FilePath(_dir, "");}

        string dirName() const              {return _dir;}
        string fileName() const             {return _file;}
        string path() const                 {return _dir + _file;}

        operator string () const            {return path();}

        string unextendedName() const       {return splitExtension(_file).first;}
        string extension() const            {return splitExtension(_file).second;}

        FilePath withExtension(const string&) const;

        static pair<string,string> splitPath(const string &path);
        static pair<string,string> splitExtension(const string &filename);

        /////// FILE OPERATIONS:

        /** Deletes the file at this path. (Doesn't delete directories.) */
        void unlink() const;

        /** Moves this file to a different path. */
        void moveTo(const FilePath& to) const  {moveTo(to.path());}
        void moveTo(const string&) const;

        /** Calls fn for each file in this FilePath's directory. */
        void forEachFile(function<void(const FilePath&)> fn) const;

        /** Calls fn for each file in the directory whose name begins with this path's name.
            If this path's name is empty (i.e. the path is a directory), fn is called for every
            file in the directory. */
        void forEachMatch(function<void(const FilePath&)> fn) const;

    private:
        string _dir, _file;
    };

}

#endif /* FilePath_hh */
