//
//  cbliteTool.cc
//  LiteCore
//
//  Created by Jens Alfke on 8/29/17.
//Copyright © 2017 Couchbase. All rights reserved.
//

#include "Tool.hh"
#include "c4Document+Fleece.h"
#include "FilePath.hh"
#include "StringUtil.hh"
#include <exception>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#if __APPLE__
#include <sys/ioctl.h>
#endif

using namespace fleeceapi;


static const int kListColumnWidth = 16;
static const int kDefaultLineWidth = 100;


class CBLiteTool : public Tool {
public:
    CBLiteTool() {
        const FlagSpec flags[] = {
            {nullptr, nullptr}
        };
        registerFlags(flags);
    }

    virtual ~CBLiteTool() {
        c4db_free(_db);
    }

    void usage() override {
        cerr <<
        "cblite: Interactive LiteCore / Couchbase Lite tool\n"
        "Usage: cblite query [FLAGS] DBPATH JSONQUERY\n"
        "           --offset N : Skip first N rows\n"
        "           --limit N : Stop after N rows\n"
        "       cblite ls [FLAGS] DBPATH\n"
        "           -l : Long format (one doc per line, with metadata)\n"
        "           --offset N : Skip first N docs\n"
        "           --limit N : Stop after N docs\n"
        "           --desc : Descending order\n"
        "           --seq : Order by sequence, not docID\n"
        "           --del : Include deleted documents\n"
        "           --conf : Show only conflicted documents\n"
        "       cblite file DBPATH\n"
        ;
    }


    int run() override {
        c4log_setCallbackLevel(kC4LogWarning);
        string cmd = nextArg("subcommand");
        if (!processFlag(cmd, kSubcommands))
            failMisuse(format("Unknown subcommand '%s'", cmd.c_str()));
        return 0;
    }


    void openDatabase(string path) {
        C4DatabaseConfig config = {kC4DB_Bundled | kC4DB_SharedKeys | kC4DB_NonObservable | kC4DB_ReadOnly};
        C4Error err;
        _db = c4db_open(c4str(path), &config, &err);
        if (!_db)
            fail(format("Couldn't open database %s", path.c_str()), err);
    }

    void openDatabaseFromNextArg() {
        openDatabase(nextArg("database path"));
    }


#pragma mark - QUERY:


    void queryDatabase(string) {
        // Read params:
        processFlags(kQueryFlags);
        openDatabaseFromNextArg();
        alloc_slice queryJSON = convertQuery(nextArg("query string"));
        endOfArgs();


        // Compile query:
        C4Error error;
        c4::ref<C4Query> query = c4query_new(_db, queryJSON, &error);
        if (!query)
            fail("compiling query", error);
        bool columnsSpecified = (c4query_columnCount(query) > 0);

        // Set parameters:
        alloc_slice params;
        if (_offset > 0 || _limit >= 0) {
            JSONEncoder enc;
            enc.beginDict();
            enc.writeKey("offset"_sl);
            enc.writeInt(_offset);
            enc.writeKey("limit"_sl);
            enc.writeInt(_limit);
            enc.endDict();
            params = enc.finish();
        }

        // Run query:
        c4::ref<C4QueryEnumerator> e = c4query_run(query, nullptr, params, &error);
        if (!e)
            fail("starting query", error);
        if (_offset > 0)
            cout << "(Skipping first " << _offset << " rows)\n";
        uint64_t nRows = 0;
        while (c4queryenum_next(e, &error)) {
            // Write a result row:
            ++nRows;
            cout << "[";
            int nCols = 0;
            if (!columnsSpecified) {
                cout << "\"_id\": \"" << e->docID << "\"";
                ++nCols;
            }
            for (Array::iterator i(e->columns); i; ++i) {
                if (nCols++)
                    cout << ", ";
                alloc_slice json = i.value().toJSON();
                cout << json;
            }
            cout << "]\n";
        }
        if (error.code)
            fail("running query", error);
        if (nRows == _limit)
            cout << "(Limit was " << _limit << " rows)\n";
    }


    alloc_slice convertQuery(slice inputQuery) {
        FLError flErr;
        alloc_slice queryJSONBuf = FLJSON5_ToJSON(slice(inputQuery), &flErr);
        if (!queryJSONBuf)
            fail("Invalid JSON in query");

        // Trim whitespace from either end:
        slice queryJSON = queryJSONBuf;
        while (isspace(queryJSON[0]))
            queryJSON.moveStart(1);
        while (isspace(queryJSON[queryJSON.size-1]))
            queryJSON.setSize(queryJSON.size-1);

        stringstream json;
        if (queryJSON[0] == '[')
            json << "{\"WHERE\": " << queryJSON;
        else
            json << slice(queryJSON.buf, queryJSON.size - 1);
        if (_offset > 0 || _limit >= 0)
            json << ", \"OFFSET\": [\"$offset\"], \"LIMIT\":  [\"$limit\"]";
        json << "}";
        return alloc_slice(json.str());
    }


#pragma mark - LIST DOCS:


    void listDocs(string) {
        // Read params:
        processFlags(kListFlags);
        openDatabaseFromNextArg();
        endOfArgs();

        C4Error error;
        C4EnumeratorOptions options {_offset, _enumFlags};
        c4::ref<C4DocEnumerator> e;
        if (_listBySeq)
            e = c4db_enumerateChanges(_db, 0, &options, &error);
        else
            e = c4db_enumerateAllDocs(_db, _startKey, _endKey, &options, &error);
        if (!e)
            fail("creating enumerator", error);

        if (_offset > 0)
            cout << "(Skipping first " << _offset << " docs)\n";
        int64_t nDocs = 0;
        int xpos = 0;
        while (c4enum_next(e, &error)) {
            if (++nDocs > _limit && _limit >= 0) {
                cout << "\n(Stopping after " << _limit << " docs)";
                error.code = 0;
                break;
            }
            C4DocumentInfo info;
            c4enum_getDocumentInfo(e, &info);
            int idWidth = (int)info.docID.size;        //TODO: Account for UTF-8 chars
            if (_enumFlags & kC4IncludeBodies) {
                if (nDocs > 1)
                    cout << "\n";
                c4::ref<C4Document> doc = c4enum_getDocument(e, &error);
                if (!doc)
                    fail("reading document");
                cout << "{\"_id\":\"" << doc->docID << "\"";
                alloc_slice json = c4doc_bodyAsJSON(doc, true, &error);
                slice j = json;
                j.moveStart(1);
                if (j.size > 1)
                    cout << ", ";
                cout << j;

            } else if (_longListing) {
                // Long form:
                if (nDocs == 1)
                    cout << "Document ID     Rev ID     Flags   Seq     Size\n";
                else
                    cout << "\n";
                info.revID.size = min(info.revID.size, (size_t)10);
                cout << info.docID << spaces(kListColumnWidth - idWidth);
                cout << info.revID << spaces(10 - (int)info.revID.size);
                cout << ((info.flags & kDocDeleted)        ? 'd' : '-');
                cout << ((info.flags & kDocConflicted)     ? 'c' : '-');
                cout << ((info.flags & kDocHasAttachments) ? 'a' : '-');
                cout << ' ';
                cout << setw(7) << info.sequence << " ";
                cout << setw(7) << setprecision(1) << (info.bodySize / 1024.0) << 'K';

            } else {
                // Short form:
                int lineWidth = terminalWidth();
                int nSpaces = xpos ? (kListColumnWidth - (xpos % kListColumnWidth)) : 0;
                int newXpos = xpos + nSpaces + idWidth;
                if (newXpos < lineWidth) {
                    if (xpos > 0)
                        cout << spaces(nSpaces);
                    xpos = newXpos;
                } else {
                    cout << "\n";
                    xpos = idWidth;
                }
                cout << info.docID;
            }
        }
        if (error.code)
            fail("enumerating documents", error);

        if (nDocs == 0)
            cout << "(No documents)";
        cout << "\n";
    }


    string spaces(int n) {
        return string(max(n, 1), ' ');
    }


    int terminalWidth() {
#if __APPLE__
        struct ttysize ts;
        if (ioctl(0, TIOCGSIZE, &ts) == 0 && ts.ts_cols > 0)
            return ts.ts_cols;
#endif
        return kDefaultLineWidth;
    }


#pragma mark - FILE INFO:


    void fileInfo(string) {
        // Read params:
        openDatabaseFromNextArg();
        endOfArgs();

        alloc_slice pathSlice = c4db_getPath(_db);
        auto nDocs = c4db_getDocumentCount(_db);
        auto lastSeq = c4db_getLastSequence(_db);
        alloc_slice indexesFleece = c4db_getIndexes(_db, nullptr);
        auto indexes = Value::fromData(indexesFleece).asArray();

        FilePath path(pathSlice.asString());
        uint64_t dbSize = 0, blobsSize = 0, nBlobs = 0;
        path["db.sqlite3"].forEachMatch([&](const litecore::FilePath &file) {
            dbSize += file.dataSize();
        });
        auto attachmentsPath = path["Attachments/"];
        if (attachmentsPath.exists()) {
            attachmentsPath.forEachFile([&](const litecore::FilePath &file) {
                blobsSize += file.dataSize();
            });
        }

        cout << "Database:   " << pathSlice << "\n";
        cout << "Total size: "; writeSize(dbSize + blobsSize); cerr << "\n";
        cout << "Documents:  " << nDocs << ", last sequence " << lastSeq << "\n";
        
        if (indexes.count() > 0) {
            cout << "Indexes:    ";
            int n = 0;
            for (Array::iterator i(indexes); i; ++i) {
                if (n++)
                    cout << ", ";
                cout << i.value().asString();
            }
        }

        if (nBlobs > 0) {
            cout << "Blobs:      " << nBlobs << ", "; writeSize(dbSize); cerr << "\n";
        }

        C4UUID publicUUID, privateUUID;
        if (c4db_getUUIDs(_db, &publicUUID, &privateUUID, nullptr)) {
            cout << "UUIDs:      public "
                 << slice(&publicUUID, sizeof(publicUUID)).hexString().c_str()
                 << ", private " << slice(&privateUUID, sizeof(privateUUID)).hexString().c_str()
                 << "\n";
        }
    }


    static void writeSize(uint64_t n) {
        static const char* kScales[] = {" bytes", "KB", "MB", "GB"};
        int scale = 0;
        while (n >= 1024 && scale < 3) {
            n = (n + 512) / 1024;
            ++scale;
        }
        cout << n << kScales[scale];
    }


private:
    void offsetFlag(string flag)    {_offset = stoul(nextArg("offset value"));}
    void limitFlag(string flag)     {_limit = stol(nextArg("limit value"));}
    void longListFlag(string flag)  {_longListing = true;}
    void seqFlag(string flag)       {_listBySeq = true;}
    void bodyFlag(string flag)      {_enumFlags |= kC4IncludeBodies;}
    void descFlag(string flag)      {_enumFlags |= kC4Descending;}
    void delFlag(string flag)       {_enumFlags |= kC4IncludeDeleted;}
    void confFlag(string flag)      {_enumFlags &= ~kC4IncludeNonConflicted;}

    static constexpr FlagSpec kSubcommands[] = {
        {"query",   (FlagHandler)&CBLiteTool::queryDatabase},
        {"ls",      (FlagHandler)&CBLiteTool::listDocs},
        {"file",    (FlagHandler)&CBLiteTool::fileInfo},
        {nullptr, nullptr}
    };

    static constexpr FlagSpec kQueryFlags[] = {
        {"--offset", (FlagHandler)&CBLiteTool::offsetFlag},
        {"--limit",  (FlagHandler)&CBLiteTool::limitFlag},
        {nullptr, nullptr}
    };

    static constexpr FlagSpec kListFlags[] = {
        {"--offset", (FlagHandler)&CBLiteTool::offsetFlag},
        {"--limit",  (FlagHandler)&CBLiteTool::limitFlag},
        {"-l",       (FlagHandler)&CBLiteTool::longListFlag},
        {"--body",   (FlagHandler)&CBLiteTool::bodyFlag},
        {"--desc",   (FlagHandler)&CBLiteTool::descFlag},
        {"--seq",    (FlagHandler)&CBLiteTool::seqFlag},
        {"--del",    (FlagHandler)&CBLiteTool::delFlag},
        {"--conf",   (FlagHandler)&CBLiteTool::confFlag},
        {nullptr, nullptr}
    };

    C4Database* _db {nullptr};
    uint64_t _offset {0};
    int64_t _limit {-1};
    alloc_slice _startKey, _endKey;
    C4EnumeratorFlags _enumFlags {kC4InclusiveStart | kC4InclusiveEnd | kC4IncludeNonConflicted};
    bool _longListing {false};
    bool _listBySeq {false};
};


constexpr CBLiteTool::FlagSpec CBLiteTool::kSubcommands[], CBLiteTool::kQueryFlags[],
          CBLiteTool::kListFlags[];


int main(int argc, const char * argv[]) {
    CBLiteTool tool;
    return tool.main(argc, argv);
}
