//
//  Tool.hh
//  LiteCore
//
//  Created by Jens Alfke on 8/29/17.
//Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "c4.hh"
#include "FleeceCpp.hh"
#include "StringUtil.hh"
#include <iostream>
#include <string>
#include <deque>

using namespace std;
using namespace fleece;
using namespace litecore;


static inline string to_string(C4String s) {
    return string((const char*)s.buf, s.size);
}

static inline C4Slice c4str(const string &s) {
    return {s.data(), s.size()};
}

static inline std::ostream& operator<< (std::ostream& o, slice s) {
    o.write((const char*)s.buf, s.size);
    return o;
}


class Tool {
public:
    Tool() { }
    virtual ~Tool() { }

    virtual int main(int argc, const char * argv[]) {
        try {
            _toolPath = string(argv[0]);
            for(int i = 1; i < argc; ++i)
                _args.emplace_back(argv[i]);
            processFlags(_flagSpecs);
            return run();
        } catch (const std::exception &x) {
            fail(format("Uncaught C++ exception: %s", x.what()));
        } catch (...) {
            fail("Uncaught unknown C++ exception");
        }
    }


    string peekNextArg() {
        if (_args.empty())
            return "";
        else
            return _args[0];
    }


    string nextArg(const char *what) {
        if (_args.empty())
            failMisuse(format("Missing argument: expected %s", what));
        string arg = _args[0];
        _args.pop_front();
        return arg;
    }

    void endOfArgs() {
        if (!_args.empty())
            fail(format("Unexpected extra args, starting with '%s'", _args[0].c_str()));
    }


    typedef void (Tool::*FlagHandler)(string flag);
    struct FlagSpec {const char *flag; FlagHandler handler;};

    virtual void processFlags(const FlagSpec specs[]) {
        while(true) {
            string flag = peekNextArg();
            if (flag.empty() || !hasPrefix(flag, "-"))
                return;
            _args.pop_front();

            if (flag == "--help") {
                usage();
                exit(0);
            } else if (!processFlag(flag, specs)) {
                fail(string("Unknown flag ") + flag);
            }
        }
    }

    bool processFlag(const string &flag, const FlagSpec specs[]) {
        for (const FlagSpec *spec = specs; spec->flag; ++spec) {
            if (flag == string(spec->flag)) {
                (this->*spec->handler)(flag);
                return true;
            }
        }
        return false;
    }

    virtual int run() {
        usage();
        return 1;
    }

    virtual void usage() = 0;

protected:
    void registerFlags(const FlagSpec flags[])      {_flagSpecs = flags;}

    void errorOccurred(const string &what) {
        cerr << "Error " << what << "\n";
        if (_failOnError)
            exit(1);
    }

    void errorOccurred(const string &what, C4Error err) {
        alloc_slice message = c4error_getMessage(err);
        cerr << "Error " << what << ": ";
        if (message.buf)
            cerr << to_string(message) << ' ';
        cerr << "(" << err.domain << "/" << err.code << ")\n";
        if (_failOnError)
            exit(1);
    }

    [[noreturn]] void fail(const string &message) {
        errorOccurred(message);
        exit(1);
    }


    [[noreturn]] void fail(const string &what, C4Error err) {
        errorOccurred(what, err);
        exit(1);
    }


    [[noreturn]] void failMisuse(const string &message) {
        cerr << "Error: " << message << "\n";
        usage();
        exit(1);
    }

    string _toolPath;
    deque<string> _args;
    const FlagSpec *_flagSpecs {nullptr};
    int _verbose {0};
    bool _failOnError {false};
};
