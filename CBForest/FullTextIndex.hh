//
//  FullTextIndex.hh
//  CBForest
//
//  Created by Jens Alfke on 12/30/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#ifndef FullTextIndex_hh
#define FullTextIndex_hh

#include "MapReduceIndex.hh"
#include <map>


namespace cbforest {

    struct TermMatch {
        uint32_t termIndex;                 ///< Index of the search term in the tokenized query
        uint32_t start, length;             ///< *Byte* range of word in query string

        bool operator < (const TermMatch &other) const  {return start < other.start;}
    };


    /** Represents a match of a full-text query. */
    class FullTextMatch {
    public:
        alloc_slice docID;                  ///< The document ID that produced the text
        cbforest::sequence sequence;        ///< The sequence number of the document revision
        std::vector<TermMatch> textMatches; ///< The positions in the query string of the matches

        alloc_slice value() const;          ///< The emitted value

        unsigned fullTextID() const         {return _fullTextID;}
        alloc_slice matchedText() const;    ///< The emitted string that was matched

        static alloc_slice matchedText(MapReduceIndex *index,
                                       slice docID,
                                       cbforest::sequence seq,
                                       unsigned fullTextID) {
            return index->readFullText(docID, seq, fullTextID);
        }
        

    private:
        FullTextMatch(const IndexEnumerator&);
        unsigned readTermMatches(slice indexValue, unsigned termIndex);

        const MapReduceIndex *_index;
        unsigned _fullTextID;
        int _lastTermIndex;
        float _rank {0.0};

        friend class FullTextIndexEnumerator;
    };


    /** Enumerator for full-text queries. */
    class FullTextIndexEnumerator {
    public:
        FullTextIndexEnumerator(Index*,
                                slice queryString,
                                slice queryStringLanguage,
                                bool ranked,
                                const DocEnumerator::Options&);

        bool next();
        void close()                                        {_e.close();}
        const FullTextMatch *match();

        const std::vector<FullTextMatch*>& allMatches()     {return _results;}

    private:
        void search();

        std::vector<std::string> _tokens;
        IndexEnumerator _e;
        bool _ranked;
        std::vector<FullTextMatch*> _results;
        int _curResultIndex;
};

}

#endif /* FullTextIndex_hh */
