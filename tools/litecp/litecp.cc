//
//  litecp.cc
//  LiteCore
//
//  Created by Jens Alfke on 4/23/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#include "c4.hh"
#include "c4Document+Fleece.h"
#include "FleeceCpp.hh"
#include "StringUtil.hh"
#include "Benchmark.hh"
#include <exception>
#include <iostream>
#include <fstream>
#include <vector>

using namespace std;
using namespace fleece;
using namespace fleeceapi;
using namespace litecore;


int LiteCpMain(vector<string> &args);

int gVerbose {0};

bool gFailOnError {false};



#pragma mark - UTILITIES:


static void usage();


static inline string to_string(C4String s) {
    return string((const char*)s.buf, s.size);
}


static inline C4Slice c4str(const string &s) {
    return {s.data(), s.size()};
}


static void errorOccurred(const string &what) {
    cerr << "Error " << what << "\n";
    if (gFailOnError)
        exit(1);
}


static void errorOccurred(const string &what, C4Error err) {
    alloc_slice message = c4error_getMessage(err);
    cerr << "Error " << what << ": ";
    if (message.buf)
        cerr << to_string(message) << ' ';
    cerr << "(" << err.domain << "/" << err.code << ")\n";
    if (gFailOnError)
        exit(1);
}


[[noreturn]] static void fail(const string &message) {
    errorOccurred(message);
    exit(1);
}


[[noreturn]] static void fail(const string &what, C4Error err) {
    errorOccurred(what, err);
    exit(1);
}


[[noreturn]] static void failMisuse(const char *message ="Invalid parameters") {
    cerr << "Error: " << message << "\n";
    usage();
    exit(1);
}


#pragma mark - CLASSES:


/** Abstract base class for a source or target of copying/replication. */
class Endpoint {
public:
    Endpoint(const string &spec)
    :_spec(spec)
    { }

    static Endpoint* create(const string &str);
    virtual ~Endpoint() { }
    virtual void prepare(bool readOnly, bool mustExist, slice docIDProperty) {
        _docIDProperty = docIDProperty;
    }
    virtual void copyTo(Endpoint*, uint64_t limit) =0;

    virtual void writeJSON(slice docID, slice json) = 0;

    virtual void finish() { }

    uint64_t docCount() {
        return _docCount;
    }

protected:

    void logDocument(slice docID) {
        ++_docCount;
        if (gVerbose >= 2)
            cout << to_string(docID) << '\n';
        else if (gVerbose == 1 && (_docCount % 1000) == 0)
            cout << _docCount << '\n';
    }

    const string _spec;
    Encoder _encoder;
    alloc_slice _docIDProperty;
    uint64_t _docCount {0};
};


class JSONEndpoint : public Endpoint {
public:
    JSONEndpoint(const string &spec)
    :Endpoint(spec)
    { }

    virtual void prepare(bool readOnly, bool mustExist, slice docIDProperty) override;
    virtual void copyTo(Endpoint*, uint64_t limit) override;
    virtual void writeJSON(slice docID, slice json) override;

private:
    unique_ptr<ifstream> _in;
    unique_ptr<ofstream> _out;
};


class DbEndpoint : public Endpoint {
public:
    DbEndpoint(const string &spec)
    :Endpoint(spec)
    { }

    virtual void prepare(bool readOnly, bool mustExist, slice docIDProperty) override;
    virtual void copyTo(Endpoint *dst, uint64_t limit) override;
    virtual void writeJSON(slice docID, slice json) override;
    virtual void finish() override;

    void pushTo(DbEndpoint*);
    void exportTo(JSONEndpoint*);
    void importFrom(JSONEndpoint*);

private:
    void commit();

    c4::ref<C4Database> _db;
    unsigned _transactionSize {0};
    unique_ptr<KeyPath> _docIDPath;

    static constexpr unsigned kMaxTransactionSize = 1000;
};


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



Endpoint* Endpoint::create(const string &str) {
    if (hasSuffix(str, ".cblite2")) {
        return new DbEndpoint(str);
    } else if (hasSuffix(str, ".json")) {
        return new JSONEndpoint(str);
    } else if (hasPrefix(str, "blip:") || hasPrefix(str, "blips:")) {
        fail("Sorry, remote databases are not yet implemented");
        //return new RemoteEndpoint(str);
    } else {
        return nullptr;
    }
}


#pragma mark - DATABASE ENDPOINT:


void DbEndpoint::prepare(bool readOnly, bool mustExist, slice docIDProperty) {
    Endpoint::prepare(readOnly, mustExist, docIDProperty);
    C4DatabaseConfig config = {kC4DB_Bundled | kC4DB_SharedKeys | kc4DB_NonObservable};
    if (readOnly)
        config.flags |= kC4DB_ReadOnly;
    else if (!mustExist)
        config.flags |= kC4DB_Create;
    C4Error err;
    _db = c4db_open(c4str(_spec), &config, &err);
    if (!_db)
        fail(format("Couldn't open database %s", _spec.c_str()), err);
    if (!c4db_beginTransaction(_db, &err))
        fail("starting transaction", err);

    auto sk = c4db_getFLSharedKeys(_db);
    _encoder.setSharedKeys(sk);
    if (docIDProperty) {
        _docIDPath.reset(new KeyPath(docIDProperty, sk, nullptr));
        if (!*_docIDPath)
            fail("Invalid key-path");
    }
}


void DbEndpoint::commit() {
    if (gVerbose > 1) {
        cout << "[Committing ... ";
        cout.flush();
    }
    Stopwatch st;
    C4Error err;
    if (!c4db_endTransaction(_db, true, &err))
        fail("committing transaction", err);
    if (gVerbose > 1) {
        double time = st.elapsed();
        cout << time << " sec for " << _transactionSize << " docs]\n";
    }
    _transactionSize = 0;
}


void DbEndpoint::copyTo(Endpoint *dst, uint64_t limit) {
    // Special case: database to database
    auto dstDB = dynamic_cast<DbEndpoint*>(dst);
    if (dstDB)
        return pushTo(dstDB);

    C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
    C4Error err;
    c4::ref<C4DocEnumerator> e = c4db_enumerateAllDocs(_db, nullslice, nullslice, &options, &err);
    if (!e)
        fail("enumerating source db", err);
    uint64_t line;
    for (line = 0; line < limit; ++line) {
        c4::ref<C4Document> doc = c4enum_nextDocument(e, &err);
        if (!doc)
            break;
        alloc_slice json = c4doc_bodyAsJSON(doc, &err);
        if (!json) {
            errorOccurred("reading document body", err);
            continue;
        }
        dst->writeJSON(doc->docID, json);
    }
    if (err.code)
        errorOccurred("enumerating source db", err);
    else if (line == limit)
        cout << "Stopped after " << limit << " documents.\n";
}


void DbEndpoint::writeJSON(slice docID, slice json) {
    _encoder.reset();
    if (!_encoder.convertJSON(json)) {
        errorOccurred(format("Couldn't parse JSON: %.*s", SPLAT(json)));
        return;
    }
    alloc_slice body = _encoder.finish();

    // Get the JSON's docIDProperty to use as the document ID:
    if (!docID && _docIDProperty) {
        Dict root = Value::fromTrustedData(body).asDict();
        Value docIDProp = root[*_docIDPath];
        if (docIDProp) {
            docID = slice(docIDProp.asString());
            if (!docID)
                fail(format("Property \"%.*s\" is not a string in JSON: %.*s", SPLAT(_docIDProperty), SPLAT(json)));
        } else {
            errorOccurred(format("No property \"%.*s\" in JSON: %.*s", SPLAT(_docIDProperty), SPLAT(json)));
        }
    }

    C4DocPutRequest put { };
    put.docID = docID;
    put.body = body;
    put.save = true;
    C4Error err;
    c4::ref<C4Document> doc = c4doc_put(_db, &put, nullptr, &err);
    if (doc) {
        docID = slice(doc->docID);
    } else {
        if (docID)
            errorOccurred(format("saving document \"%.*s\"", SPLAT(put.docID)), err);
        else
            errorOccurred("saving document", err);
    }

    logDocument(docID);

    if (++_transactionSize >= kMaxTransactionSize) {
        commit();
        if (!c4db_beginTransaction(_db, &err))
            fail("starting transaction", err);
    }
}


void DbEndpoint::finish() {
    commit();
    C4Error err;
    if (!c4db_close(_db, &err))
        errorOccurred("closing database", err);
}


void DbEndpoint::pushTo(DbEndpoint *) {
    fail("Sorry, db-to-db replication is not implemented yet");
}


#pragma mark - JSON ENDPOINT:


void JSONEndpoint::prepare(bool readOnly, bool mustExist, slice docIDProperty) {
    Endpoint::prepare(readOnly, mustExist, docIDProperty);
    if (!docIDProperty)
        _docIDProperty = c4str("_id");
    bool err;
    if (readOnly) {
        _in.reset(new ifstream(_spec, ios_base::in));
        err = _in->fail();
    } else {
        if (mustExist && remove(_spec.c_str()) != 0)
            fail(format("Destination JSON file %s doesn't exist or is not writeable [--existing]",
                        _spec.c_str()));
        _out.reset(new ofstream(_spec, ios_base::trunc | ios_base::out));
        err = _out->fail();
    }
    if (err)
        fail(format("Couldn't open JSON file %s", _spec.c_str()));
}


void JSONEndpoint::copyTo(Endpoint *dst, uint64_t limit) {
    string line;
    unsigned lineNo;
    for (lineNo = 0; lineNo < limit && getline(*_in, line); ++lineNo) {
        dst->writeJSON(nullslice, c4str(line));
    }
    if (_in->bad())
        errorOccurred("Couldn't read JSON file");
    else if (lineNo == limit)
        cout << "Stopped after " << limit << " documents.\n";
}


void JSONEndpoint::writeJSON(slice docID, slice json) {
    if (docID) {
        *_out << "{\"" << to_string(_docIDProperty) << "\":\"";
        _out->write((char*)docID.buf, docID.size);
        *_out << "\",";
        json.moveStart(1);
    }
    _out->write((char*)json.buf, json.size);
    *_out << '\n';
    logDocument(docID);
}


#pragma mark - MAIN:


static void usage() {
    cerr <<
    "litecp: Replicates/imports/exports LiteCore and Couchbase Lite 2 databases\n"
    "Usage: litecp <options> <src> <dst>\n"
    "  where <src> and <dst> may be any of:\n"
    "    * a database path (.cblite2 extension)\n"
    "    * a JSON file path (.json extension) NOTE: Must contain JSON objects separated by \\n\n"
    "    * a remote database URL (blip: or blips: scheme) [NOT YET IMPLEMENTED]\n"
    "Options:\n"
    "    --existing or -x : Fail if <dst> doesn't already exist.\n"
    "    --id <property>: When <src> is JSON, this is a property name/path whose value will\n"
    "           be used as the docID. (If omitted, documents are given UUIDs.)\n"
    "           When <dst> is JSON, this is a property name that will be added to the JSON, whose\n"
    "           value is the docID. (If omitted, defaults to \"_id\".)\n"
    "    --limit <n>: Stop after <n> documents.\n"
    "    --careful: Abort on any error."
    "    --verbose or -v: Log every 1000 docs. If given twice, log every docID.\n"
    "    --help: You're looking at it.\n"
    ;
}


int LiteCpMain(vector<string> &args) {
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

        Stopwatch timer;

        Endpoint *src = Endpoint::create(args[0]);
        if (!src)
            failMisuse("Unknown source type");
        Endpoint *dst = Endpoint::create(args[1]);
        if (!dst)
            failMisuse("Unknown destination type");
        src->prepare(true, true, docIDProperty);
        dst->prepare(false,!createDst, docIDProperty);

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
