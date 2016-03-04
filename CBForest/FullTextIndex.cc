//
//  FullTextIndex.cc
//  CBForest
//
//  Created by Jens Alfke on 12/30/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "FullTextIndex.hh"
#include "MapReduceIndex.hh"
#include "Tokenizer.hh"
#include <algorithm>

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
                row = new FullTextMatch(_e);
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
            auto row = i->second;
            if (row->_lastTermIndex == maxTermIndex) {
                auto &matches = row->textMatches;
                std::sort(matches.begin(), matches.end());
                if (_ranked) {
                    double rank = 0.0;
                    for (auto m = matches.begin(); m != matches.end(); ++m)
                        rank += 1.0 / termTotalCounts[m->termIndex];
                    row->_rank = (float)rank;
                }
                _results.push_back(row);
            } else {
                delete row;   // skip it
            }
        }

        if (_ranked) {
            std::sort(_results.begin(), _results.end(), [](FullTextMatch *a, FullTextMatch *b) {
                return a->_rank > b->_rank;  // sort by _descending_ rank
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


    FullTextMatch::FullTextMatch(const IndexEnumerator &e)
    :docID {e.docID()},
     sequence {e.sequence()},
     _index {(const MapReduceIndex*)e.index()}
     // _lastTermIndex, _fullTextID will be initialized later in readTermMatches
    { }


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