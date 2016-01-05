//
//  FullTextIndex.cc
//  CBForest
//
//  Created by Jens Alfke on 12/30/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

#include "FullTextIndex.hh"
#include "MapReduceIndex.hh"
#include "Tokenizer.hh"

namespace cbforest {

    static std::vector<KeyRange> keyRangesFor(slice queryString,
                                              std::string language,
                                              std::vector<std::string> &tokens)
    {
        if (language.size() == 0)
            language = Tokenizer::defaultStemmer;
        Tokenizer tokenizer(language);

        std::vector<KeyRange> collatableKeys;
        for (TokenIterator i(tokenizer, queryString, true); i; ++i) {
            tokens.push_back(i.token());
            collatableKeys.push_back(Collatable(CollatableBuilder(i.token())));
        }
        return collatableKeys;
    }


    FullTextIndexEnumerator::FullTextIndexEnumerator(Index *index,
                                                     slice queryString,
                                                     slice queryStringLanguage,
                                                     bool ranked,
                                                     const DocEnumerator::Options &options)
    :_tokens(),
     _e(index, keyRangesFor(queryString, std::string(queryStringLanguage), _tokens), options),
     _ranked(ranked),
     _curResultIndex(-1)
    {
        search();
    }


    // Runs the query, accumulating the results in _result.
    void FullTextIndexEnumerator::search() {
        std::vector<unsigned> termTotalCounts(_tokens.size());      // used for ranking
        typedef std::pair<cbforest::sequence, unsigned> RowID;
        std::map<RowID, FullTextMatch*> rows;
        
        while (_e.next()) {
            unsigned fullTextID;
            std::vector<size_t> matches = _e.getTextTokenInfo(fullTextID);

            auto termIndex = _e.currentKeyRangeIndex();
            RowID rowID(_e.sequence(), fullTextID);
            auto rowP = rows.find(rowID);
            FullTextMatch *row = NULL;
            if (rowP != rows.end()) {
                if (rowP->second->_lastTermIndex < termIndex-1) {
                    delete rowP->second;
                    rows.erase(rowP);
                } else {
                    row = rowP->second;
                }
            } else if (termIndex == 0) {
                // Only add new results during scan of first term, since results have to match all terms
                row = new FullTextMatch;
                row->_index = (const MapReduceIndex*)_e.index();
                row->docID = _e.docID();
                row->sequence = _e.sequence();
                rows[rowID] = row;
            }

            if (row) {
                auto matchCount = row->readTermMatches(_e.value(), termIndex);
                termTotalCounts[termIndex] += matchCount;
            }
        }

        // Now collect the rows that appeared for every query term:
        unsigned maxTermIndex = (unsigned)_tokens.size() - 1;
        for (auto i = rows.begin(); i != rows.end(); ++i) {
            if (i->second->_lastTermIndex == maxTermIndex)
                _results.push_back(i->second);
            else
                delete i->second;
        }

        if (_ranked) {
            for (auto i = _results.begin(); i != _results.end(); ++i) {
                double rank = 0.0;
                auto &matches = (*i)->textMatches;
                for (auto m = matches.begin(); m != matches.end(); ++m)
                    rank += 1.0 / termTotalCounts[m->termIndex];
                (*i)->_rank = (float)rank;
            }
            std::sort(_results.begin(), _results.end(), [](FullTextMatch *a, FullTextMatch *b) {
                return a->_rank > b->_rank;  // sort by descending rank
            });
        }
    }


    bool FullTextIndexEnumerator::next() {
        return ++_curResultIndex < _results.size();
    }

    const FullTextMatch* FullTextIndexEnumerator::match() {
        if (_curResultIndex < 0 || _curResultIndex >= _results.size())
            return NULL;
        return _results[_curResultIndex];
    }


#pragma mark - FULLTEXTMATCH:


    alloc_slice FullTextMatch::matchedText() const {
        return _index->readFullText(docID, sequence, _fullTextID);
    }

    alloc_slice FullTextMatch::value() const {
        return _index->readFullTextValue(docID, sequence, _fullTextID);
    }


    unsigned FullTextMatch::readTermMatches(cbforest::slice indexValue, unsigned termIndex) {
        _lastTermIndex = termIndex;
        CollatableReader reader(indexValue);
        reader.beginArray();
        _fullTextID = (uint32_t)reader.readInt();
        std::vector<size_t> tokens;
        unsigned matchCount = 0;
        do {
            TermMatch match;
            match.termIndex = termIndex;
            match.start = (uint32_t)reader.readInt();
            match.length = (uint32_t)reader.readInt();
            textMatches.push_back(match);
            ++matchCount;
        } while (reader.peekTag() != CollatableReader::kEndSequence);
        return matchCount;
    }

}