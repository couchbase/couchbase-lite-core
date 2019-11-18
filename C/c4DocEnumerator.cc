//
// c4DocEnumerator.cc
//
// Copyright (c) 2015 Couchbase, Inc All rights reserved.
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

#include "c4Internal.hh"
#include "c4DocEnumerator.h"

#include "Database.hh"
#include "Document.hh"
#include "DataFile.hh"
#include "Record.hh"
#include "RecordEnumerator.hh"
#include "Logging.hh"
#include "InstanceCounted.hh"


#pragma mark - DOC ENUMERATION:

CBL_CORE_API const C4EnumeratorOptions kC4DefaultEnumeratorOptions = {
    kC4IncludeNonConflicted | kC4IncludeBodies
};


struct C4DocEnumerator : public RecordEnumerator, fleece::InstanceCounted {
    C4DocEnumerator(C4Database *database,
                    sequence_t since,
                    const C4EnumeratorOptions &options)
    :RecordEnumerator(database->defaultKeyStore(), since, recordOptions(options))
    ,_database(database)
    { }

    C4DocEnumerator(C4Database *database,
                    const C4EnumeratorOptions &options)
    :RecordEnumerator(database->defaultKeyStore(), recordOptions(options))
    ,_database(database)
    { }

    static RecordEnumerator::Options recordOptions(const C4EnumeratorOptions &c4options) {
        RecordEnumerator::Options options;
        if (c4options.flags & kC4Descending)
            options.sortOption = kDescending;
        else if (c4options.flags & kC4Unsorted)
            options.sortOption = kUnsorted;
        options.includeDeleted = (c4options.flags & kC4IncludeDeleted) != 0;
        options.onlyConflicts  = (c4options.flags & kC4IncludeNonConflicted) == 0;
        if ((c4options.flags & kC4IncludeBodies) == 0)
            options.contentOption = kMetaOnly;
        return options;
    }

    Retained<Document> getDoc() {
        if (!hasRecord())
            return nullptr;
        return _database->documentFactory().newDocumentInstance(record());
    }

    bool getDocInfo(C4DocumentInfo *outInfo) {
        if (!*this)
            return false;
        outInfo->docID = record().key();
        outInfo->revID = _docRevID = _database->documentFactory().revIDFromVersion(record().version());
        outInfo->flags = (C4DocumentFlags)record().flags() | kDocExists;
        outInfo->sequence = record().sequence();
        outInfo->bodySize = record().bodySize();
        outInfo->expiration = record().expiration();
        return true;
    }

private:
    Retained<Database> _database;
    alloc_slice _docRevID;
};


void c4enum_close(C4DocEnumerator *e) noexcept {
    if (e)
        e->close();
}

void c4enum_free(C4DocEnumerator *e) noexcept {
    delete e;
}


C4DocEnumerator* c4db_enumerateChanges(C4Database *database,
                                       C4SequenceNumber since,
                                       const C4EnumeratorOptions *c4options,
                                       C4Error *outError) noexcept
{
    return tryCatch<C4DocEnumerator*>(outError, [&]{
        return new C4DocEnumerator(database, since,
                                   c4options ? *c4options : kC4DefaultEnumeratorOptions);
    });
}


C4DocEnumerator* c4db_enumerateAllDocs(C4Database *database,
                                       const C4EnumeratorOptions *c4options,
                                       C4Error *outError) noexcept
{
    return tryCatch<C4DocEnumerator*>(outError, [&]{
        return new C4DocEnumerator(database,
                                   c4options ? *c4options : kC4DefaultEnumeratorOptions);
    });
}


bool c4enum_next(C4DocEnumerator *e, C4Error *outError) noexcept {
    return tryCatch<bool>(outError, [&]{
        if (e->next())
            return true;
        clearError(outError);      // end of iteration is not an error
        return false;
    });
}


bool c4enum_getDocumentInfo(C4DocEnumerator *e, C4DocumentInfo *outInfo) noexcept {
    return e->getDocInfo(outInfo);
}


C4Document* c4enum_getDocument(C4DocEnumerator *e, C4Error *outError) noexcept {
    return tryCatch<C4Document*>(outError, [&]{
        Retained<Document> doc = e->getDoc();
        if (!doc)
            clearError(outError);      // end of iteration is not an error
        return retain(doc.get());
    });
}

