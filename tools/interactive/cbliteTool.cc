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
#include <fnmatch.h>        // POSIX (?)
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <vector>

#if __APPLE__
#include <sys/ioctl.h>
#endif

using namespace std;
using namespace fleeceapi;


static const int kListColumnWidth = 16;
static const int kDefaultLineWidth = 100;


class CBLiteTool : public Tool {
public:
    CBLiteTool() {
    }

    virtual ~CBLiteTool() {
        c4db_free(_db);
    }


    string it(const char *str) {
        return ansiItalic() + str + ansiReset();
    }

    void usage() override {
        cerr <<
        ansiBold() << "cblite: Couchbase Lite / LiteCore database multi-tool\n" << ansiReset() <<
        "Usage: cblite query " << it("[FLAGS] DBPATH JSONQUERY") << "\n"
        "       cblite ls " << it("[FLAGS] DBPATH [PATTERN]") << "\n"
        "       cblite cat " << it("[FLAGS] DBPATH DOCID [DOCID...]") << "\n"
        "       cblite revs " << it("DBPATH DOCID") << "\n"
        "       cblite file " << it("DBPATH") << "\n"
        "       cblite help " << it("[SUBCOMMAND]") << "\n"
        "       cblite " << it("DBPATH") << "   (interactive shell)\n"
        "           The shell accepts the same commands listed above, but without the\n"
        "           'cblite' and DBPATH parameters. For example, 'ls -l'.\n"
        "   For information about parameters, run `cblite help`.\n"
        ;
    }

    int run() override {
        c4log_setCallbackLevel(kC4LogWarning);
        clearFlags();
        if (argCount() == 0) {
            cerr << "Missing subcommand or database path.\n"
                 << "For a list of subcommands, run " << ansiBold() << "cblite help" << ansiReset() << ".\n"
                 << "To start the interactive mode, run "
                 << ansiBold() << "cblite " << ansiItalic() << "DBPATH" << ansiReset() << '\n';
            fail();
        }
        string cmd = nextArg("subcommand");
        if (hasSuffix(cmd, ".cblite2")) {
            endOfArgs();
            openDatabase(cmd);
            runInteractively();
        } else {
            if (!processFlag(cmd, kSubcommands))
                failMisuse(format("Unknown subcommand '%s'", cmd.c_str()));
        }
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
        if (!_db)
            openDatabase(nextArg("database path"));
    }


#pragma mark - QUERY:

    void writeUsageCommand(const char *cmd, bool hasFlags, const char *otherArgs ="") {
        cerr << ansiBold();
        if (!_interactive)
            cerr << "cblite ";
        cerr << cmd << ' ' << ansiItalic();
        if (hasFlags)
            cerr << "[FLAGS]" << ' ';
        if (!_interactive)
            cerr << "DBPATH ";
        cerr << otherArgs << ansiReset() << "\n";
    }

    void queryUsage() {
        writeUsageCommand("query", true, "JSONQUERY");
        cerr <<
        "  Runs a query against the database."
        "    --offset N : Skip first N rows\n"
        "    --limit N : Stop after N rows\n"
        "    " << it("JSONQUERY") << " : LiteCore JSON (or JSON5) query expression\n"
        ;
    }


    void queryDatabase() {
        // Read params:
        processFlags(kQueryFlags);
        if (_showHelp) {
            queryUsage();
            return;
        }
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


    void listUsage() {
        writeUsageCommand("ls", true, "[PATTERN]");
        cerr <<
        "  Lists the IDs, and optionally other metadata, of the documents in the database.\n"
        "    -l : Long format (one doc per line, with metadata)\n"
        "    --offset N : Skip first N docs\n"
        "    --limit N : Stop after N docs\n"
        "    --desc : Descending order\n"
        "    --seq : Order by sequence, not docID\n"
        "    --del : Include deleted documents\n"
        "    --conf : Show only conflicted documents\n"
        "    --body : Display document bodies\n"
        "    --pretty : Pretty-print document bodies (implies --body)\n"
        "    --json5 : JSON5 syntax, i.e. unquoted dict keys (implies --body)\n"
        "    " << it("PATTERN") << " : pattern for matching docIDs, with shell-style wildcards '*', '?'\n"
        ;
    }


    void listDocsCommand() {
        // Read params:
        _prettyPrint = false;
        processFlags(kListFlags);
        if (_showHelp) {
            listUsage();
            return;
        }
        openDatabaseFromNextArg();
        string docIDPattern;
        if (argCount() > 0)
            docIDPattern = nextArg("docID pattern");
        endOfArgs();

        listDocs(docIDPattern);
    }


    void listDocs(string docIDPattern) {
        C4Error error;
        C4EnumeratorOptions options {_offset, _enumFlags};
        c4::ref<C4DocEnumerator> e;
        if (_listBySeq)
            e = c4db_enumerateChanges(_db, 0, &options, &error);
        else
            e = c4db_enumerateAllDocs(_db, _startKey, _endKey, &options, &error);
        if (!e)
            fail("creating enumerator", error);

        if (_offset > 0) {
            cout << "(Skipping first " << _offset << " docs)\n";
            if (!docIDPattern.empty())
                options.skip = 0;           // need to skip manually if there's a pattern to match
        }

        int64_t nDocs = 0;
        int xpos = 0;
        while (c4enum_next(e, &error)) {
            C4DocumentInfo info;
            c4enum_getDocumentInfo(e, &info);

            if (!docIDPattern.empty()) {
                // Check whether docID matches pattern:
                string docID = slice(info.docID).asString();
                if (fnmatch(docIDPattern.c_str(), docID.c_str(), 0) != 0)
                    continue;
                if (_offset > 0) {
                    --_offset;
                    continue;
                }
            }

            if (++nDocs > _limit && _limit >= 0) {
                cout << "\n(Stopping after " << _limit << " docs)";
                error.code = 0;
                break;
            }
            
            int idWidth = (int)info.docID.size;        //TODO: Account for UTF-8 chars
            if (_enumFlags & kC4IncludeBodies) {
                if (nDocs > 1)
                    cout << "\n";
                c4::ref<C4Document> doc = c4enum_getDocument(e, &error);
                if (!doc)
                    fail("reading document");
                catDoc(doc, true);

            } else if (_longListing) {
                // Long form:
                if (nDocs == 1) {
                    cout << ansi("4") << "Document ID     Rev ID     Flags   Seq     Size" << ansiReset() << "\n";
                } else {
                    cout << "\n";
                }
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

        if (nDocs == 0) {
            if (docIDPattern.empty())
                cout << "(No documents)";
            else
                cout << "(No documents with IDs matching \"" << docIDPattern << "\")";
        }
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


#pragma mark - CAT:


    void catUsage() {
        writeUsageCommand("cat", true, "DOCID [DOCID...]");
        cerr <<
        "  Displays the bodies of documents in JSON form.\n"
        "    --raw : Raw JSON (not pretty-printed)\n"
        "    --json5 : JSON5 syntax (no quotes around dict keys)\n"
        "    " << it("DOCID") << " : Document ID, or pattern if it includes '*' or '?'\n"
        ;
    }


    void catDocs() {
        // Read params:
        processFlags(kCatFlags);
        if (_showHelp) {
            catUsage();
            return;
        }
        openDatabaseFromNextArg();

        bool includeIDs = (argCount() > 1);
        while (argCount() > 0) {
            string docID = nextArg("document ID");
            if (isGlobPattern(docID)) {
                _enumFlags |= kC4IncludeBodies; // force displaying doc bodies
                listDocs(docID);
            } else {
                unquoteGlobPattern(docID); // remove any protective backslashes
                c4::ref<C4Document> doc = readDoc(docID);
                if (doc) {
                    catDoc(doc, includeIDs);
                    cout << '\n';
                }
            }
        }
    }


    c4::ref<C4Document> readDoc(string docID) {
        C4Error error;
        c4::ref<C4Document> doc = c4doc_get(_db, slice(docID), true, &error);
        if (!doc) {
            if (error.domain == LiteCoreDomain && error.code == kC4ErrorNotFound)
                cerr << "Error: Document \"" << docID << "\" not found.\n";
            else
                errorOccurred(format("reading document \"%s\"", docID.c_str()), error);
        }
        return doc;
    }


    void catDoc(C4Document *doc, bool includeID) {
        Value body = Value::fromData(doc->selectedRev.body);
        slice docID = (includeID ? slice(doc->docID) : nullslice);
        if (_prettyPrint)
            prettyPrint(body, "", docID);
        else
            rawPrint(body, docID);
    }


    void rawPrint(Value body, slice docID) {
        FLSharedKeys sharedKeys = c4db_getFLSharedKeys(_db);
        alloc_slice jsonBuf = body.toJSON(sharedKeys, _json5, true);
        slice restOfJSON = jsonBuf;
        if (docID) {
            // Splice a synthesized "_id" property into the start of the JSON object:
            cout << "{" << ansiDim() << ansiItalic()
                 << (_json5 ? "_id" : "\"_id\"") << ":\""
                 << ansiReset() << ansiDim()
                 << docID << "\"";
            restOfJSON.moveStart(1);
            if (restOfJSON.size > 1)
                cout << ", ";
            cout << ansiReset();
        }
        cout << restOfJSON;
    }


    void prettyPrint(Value value, const string &indent ="", slice docID =nullslice) {
        // TODO: Support an includeID option
        switch (value.type()) {
            case kFLDict: {
                auto sk = c4db_getFLSharedKeys(_db);
                string subIndent = indent + "  ";
                cout << "{\n";
                if (docID) {
                    cout << subIndent << ansiDim() << ansiItalic();
                    cout << (_json5 ? "_id" : "\"_id\"");
                    cout << ansiReset() << ansiDim() << ": \"" << docID << "\"";
                    if (value.asDict().count() > 0)
                        cout << ',';
                    cout << ansiReset() << '\n';
                }
                for (Dict::iterator i(value.asDict(), sk); i; ++i) {
                    cout << subIndent << ansiItalic();
                    slice key = i.keyString();
                    if (_json5 && canBeUnquotedJSON5Key(key))
                        cout << key;
                    else
                        cout << '"' << key << '"';      //FIX: Escape quotes
                    cout << ansiReset() << ": ";

                    prettyPrint(i.value(), subIndent);
                    if (i.count() > 1)
                        cout << ',';
                    cout << '\n';
                }
                cout << indent << "}";
                break;
            }
            case kFLArray: {
                string subIndent = indent + "  ";
                cout << "[\n";
                for (Array::iterator i(value.asArray()); i; ++i) {
                    cout << subIndent;
                    prettyPrint(i.value(), subIndent);
                    if (i.count() > 1)
                        cout << ',';
                    cout << '\n';
                    }
                cout << indent << "]";
                break;
            }
            default: {
                alloc_slice json(value.toJSON());
                cout << json;
                break;
            }
        }
    }

    static bool canBeUnquotedJSON5Key(slice key) {
        if (key.size == 0 || isdigit(key[0]))
            return false;
        for (unsigned i = 0; i < key.size; i++) {
            if (!isalnum(key[i]) && key[i] != '_' && key[i] != '$')
                return false;
        }
        return true;
    }


    static bool isGlobPattern(string &str) {
        size_t size = str.size();
        for (size_t i = 0; i < size; ++i) {
            char c = str[i];
            if ((c == '*' || c == '?') && (i == 0 || str[i-1] != '\\'))
                return true;
        }
        return false;
    }

    static void unquoteGlobPattern(string &str) {
        size_t size = str.size();
        for (size_t i = 0; i < size; ++i) {
            if (str[i] == '\\') {
                str.erase(i, 1);
                --size;
            }
        }
    }


#pragma mark - DOCUMENT INFO:


    void revsUsage() {
        writeUsageCommand("revs", false, "DOCID");
        cerr <<
        "  Shows a document's revision history\n"
        ;
    }


    using RevTree = map<alloc_slice,set<alloc_slice>>; // Maps revID to set of child revIDs

    void revsInfo() {
        // Read params:
        processFlags(nullptr);
        if (_showHelp) {
            revsUsage();
            return;
        }
        openDatabaseFromNextArg();
        string docID = nextArg("document ID");
        endOfArgs();

        auto doc = readDoc(docID);
        if (!doc)
            return;

        cout << "Document \"" << ansiBold() << doc->docID << ansiReset()
             << "\", current revID " << ansiBold() << doc->revID << ansiReset()
             << ", sequence #" << doc->sequence;
        if (doc->flags & kDocDeleted)
            cout << ", Deleted";
        if (doc->flags & kDocConflicted)
            cout << ", Conflicted";
        if (doc->flags & kDocHasAttachments)
            cout << ", Has Attachments";
        cout << "\n";

        // Collect revision tree info:
        RevTree tree;
        alloc_slice root; // use empty slice as root of tree

        do {
            alloc_slice leafRevID = doc->selectedRev.revID;
            alloc_slice childID = leafRevID;
            while (c4doc_selectParentRevision(doc)) {
                alloc_slice parentID = doc->selectedRev.revID;
                tree[parentID].insert(childID);
                childID = parentID;
            }
            tree[root].insert(childID);
            c4doc_selectRevision(doc, leafRevID, false, nullptr);
        } while (c4doc_selectNextLeafRevision(doc, true, true, nullptr));

        writeRevisionChildren(doc, tree, root, "");
    }


    void writeRevisionTree(C4Document *doc,
                           RevTree &tree,
                           alloc_slice root,
                           const string &indent)
    {
        static const char* const kRevFlagName[7] = {
            "Deleted", "Leaf", "New", "Attach", "KeepBody", "Conflict", "Foreign"
        };
        C4Error error;
        if (!c4doc_selectRevision(doc, root, true, &error))
            fail("accessing revision", error);
        auto &rev = doc->selectedRev;
        cout << indent << "* ";
        if (rev.flags & kRevLeaf)
            cout << ansiBold();
        cout << rev.revID << ansiReset() << " (#" << rev.sequence << ")";
        if (rev.body.buf)
            cout << ", " << rev.body.size << " bytes";
        for (int bit = 0; bit < 7; bit++) {
            if (rev.flags & (1 << bit))
                cout << ", " << kRevFlagName[bit];
        }
        cout << "\n";
        writeRevisionChildren(doc, tree, root, indent + "  ");
    }

    void writeRevisionChildren(C4Document *doc,
                               RevTree &tree,
                               alloc_slice root,
                               const string &indent)
    {
        auto &children = tree[root];
        for (auto i = children.rbegin(); i != children.rend(); ++i) {
            writeRevisionTree(doc, tree, *i, indent);
        }
    }


#pragma mark - FILE INFO:


    void fileUsage() {
        writeUsageCommand("file", false);
        cerr <<
        "  Displays information about the database\n"
        ;
    }


    void fileInfo() {
        // Read params:
        processFlags(nullptr);
        if (_showHelp) {
            fileUsage();
            return;
        }
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
            cout << "\n";
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


#pragma mark - INTERACTIVE MODE:


    void shell() {
        // Read params:
        openDatabaseFromNextArg();
        endOfArgs();
        runInteractively();
    }


    void runInteractively() {
        _interactive = true;
        cout << "Opened database " << alloc_slice(c4db_getPath(_db)) << '\n';

        while(true) {
            try {
                if (!readLine("(cblite) "))
                    return;
                string cmd = nextArg("subcommand");
                clearFlags();
                if (!processFlag(cmd, kInteractiveSubcommands))
                    cerr << format("Unknown subcommand '%s'; type 'help' for a list of commands.\n",
                                   cmd.c_str());
            } catch (const fail_error &x) {
                // subcommand failed (error message was already printed); continue
            }
        }
    }


    void helpCommand() {
        if (argCount() > 0) {
            _showHelp = true; // forces command to show help and return
            string cmd = nextArg("subcommand");
            if (!processFlag(cmd, kInteractiveSubcommands))
                cerr << format("Unknown subcommand '%s'\n", cmd.c_str());
        } else {
            listUsage();
            catUsage();
            revsUsage();
            fileUsage();
            queryUsage();
            if (_interactive)
                cerr << ansiBold() << "help " << it("[COMMAND]") << ansiReset() << '\n'
                     << ansiBold() << "quit" << ansiReset() << "  (or Ctrl-D)\n";
            else {
                cerr <<
                ansiBold() << "cblite help [SUBCOMMAND]\n" << ansiReset() <<
                "  Displays help for a command, or for all commands.\n" <<
                ansiBold() << "cblite DBPATH\n" << ansiReset() <<
                "  Starts an interactive shell where you can run multiple commands on the same database.\n";
            }
        }
    }


    void quitCommand() {
        exit(0);
    }


#pragma mark - FLAGS:


    void clearFlags() {
        _offset = 0;
        _limit = -1;
        _startKey = _endKey = nullslice;
        _enumFlags = kC4InclusiveStart | kC4InclusiveEnd | kC4IncludeNonConflicted;
        _longListing = _listBySeq = false;
        _prettyPrint = true;
        _json5 = false;
        _showHelp = false;
    }


    void offsetFlag()    {_offset = stoul(nextArg("offset value"));}
    void limitFlag()     {_limit = stol(nextArg("limit value"));}
    void longListFlag()  {_longListing = true;}
    void seqFlag()       {_listBySeq = true;}
    void bodyFlag()      {_enumFlags |= kC4IncludeBodies;}
    void descFlag()      {_enumFlags |= kC4Descending;}
    void delFlag()       {_enumFlags |= kC4IncludeDeleted;}
    void confFlag()      {_enumFlags &= ~kC4IncludeNonConflicted;}
    void prettyFlag()    {_prettyPrint = true; _enumFlags |= kC4IncludeBodies;}
    void json5Flag()     {_json5 = true; _enumFlags |= kC4IncludeBodies;}
    void rawFlag()       {_prettyPrint = false; _enumFlags |= kC4IncludeBodies;}
    void helpFlag()      {_showHelp = true;}

    static constexpr FlagSpec kSubcommands[] = {
        {"query",   (FlagHandler)&CBLiteTool::queryDatabase},
        {"ls",      (FlagHandler)&CBLiteTool::listDocsCommand},
        {"cat",     (FlagHandler)&CBLiteTool::catDocs},
        {"revs",    (FlagHandler)&CBLiteTool::revsInfo},
        {"file",    (FlagHandler)&CBLiteTool::fileInfo},
        {"shell",   (FlagHandler)&CBLiteTool::shell},
        {"help",    (FlagHandler)&CBLiteTool::helpCommand},
        {nullptr, nullptr}
    };

    static constexpr FlagSpec kInteractiveSubcommands[] = {
        {"query",   (FlagHandler)&CBLiteTool::queryDatabase},
        {"ls",      (FlagHandler)&CBLiteTool::listDocsCommand},
        {"cat",     (FlagHandler)&CBLiteTool::catDocs},
        {"revs",    (FlagHandler)&CBLiteTool::revsInfo},
        {"file",    (FlagHandler)&CBLiteTool::fileInfo},
        {"help",    (FlagHandler)&CBLiteTool::helpCommand},
        {"quit",    (FlagHandler)&CBLiteTool::quitCommand},
        {nullptr, nullptr}
    };

    static constexpr FlagSpec kQueryFlags[] = {
        {"--offset", (FlagHandler)&CBLiteTool::offsetFlag},
        {"--limit",  (FlagHandler)&CBLiteTool::limitFlag},
        {"--help",   (FlagHandler)&CBLiteTool::helpFlag},
        {nullptr, nullptr}
    };

    static constexpr FlagSpec kListFlags[] = {
        {"--offset", (FlagHandler)&CBLiteTool::offsetFlag},
        {"--limit",  (FlagHandler)&CBLiteTool::limitFlag},
        {"-l",       (FlagHandler)&CBLiteTool::longListFlag},
        {"--body",   (FlagHandler)&CBLiteTool::bodyFlag},
        {"--pretty", (FlagHandler)&CBLiteTool::prettyFlag},
        {"--raw",    (FlagHandler)&CBLiteTool::rawFlag},
        {"--json5",  (FlagHandler)&CBLiteTool::json5Flag},
        {"--desc",   (FlagHandler)&CBLiteTool::descFlag},
        {"--seq",    (FlagHandler)&CBLiteTool::seqFlag},
        {"--del",    (FlagHandler)&CBLiteTool::delFlag},
        {"--conf",   (FlagHandler)&CBLiteTool::confFlag},
        {"--help",   (FlagHandler)&CBLiteTool::helpFlag},
        {nullptr, nullptr}
    };

    static constexpr FlagSpec kCatFlags[] = {
        {"--pretty", (FlagHandler)&CBLiteTool::prettyFlag},
        {"--raw",    (FlagHandler)&CBLiteTool::rawFlag},
        {"--json5",  (FlagHandler)&CBLiteTool::json5Flag},
        {nullptr, nullptr}
    };

    C4Database* _db {nullptr};
    bool _interactive {false};
    uint64_t _offset {0};
    int64_t _limit {-1};
    alloc_slice _startKey, _endKey;
    C4EnumeratorFlags _enumFlags {kC4InclusiveStart | kC4InclusiveEnd | kC4IncludeNonConflicted};
    bool _longListing {false};
    bool _listBySeq {false};
    bool _prettyPrint {true};
    bool _json5 {false};
    bool _showHelp {false};
};


constexpr CBLiteTool::FlagSpec CBLiteTool::kSubcommands[], CBLiteTool::kInteractiveSubcommands[],
                               CBLiteTool::kQueryFlags[],
                               CBLiteTool::kListFlags[],
                               CBLiteTool::kCatFlags[];


int main(int argc, const char * argv[]) {
    CBLiteTool tool;
    return tool.main(argc, argv);
}
