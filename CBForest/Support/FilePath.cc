//
//  FilePath.cc
//  CBForest
//
//  Created by Jens Alfke on 8/19/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "FilePath.hh"
#include "Error.hh"
#include <dirent.h>
#include <errno.h>
#include <unistd.h>


namespace cbforest {

#ifdef _MSC_VER
    static const char kSeparatorChar = '\\';
    static const char* kCurrentDir = ".\\";
#else
    static const char kSeparatorChar = '/';
    static const char* kCurrentDir = "./";
#endif


    FilePath::FilePath(const string &dirName, const string &fileName)
    :_dir(dirName), _file(fileName)
    {
        if (_dir.empty())
            _dir = kCurrentDir;
        else if (_dir[_dir.size()-1] != kSeparatorChar)
            _dir += kSeparatorChar;
    }


    pair<string,string> FilePath::splitPath(const string &path) {
        string dirname, basename;
        auto slash = path.rfind(kSeparatorChar);
        if (slash == string::npos)
            return {kCurrentDir, path};
        else
            return {path.substr(0, slash+1), path.substr(slash+1)};
    }

    pair<string,string> FilePath::splitExtension(const string &file) {
        auto dot = file.rfind('.');
        if (dot == string::npos)
            return {file, ""};
        else
            return {file.substr(0, dot), file.substr(dot+1)};
    }


    FilePath FilePath::withExtension(const string &ext) const {
        auto name = unextendedName();
        if (ext.empty())
            return FilePath(_dir, name);
        else if (ext[0] == '.')
            return FilePath(_dir, name + ext);
        else
            return FilePath(_dir, name + "." + ext);
    }


    void FilePath::forEachMatch(function<void(const FilePath&)> fn) const {
        auto dir = opendir(_dir.c_str());
        if (!dir)
            error::_throwErrno();
        try {
            struct dirent entry, *result;
            while (1) {
                int err = readdir_r(dir, &entry, &result);
                if (err)
                    error::_throw(error::POSIX, err);
                else if (!result)
                    break;
                if (result->d_type == DT_REG) {
                    string name(result->d_name, result->d_namlen);
                    if (_file.empty() || name.find(_file) == 0) {
                        fn(FilePath(_dir,name));
                    }
                }
            }
        } catch(...) {
            closedir(dir);
            throw;
        }
        closedir(dir);
    }


    void FilePath::forEachFile(function<void(const FilePath&)> fn) const {
        dir().forEachMatch(fn);
    }


    void FilePath::unlink() const {
        if (::unlink(path().c_str()) != 0)
            error::_throwErrno();
    }

    
    void FilePath::moveTo(const string &to) const {
        if (::rename(path().c_str(), to.c_str()) != 0)
            error::_throwErrno();
    }

}
