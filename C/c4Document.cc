//
//  c4Document.cc
//  CBForest
//
//  Created by Jens Alfke on 11/6/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#define NOMINMAX
#include "c4Impl.hh"
#include "c4Document.h"
#include "c4Database.h"
#include "c4Private.h"

#include "c4DocInternal.hh"

#include "forestdb.h"

using namespace cbforest;


namespace c4Internal {
    C4Document* newC4Document(C4Database *db, Document &&doc) {
        // Doesn't need to lock since Document is already in memory
        return C4DocumentInternal::newInstance(db, std::move(doc));
    }
}


bool C4DocumentInternal::mustBeSchema(int schema, C4Error *outError) {
    return _db->mustBeSchema(schema, outError);
}



void c4doc_free(C4Document *doc) {
    delete (C4DocumentInternal*)doc;
}


C4Document* c4doc_get(C4Database *database,
                      C4Slice docID,
                      bool mustExist,
                      C4Error *outError)
{
    try {
        WITH_LOCK(database);
        auto doc = C4DocumentInternal::newInstance(database, docID);
        if (mustExist && !doc->exists()) {
            delete doc;
            doc = NULL;
            recordError(ForestDBDomain, FDB_RESULT_KEY_NOT_FOUND, outError);
        }
        
        
        return doc;
    } catchError(outError);
    return NULL;
}


C4Document* c4doc_getBySequence(C4Database *database,
                                C4SequenceNumber sequence,
                                C4Error *outError)
{
    try {
        WITH_LOCK(database);
        auto doc = C4DocumentInternal::newInstance(database, database->defaultKeyStore().get(sequence));
        if (!doc->exists()) {
            delete doc;
            doc = NULL;
            recordError(ForestDBDomain, FDB_RESULT_KEY_NOT_FOUND, outError);
        }
        return doc;
    } catchError(outError);
    return NULL;
}


C4SliceResult c4doc_getType(C4Document *doc) {
    slice result = internal(doc)->type().copy();
    return {result.buf, result.size};
}

void c4doc_setType(C4Document *doc, C4Slice docType) {
    return internal(doc)->setType(docType);
}


#pragma mark - REVISIONS:


bool c4doc_selectRevision(C4Document* doc,
                          C4Slice revID,
                          bool withBody,
                          C4Error *outError)
{
    try {
        internal(doc)->selectRevision(revID, withBody);
        return true;
    } catchError(outError);
    return false;
}


bool c4doc_selectCurrentRevision(C4Document* doc)
{
    return internal(doc)->selectCurrentRevision();
}


bool c4doc_loadRevisionBody(C4Document* doc, C4Error *outError) {
    try {
        if (internal(doc)->loadSelectedRevBodyIfAvailable())
            return true;
        recordHTTPError(kC4HTTPGone, outError);
    } catchError(outError);
    return false;
}


bool c4doc_hasRevisionBody(C4Document* doc) {
    try {
        return internal(doc)->hasRevisionBody();
    } catchError(NULL);
    return false;
}


bool c4doc_selectParentRevision(C4Document* doc) {
    return internal(doc)->selectParentRevision();
}


bool c4doc_selectNextRevision(C4Document* doc) {
    return internal(doc)->selectNextRevision();
}


bool c4doc_selectNextLeafRevision(C4Document* doc,
                                  bool includeDeleted,
                                  bool withBody,
                                  C4Error *outError)
{
    try {
        if (internal(doc)->selectNextLeafRevision(includeDeleted, withBody))
            return true;
        clearError(outError); // normal failure
    } catchError(outError);
    return false;
}


unsigned c4rev_getGeneration(C4Slice revID) {
    try {
        return revidBuffer(revID).generation();
    }catchError(NULL)
    return 0;
}


#pragma mark - SAVING:


int32_t c4doc_purgeRevision(C4Document *doc,
                            C4Slice revID,
                            C4Error *outError)
{
    auto idoc = internal(doc);
    if (!idoc->mustBeInTransaction(outError))
        return -1;
    try {
        idoc->loadRevisions();
        return idoc->purgeRevision(revID);
    } catchError(outError)
    return -1;
}
