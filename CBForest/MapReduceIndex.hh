//
//  MapReduceIndex.hh
//  CBForest
//
//  Created by Jens Alfke on 5/15/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#ifndef __CBForest__MapReduceIndex__
#define __CBForest__MapReduceIndex__

#include "Index.hh"
#include "Geohash.hh"
#include <set>
#include <vector>


namespace cbforest {

    class MapReduceIndexWriter;

    /** An Index that uses a MapFn to index the documents of another KeyStore. */
    class MapReduceIndex : public Index {
    public:
        MapReduceIndex(Database*,
                       std::string name,
                       Database *sourceDatabase);

        KeyStore& sourceStore() const           {return _sourceDatabase->defaultKeyStore();}
        void readState();
        int indexType() const                   {return _indexType;}
        
        void setup(int indexType, std::string mapVersion);

        void setDocumentType(slice docType)     {_documentType = docType;}
        alloc_slice documentType() const        {return _documentType;}

        /** The last source database sequence number to be indexed. */
        sequence lastSequenceIndexed() const;

        /** The last source database sequence number at which the index actually changed. (<= lastSequenceIndexed.) */
        sequence lastSequenceChangedAt() const;

        /** The number of rows in the index. */
        uint64_t rowCount() const;

        /** Removes all the data in the index. */
        void erase();

        /** Reads the full text passed to the call to emitTextTokens(), given some info about the
            document and the fullTextID available from IndexEnumerator::getTextToken(). */
        alloc_slice readFullText(slice docID, sequence seq, unsigned fullTextID) const;

        /** Reads the value that was emitted along with a full-text key. */
        alloc_slice readFullTextValue(slice docID, sequence seq, unsigned fullTextID) const;

        void readGeoArea(slice docID, sequence seq, unsigned geoID,
                         geohash::area &outArea,
                         alloc_slice& outGeoJSON,
                         alloc_slice& outValue);

    private:
        bool checkForPurge();
        void invalidate();
        void deleted();
        void saveState(Transaction& t);
        alloc_slice getSpecialEntry(slice docID, sequence, unsigned fullTextID) const;

        Database* const _sourceDatabase;
        std::string _mapVersion, _lastMapVersion;
        int _indexType {0};
        sequence _lastSequenceIndexed {0}, _lastSequenceChangedAt {0};
        sequence _stateReadAt {0}; // index sequence # at which state was last valid
        uint64_t _lastPurgeCount {0};   // db lastPurgeCount when index was last built
        uint64_t _rowCount {0};
        alloc_slice _documentType;

        friend class MapReduceIndexer;
        friend class MapReduceIndexWriter;
    };


    /** An activity that updates one or more map-reduce indexes. */
    class MapReduceIndexer {
    public:
        ~MapReduceIndexer();

        void addIndex(MapReduceIndex*);

        /** If set, indexing will only occur if this index needs to be updated. */
        void triggerOnIndex(MapReduceIndex* index)  {_triggerIndex = index;}

        /** Determines at which sequence indexing should start.
            Returns UINT64_MAX if no re-indexing is necessary. */
        sequence startingSequence();

        /** Returns the set of document types that the views collectively map,
            or NULL if all documents should be mapped. */
        std::set<slice> *documentTypes();

        /** Returns true if the given document should be indexed by the given view,
            i.e. if the view has not yet indexed this doc's sequence. */
        bool shouldMapDocIntoView(const Document &doc, unsigned viewNumber);

        bool shouldMapDocTypeIntoView(slice docType, unsigned viewNumber);

        /** Writes a set of key/value pairs into a view's index, associated with a doc/sequence.
            This must be called even if there are no pairs to index, so that obsolete index rows
            can be removed. */
        void emitDocIntoView(slice docID,
                             sequence docSequence,
                             unsigned viewNumber,
                             const std::vector<Collatable> &keys,
                             const std::vector<alloc_slice> &values);

        /** Removes the document from all views' indexes. Same as emitting an empty set of
            key/value pairs for each view. */
        void skipDoc(slice docID, sequence docSequence);

        /** Removes the document from the given view's index. Same as emitting an empty set of
            key/value pairs. */
        void skipDocInView(slice docID, sequence docSequence, unsigned viewNumber);

        /** Call when all documents have been indexed. Pass the last sequence that was enumerated
            (usually the database's lastSequence).*/
        void finished(sequence seq =1)       {_finishedSequence = seq;}

    private:
        std::vector<MapReduceIndexWriter*> _writers;
        MapReduceIndex* _triggerIndex {nullptr};
        sequence _latestDbSequence {0};
        sequence _finishedSequence {0};
        bool _allDocTypes {false};
        std::set<slice> _docTypes;

        const std::vector<Collatable> _noKeys;
        const std::vector<alloc_slice> _noValues;
};
}

#endif /* defined(__CBForest__MapReduceIndex__) */
