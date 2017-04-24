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
#include <exception>
#include <iostream>
#include <fstream>
#include <vector>

using namespace std;
using namespace fleece;
using namespace fleeceapi;
using namespace litecore;


int LiteCpMain(vector<string> &args);

bool gVerbose {false};



#pragma mark - UTILITIES:


static void usage();


static inline string to_string(C4String s) {
    return string((const char*)s.buf, s.size);
}

static inline C4Slice c4str(const string &s) {
    return {s.data(), s.size()};
}


[[noreturn]] static void fail(const char *message) {
    cerr << "Error: " << message << "\n";
    exit(1);
}

[[noreturn]] static void fail(const string &message) {
    fail(message.c_str());
}


[[noreturn]] static void fail(const char *what, C4Error err) {
    alloc_slice message = c4error_getMessage(err);
    cerr << "Error " << what << ": ";
    if (message.buf)
        cerr << to_string(message);
    cerr << "(" << err.domain << "/" << err.code << ")\n";
    exit(1);
}

[[noreturn]] static void fail(const string &what, C4Error err) {
    fail(what.c_str(), err);
}



[[noreturn]] static void failMisuse(const char *message ="Invalid parameters") {
    cerr << "Error: " << message << "\n";
    usage();
    exit(1);
}


#pragma mark - CLASSES:


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
    virtual void copyTo(Endpoint*) =0;

    virtual void writeJSON(slice docID, slice json) = 0;

    virtual void finish() { }

protected:
    const string _spec;
    Encoder _encoder;
    alloc_slice _docIDProperty;
};


class JSONEndpoint : public Endpoint {
public:
    JSONEndpoint(const string &spec)
    :Endpoint(spec)
    { }

    virtual void prepare(bool readOnly, bool mustExist, slice docIDProperty) override;
    virtual void copyTo(Endpoint*) override;
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
    virtual void copyTo(Endpoint *dst) override;
    virtual void writeJSON(slice docID, slice json) override;
    virtual void finish() override;

    void pushTo(DbEndpoint*);
    void exportTo(JSONEndpoint*);
    void importFrom(JSONEndpoint*);

private:
    c4::ref<C4Database> _db;
    unsigned _transactionSize {0};
    unique_ptr<KeyPath> _docIDPath;

    static constexpr unsigned kMaxTransactionSize = 10000;
};


#if 0
class RemoteEndpoint : public Endpoint {
public:
    RemoteEndpoint(const string &spec)
    :Endpoint(spec)
    { }
    virtual void prepare(bool readOnly, bool mustExist, slice docIDProperty) override;
    virtual void copyTo(Endpoint*) override;
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


#pragma mark - DATABASE:


void DbEndpoint::prepare(bool readOnly, bool mustExist, slice docIDProperty) {
    Endpoint::prepare(readOnly, mustExist, docIDProperty);
    C4DatabaseConfig config = {kC4DB_Bundled | kC4DB_SharedKeys};
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



void DbEndpoint::copyTo(Endpoint *dst) {
    // Special case: database to database
    auto dstDB = dynamic_cast<DbEndpoint*>(dst);
    if (dstDB)
        return pushTo(dstDB);

    C4EnumeratorOptions options = kC4DefaultEnumeratorOptions;
    C4Error err;
    c4::ref<C4DocEnumerator> e = c4db_enumerateAllDocs(_db, nullslice, nullslice, &options, &err);
    if (!e)
        fail("enumerating source db", err);
    while (true) {
        c4::ref<C4Document> doc = c4enum_nextDocument(e, &err);
        if (!doc)
            break;
        alloc_slice json = c4doc_bodyAsJSON(doc, &err);
        if (!json)
            fail("reading document body", err);
        dst->writeJSON(doc->docID, json);
    }
    if (err.code)
        fail("enumerating source db", err);
}


void DbEndpoint::writeJSON(slice docID, slice json) {
    _encoder.reset();
    if (!_encoder.convertJSON(json))
        fail(format("Couldn't parse JSON: %.*s", SPLAT(json)));
    alloc_slice body = _encoder.finish();

    // Get the JSON's docIDProperty to use as the document ID:
    if (!docID && _docIDProperty) {
        Dict root = Value::fromTrustedData(body).asDict();
        Value docIDProp = root[*_docIDPath];
        if (!docIDProp) {
            fail(format("No property \"%.*s\" in JSON: %.*s", SPLAT(_docIDProperty), SPLAT(json)));
            // ??? Should I just assign a UUID instead of failing?
        }
        docID = slice(docIDProp.asString());
        if (!docID)
            fail(format("Property \"%.*s\" is not a string in JSON: %.*s", SPLAT(_docIDProperty), SPLAT(json)));
    }

    C4DocPutRequest put { };
    put.docID = docID;
    put.body = body;
    put.save = true;
    C4Error err;
    c4::ref<C4Document> doc = c4doc_put(_db, &put, nullptr, &err);
    if (!doc) {
        if (docID)
            fail(format("saving document \"%.*s\"", SPLAT(put.docID)), err);
        else
            fail("saving document", err);
    }

    if (gVerbose)
        cout << to_string(doc->docID) << '\n';

    if (++_transactionSize >= kMaxTransactionSize) {
        if (gVerbose)
            cout << "[Committing...";
        if (!c4db_endTransaction(_db, true, &err))
            fail("committing transaction", err);
        if (!c4db_beginTransaction(_db, &err))
            fail("starting transaction", err);
        _transactionSize = 0;
        if (gVerbose)
            cout << "]\n";
    }
}


void DbEndpoint::finish() {
    C4Error err;
    if (!c4db_endTransaction(_db, true, &err))
        fail("committing transaction", err);
    c4db_close(_db, &err);
}


void DbEndpoint::pushTo(DbEndpoint *) {
    fail("Sorry, db-to-db replication is not implemented yet");
}


#pragma mark - JSON:


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


void JSONEndpoint::copyTo(Endpoint *dst) {
    string line;
    unsigned lineNo = 1;
    while (getline(*_in, line)) {
        dst->writeJSON(nullslice, c4str(line));
        ++lineNo;
    }
    if (_in->bad())
        fail("Couldn't read JSON file");
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
    "    --existing or -x : Fail if <dst> doesn't already exist\n"
    "    --id <property>: When <src> is JSON, this is a property name/path whose value will\n"
    "           be used as the docID. (If omitted, documents are given UUIDs.)\n"
    "           When <dst> is JSON, this is a property name that will be added to the JSON, whose\n"
    "           value is the docID. (If omitted, defaults to \"_id\".)\n"
    "    --verbose or -v: Log every document being imported/exported\n"
    "    --help: You're looking at it\n"
    ;
}


int LiteCpMain(vector<string> &args) {
    try {
        bool createDst = true;
        string docIDPropertyStr;
        C4String docIDProperty {};

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
            } else if (flag == "verbose" || flag == "v") {
                gVerbose = true;
            } else if (flag == "help" || flag == "h") {
                usage();
                return 0;
            } else {
                fail(string("Unknown flag") + arg);
            }
        }

        if (args.size() != 2)
            fail("Missing source or destination path/URL");

        Endpoint *src = Endpoint::create(args[0]);
        if (!src)
            failMisuse("Unknown source type");
        Endpoint *dst = Endpoint::create(args[1]);
        if (!dst)
            failMisuse("Unknown destination type");
        src->prepare(true, true, docIDProperty);
        dst->prepare(false,!createDst, docIDProperty);
        src->copyTo(dst);
        dst->finish();
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
