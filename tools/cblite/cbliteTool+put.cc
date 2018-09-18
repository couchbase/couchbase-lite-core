//
// cbliteTool+put.cc
//
// Copyright (c) 2018 Couchbase, Inc All rights reserved.
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


const Tool::FlagSpec CBLiteTool::kPutFlags[] = {
    {"--create", (FlagHandler)&CBLiteTool::createDocFlag},
    {"--update", (FlagHandler)&CBLiteTool::updateDocFlag},
    {"--delete", (FlagHandler)&CBLiteTool::deleteDocFlag},
    {nullptr, nullptr}
};

void CBLiteTool::putUsage() {
    writeUsageCommand("put", true, "DOCID \"JSON\"");
    cerr <<
    "  Updates a document.\n"
    "    --create : Document must not exist\n"
    "    --delete : Deletes the document (and JSON is optional); same as `rm` subcommand\n"
    "    --update : Document must already exist\n"
    "    " << it("DOCID") << " : Document ID\n"
    "    " << it("JSON") << " : Document body as JSON (JSON5 syntax allowed.) Must be quoted.\n"
    ;

    writeUsageCommand("rm", false, "DOCID");
    cerr <<
    "  Deletes a document. (Same as `put --delete`)\n"
    "    " << it("DOCID") << " : Document ID\n"
    ;
}


void CBLiteTool::putDoc() {
    _putMode = (_currentCommand == "rm") ? kDelete : kPut;

    // Read params:
    processFlags(kPutFlags);
    if (_showHelp) {
        putUsage();
        return;
    }

    if (_db) {
        if (_dbFlags & kC4DB_ReadOnly)
            fail("Database opened read-only; run `cblite --writeable` to allow writes");
    } else {
        _dbFlags &= ~kC4DB_ReadOnly;
        openDatabaseFromNextArg();
    }

    string docID = nextArg("document ID");
    string json5;
    if (_putMode != kDelete)
        json5 = nextArg("document body as JSON");
    endOfArgs();

    C4Error error;
    c4::Transaction t(_db);
    if (!t.begin(&error))
        fail("Couldn't open database transaction");

    c4::ref<C4Document> doc = c4doc_get(_db, slice(docID), false, &error);
    if (!doc)
        fail("Couldn't read document", error);
    bool existed = (doc->flags & kDocExists) != 0 && (doc->selectedRev.flags & kRevDeleted) == 0;
    if (!existed && (_putMode == kUpdate || _putMode == kDelete)) {
        if (doc->flags & kDocExists)
            fail("Document is already deleted");
        else
            fail("Document doesn't exist");
    }
    if (existed && _putMode == kCreate)
        fail("Document already exists");

    alloc_slice body;
    if (_putMode != kDelete) {
        alloc_slice json = FLJSON5_ToJSON(slice(json5), nullptr);
        if (!json)
            fail("Invalid JSON");
        body = c4db_encodeJSON(_db, json, &error);
        if (!body)
            fail("Couldn't encode body", error);
    }

    doc = c4doc_update(doc, body, (_putMode == kDelete ? kRevDeleted : 0), &error);
    if (!doc)
        fail("Couldn't save document", error);

    if (!t.commit(&error))
        fail("Couldn't commit database transaction", error);

    const char *verb;
    if (_putMode == kDelete)
        verb = "Deleted";
    else if (existed)
        verb = "Updated";
    else
        verb = "Created";
    string revID = slice(doc->selectedRev.revID).asString();
    if (revID.size() > 10)
        revID.resize(10);
    cout << verb << " `" << docID
         << "`, new revision " << revID << " (sequence " << doc->sequence << ")\n";
}
