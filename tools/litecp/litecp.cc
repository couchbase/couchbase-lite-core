//
//  litecp.cc
//  LiteCore
//
//  Created by Jens Alfke on 4/23/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "ToolUtils.hh"
#include "JSONEndpoint.hh"
#include "DBEndpoint.hh"
#include "DirEndpoint.hh"
#include "FilePath.hh"
#include "StringUtil.hh"
#include "Stopwatch.hh"
#include <algorithm>
#include <exception>
#include <fstream>
#include <vector>


#if 0
class RemoteEndpoint : public Endpoint {
public:
    RemoteEndpoint(const string &spec)
    :Endpoint(spec)
    { }
    virtual void prepare(bool readOnly, bool mustExist, slice docIDProperty) override;
    virtual void copyTo(Endpoint*, uint64_t limit) override;
};
#endif


void usage() {
    cerr <<
    "litecp: Replicates/imports/exports LiteCore and Couchbase Lite 2 databases\n"
    "Usage: litecp <options> <src> <dst>\n"
    "  where <src> and <dst> may be any of:\n"
    "    * a database path (.cblite2 extension)\n"
    "    * a JSON file path (.json extension) in one-object-per line format\n"
    "    * a '/'-terminated path to a directory of JSON files (.json extensions)\n"
    "      in one-object-per-file format\n"
//  "    * a remote database URL (blip: or blips: scheme) [NOT YET IMPLEMENTED]\n"
    "  All combinations are allowed. Copying database-to-database uses the replicator.\n"
    "Options:\n"
    "    --existing or -x : Fail if <dst> doesn't already exist.\n"
    "    --id <property>: When <src> is JSON, this is a property name/path whose value will\n"
    "           be used as the docID. (If omitted, documents are given UUIDs.)\n"
    "           When <dst> is JSON, this is a property name that will be added to the JSON, whose\n"
    "           value is the docID. (If omitted, defaults to \"_id\".)\n"
    "    --limit <n>: Stop after <n> documents.\n"
    "    --careful: Abort on any error.\n"
    "    --verbose or -v: Log every 1000 docs. If given twice, log every docID.\n"
    "           If given three times, turn on LiteCore DB and Sync logging.\n"
    "    --help: You're looking at it.\n"
    ;
}


static int LiteCpMain(vector<string> &args) {
    try {
        bool createDst = true;
        string docIDPropertyStr;
        C4String docIDProperty {};
        uint64_t limit = UINT64_MAX;

        if (args.empty()) {
            usage();
            return 0;
        }
        while (args[0][0] == '-') {
            auto arg = args[0];
            auto flag = arg;
            while (flag[0] == '-')
                flag.erase(flag.begin());
            args.erase(args.begin());

            if (flag == "x" || flag == "existing") {
                createDst = false;
            } else if (flag == "id" || flag == "_id" || flag == "docID") {
                docIDPropertyStr = args[0];
                docIDProperty = c4str(docIDPropertyStr);
                args.erase(args.begin());
            } else if (flag == "limit" || flag == "l") {
                limit = stoull(args[0]);
                args.erase(args.begin());
            } else if (flag == "careful") {
                gFailOnError = true;
            } else if (flag == "verbose" || flag == "v") {
                ++gVerbose;
            } else if (flag == "help" || flag == "h") {
                usage();
                return 0;
            } else {
                fail(string("Unknown flag") + arg);
            }
        }

        if (args.size() != 2)
            fail("Missing source or destination path/URL");

        auto level = kC4LogWarning;
        if (gVerbose > 2)
            level = (C4LogLevel)std::max(0, level - (gVerbose - 2));
        c4log_setCallbackLevel(level);
        c4log_setLevel(c4log_getDomain("Sync", true), level);
        c4log_setLevel(c4log_getDomain("DB", true), level);

        Stopwatch timer;

        Endpoint *src = Endpoint::create(args[0]);
        if (!src)
            fail("Unknown source type");
        Endpoint *dst = Endpoint::create(args[1]);
        if (!dst)
            fail("Unknown destination type");
        src->prepare(true, true, docIDProperty, dst);
        dst->prepare(false,!createDst, docIDProperty, src);

        if (gVerbose)
            cout << "Copying...\n";
        src->copyTo(dst, limit);
        dst->finish();

        double time = timer.elapsed();
        cout << "Completed " << dst->docCount() << " docs in " << time << " secs; "
             << int(dst->docCount() / time) << " docs/sec\n";

        return 0;
    } catch (const std::exception &x) {
        fail(format("Uncaught C++ exception: %s", x.what()));
    } catch (...) {
        fail("Uncaught unknown C++ exception");
    }
}


int main(int argc, const char * argv[]) {
    vector<string> args;
    args.reserve(argc - 1);
    for(int i = 1; i < argc; ++i)
        args.emplace_back(argv[i]);
    return LiteCpMain(args);
}
