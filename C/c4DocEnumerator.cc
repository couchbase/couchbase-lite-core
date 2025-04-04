//
// c4DocEnumerator.cc
//
// Copyright 2015-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//

#include "c4DocEnumerator.hh"
#include "CollectionImpl.hh"
#include "Record.hh"
#include "RecordEnumerator.hh"
#include "RevID.hh"
#include "VersionVector.hh"
#include "Error.hh"
#include "fleece/InstanceCounted.hh"

using namespace litecore;


#pragma mark - DOC ENUMERATION:

const C4EnumeratorOptions kC4DefaultEnumeratorOptions = {kC4IncludeNonConflicted | kC4IncludeBodies};

static RecordEnumerator::Options recordOptions(const C4EnumeratorOptions& c4options, slice startKey) {
    RecordEnumerator::Options options;
    if ( c4options.flags & kC4Descending ) options.sortOption = kDescending;
    else if ( c4options.flags & kC4Unsorted )
        options.sortOption = kUnsorted;
    options.includeDeleted = (c4options.flags & kC4IncludeDeleted) != 0;
    options.onlyConflicts  = (c4options.flags & kC4IncludeNonConflicted) == 0;
    if ( (c4options.flags & kC4IncludeBodies) == 0 ) options.contentOption = kMetaOnly;
    else
        options.contentOption = kEntireBody;
    options.startKey = startKey;
    return options;
}

static RecordEnumerator::Options recordOptions(const C4EnumeratorOptions& c4options, C4SequenceNumber since) {
    auto options        = recordOptions(c4options, nullslice);
    options.minSequence = since + 1;
    return options;
}

class C4DocEnumerator::Impl
    : public RecordEnumerator
    , public InstanceCounted {
  public:
    Impl(C4Collection* collection, const C4EnumeratorOptions& c4Options, const RecordEnumerator::Options& options)
        : RecordEnumerator(asInternal(collection)->keyStore(), options)
        , _collection(asInternal(collection))
        , _c4Options(c4Options) {}

    Retained<C4Document> getDoc() {
        if ( !hasRecord() ) return nullptr;
        return _collection->newDocumentInstance(record());
    }

    bool getDocInfo(C4DocumentInfo* outInfo) noexcept {
        if ( !this->hasRecord() ) return false;

        revid vers(record().version());
        if ( (_c4Options.flags & kC4IncludeRevHistory) && vers.isVersion() )
            _docRevID = vers.asVersionVector().asASCII();
        else
            _docRevID = vers.expanded();

        outInfo->docID      = record().key();
        outInfo->revID      = _docRevID;
        outInfo->flags      = (C4DocumentFlags)record().flags() | kDocExists;
        outInfo->sequence   = record().sequence();
        outInfo->bodySize   = record().bodySize();
        outInfo->metaSize   = record().extraSize();
        outInfo->expiration = record().expiration();
        return true;
    }

  private:
    litecore::CollectionImpl* _collection;
    C4EnumeratorOptions const _c4Options;
    alloc_slice               _docRevID;
};

C4DocEnumerator::C4DocEnumerator(C4Collection* collection, const C4EnumeratorOptions& options)
    : C4DocEnumerator(collection, nullslice, options) {}

C4DocEnumerator::C4DocEnumerator(C4Collection* collection, slice startKey, const C4EnumeratorOptions& options)
    : _impl(new Impl(collection, options, recordOptions(options, startKey))) {}

C4DocEnumerator::C4DocEnumerator(C4Collection* collection, C4SequenceNumber since, const C4EnumeratorOptions& options)
    : _impl(new Impl(collection, options, recordOptions(options, since))) {}

C4DocEnumerator::~C4DocEnumerator() = default;

bool C4DocEnumerator::getDocumentInfo(C4DocumentInfo& info) const noexcept { return _impl && _impl->getDocInfo(&info); }

C4DocumentInfo C4DocEnumerator::documentInfo() const {
    C4DocumentInfo i;
    if ( !getDocumentInfo(i) ) error::_throw(error::NotFound, "No more documents");
    return i;
}

Retained<C4Document> C4DocEnumerator::getDocument() const { return _impl ? _impl->getDoc() : nullptr; }

bool C4DocEnumerator::next() {
    if ( _impl && _impl->next() ) return true;
    _impl = nullptr;
    return false;
}

void C4DocEnumerator::close() noexcept { _impl = nullptr; }
