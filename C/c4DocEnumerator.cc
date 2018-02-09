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
#include <set>


#pragma mark - DOC ENUMERATION:

CBL_CORE_API const C4EnumeratorOptions kC4DefaultEnumeratorOptions = {
    kC4IncludeNonConflicted | kC4IncludeBodies
};


struct C4DocEnumerator: C4InstanceCounted {
    C4DocEnumerator(C4Database *database,
                    sequence_t since,
                    const C4EnumeratorOptions &options)
    :_database(database),
     _e(database->defaultKeyStore(), since, allDocOptions(options)),
     _options(options)
    { }

    C4DocEnumerator(C4Database *database,
                    const C4EnumeratorOptions &options)
    :_database(database),
     _e(database->defaultKeyStore(), allDocOptions(options)),
     _options(options)
    { }

    void close() {
        _e.close();
    }

    static RecordEnumerator::Options allDocOptions(const C4EnumeratorOptions &c4options) {
        RecordEnumerator::Options options;
        options.descending      = (c4options.flags & kC4Descending) != 0;
        options.includeDeleted  = (c4options.flags & kC4IncludeDeleted) != 0;
        if ((c4options.flags & kC4IncludeBodies) == 0)
            options.contentOptions = kMetaOnly;
        return options;
    }

    void setFilter(const EnumFilter &f)  {_filter = f;}

    C4Database* database() const {return external(_database);}

    bool next() {
        do {
            if (!_e.next())
                return false;
        } while (!useDoc());
        return true;
    }

    C4Document* getDoc() {
        return _e ? _database->documentFactory().newDocumentInstance(_e.record()) : nullptr;
    }

    bool getDocInfo(C4DocumentInfo *outInfo) {
        if (!_e)
            return false;
        outInfo->docID = _e.record().key();
        outInfo->revID = _docRevID;
        outInfo->flags = _docFlags;
        outInfo->sequence = _e.record().sequence();
        outInfo->bodySize = _e.record().bodySize();
        return true;
    }

private:
    inline bool useDoc() {
        auto &rec = _e.record();
        if (!rec.exists()) {
            // Client must be enumerating a list of docIDs, and this doc doesn't exist.
            // Return it anyway, without the kDocExists flag.
            _docFlags = 0;
            _docRevID = nullslice;
            return (!_filter || _filter(rec, 0));
        }
        _docRevID = _database->documentFactory().revIDFromVersion(rec.version());
        _docFlags = (C4DocumentFlags)rec.flags() | kDocExists;
        auto optFlags = _options.flags;
        return (optFlags & kC4IncludeNonConflicted ||  (_docFlags & ::kDocConflicted))
            && (!_filter || _filter(rec, _docFlags));
    }

    Retained<Database> _database;
    RecordEnumerator _e;
    C4EnumeratorOptions _options;
    EnumFilter _filter;

    C4DocumentFlags _docFlags;
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


namespace c4Internal {
    void setEnumFilter(C4DocEnumerator *e, EnumFilter f) {
        e->setFilter(f);
    }
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
        auto c4doc = e->getDoc();
        if (!c4doc)
            clearError(outError);      // end of iteration is not an error
        return c4doc;
    });
    return nullptr;
}

