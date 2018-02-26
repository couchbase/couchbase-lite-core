//
// cbliteTool+cat.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "cbliteTool.hh"
#include <algorithm>


void CBLiteTool::catUsage() {
    writeUsageCommand("cat", true, "DOCID [DOCID...]");
    cerr <<
    "  Displays the bodies of documents in JSON form.\n"
    "    --key KEY : Display only a single key/value (may be used multiple times)\n"
    "    --rev : Show the revision ID(s)\n"
    "    --raw : Raw JSON (not pretty-printed)\n"
    "    --json5 : JSON5 syntax (no quotes around dict keys)\n"
    "    " << it("DOCID") << " : Document ID, or pattern if it includes '*' or '?'\n"
    ;
}


void CBLiteTool::catDocs() {
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


c4::ref<C4Document> CBLiteTool::readDoc(string docID) {
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


void CBLiteTool::catDoc(C4Document *doc, bool includeID) {
    Value body = Value::fromData(doc->selectedRev.body);
    slice docID, revID;
    if (includeID || _showRevID)
        docID = slice(doc->docID);
    if (_showRevID)
        revID = (slice)doc->selectedRev.revID;
    if (_prettyPrint)
        prettyPrint(body, "", docID, revID, (_keys.empty() ? nullptr : &_keys));
    else
        rawPrint(body, docID, revID);
}


void CBLiteTool::rawPrint(Value body, slice docID, slice revID) {
    FLSharedKeys sharedKeys = c4db_getFLSharedKeys(_db);
    alloc_slice jsonBuf = body.toJSON(sharedKeys, _json5, true);
    slice restOfJSON = jsonBuf;
    if (docID) {
        // Splice a synthesized "_id" property into the start of the JSON object:
        cout << "{" << ansiDim() << ansiItalic()
             << (_json5 ? "_id" : "\"_id\"") << ":\""
             << ansiReset() << ansiDim()
             << docID << "\"";
        if (revID) {
            cout << "," << ansiItalic()
                 << (_json5 ? "_rev" : "\"_rev\"") << ":\""
                 << ansiReset() << ansiDim()
                 << revID << "\"";
        }
        restOfJSON.moveStart(1);
        if (restOfJSON.size > 1)
            cout << ", ";
        cout << ansiReset();
    }
    cout << restOfJSON;
}


void CBLiteTool::prettyPrint(Value value,
                             const string &indent,
                             slice docID,
                             slice revID,
                             const set<alloc_slice> *onlyKeys) {
    // TODO: Support an includeID option
    switch (value.type()) {
        case kFLDict: {
            auto dict = value.asDict();
            auto sk = c4db_getFLSharedKeys(_db);
            string subIndent = indent + "  ";
            int n = 0;
            cout << "{";
            if (docID) {
                n++;
                cout << '\n' << subIndent << ansiDim() << ansiItalic();
                cout << (_json5 ? "_id" : "\"_id\"");
                cout << ansiReset() << ansiDim() << ": \"" << docID << "\"";
                if (revID) {
                    n++;
                    cout << ",\n" << subIndent << ansiItalic();
                    cout << (_json5 ? "_rev" : "\"_rev\"");
                    cout << ansiReset() << ansiDim() << ": \"" << revID << "\"";
                }
            }
            vector<slice> keys;
            for (Dict::iterator i(dict, sk); i; ++i) {
                slice key = i.keyString();
                if (!onlyKeys || onlyKeys->find(alloc_slice(key)) != onlyKeys->end())
                    keys.push_back(key);
            }
            sort(keys.begin(), keys.end());
            for (slice key : keys) {
                if (n++ > 0)
                    cout << ',' << ansiReset();
                cout << '\n' << subIndent << ansiItalic();
                if (_json5 && canBeUnquotedJSON5Key(key))
                    cout << key;
                else
                    cout << '"' << key << '"';      //FIX: Escape quotes
                cout << ansiReset() << ": ";

                prettyPrint(dict.get(key, sk), subIndent);
            }
            cout << '\n' << indent << "}";
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
        case kFLData: {
            // toJSON would convert to base64, which isn't readable, so use escapes instead:
            static const char kHexDigits[17] = "0123456789abcdef";
            slice data = value.asData();
            auto end = (const uint8_t*)data.end();
            cout << "«";
            bool dim = false;
            for (auto c = (const uint8_t*)data.buf; c != end; ++c) {
                if (*c >= 32 && *c < 127) {
                    if (dim)
                        cout << ansiReset();
                    dim = false;
                    cout << (char)*c;
                } else {
                    if (!dim)
                        cout << ansiDim();
                    dim = true;
                    cout << '\\' << kHexDigits[*c/16] << kHexDigits[*c%16];
                }
            }
            if (dim)
                cout << ansiReset();
            cout << "»";
            break;
        }
        default: {
            alloc_slice json(value.toJSON());
            cout << json;
            break;
        }
    }
}

bool CBLiteTool::canBeUnquotedJSON5Key(slice key) {
    if (key.size == 0 || isdigit(key[0]))
        return false;
    for (unsigned i = 0; i < key.size; i++) {
        if (!isalnum(key[i]) && key[i] != '_' && key[i] != '$')
            return false;
    }
    return true;
}


bool CBLiteTool::isGlobPattern(string &str) {
    size_t size = str.size();
    for (size_t i = 0; i < size; ++i) {
        char c = str[i];
        if ((c == '*' || c == '?') && (i == 0 || str[i-1] != '\\'))
            return true;
    }
    return false;
}

void CBLiteTool::unquoteGlobPattern(string &str) {
    size_t size = str.size();
    for (size_t i = 0; i < size; ++i) {
        if (str[i] == '\\') {
            str.erase(i, 1);
            --size;
        }
    }
}
