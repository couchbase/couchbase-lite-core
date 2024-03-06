//
// SQLiteFTSRankFunction.cpp
//
// Copyright 2016-Present Couchbase, Inc.
//
// Use of this software is governed by the Business Source License included
// in the file licenses/BSL-Couchbase.txt.  As of the Change Date specified
// in that file, in accordance with the Business Source License, use of this
// software will be governed by the Apache License, Version 2.0, included in
// the file licenses/APL2.txt.
//
//  Adapted from public domain source code at https://www.sqlite.org/fts3.html#appendix_a

#include "SQLiteFleeceUtil.hh"
#include <sqlite3.h>
#include <cstdint>

namespace litecore {

    /*
     ** SQLite user defined function to use with matchinfo() to calculate the
     ** relevancy of an FTS match. The value returned is the relevancy score
     ** (a real value greater than or equal to zero). A larger value indicates
     ** a more relevant document.
     **
     ** The overall relevancy returned is the sum of the relevancies of each
     ** column value in the FTS table. The relevancy of a column value is the
     ** sum of the following for each reportable phrase in the FTS query:
     **
     **   (<hit count> / <global hit count>) * <column weight>
     **
     ** where <hit count> is the number of instances of the phrase in the
     ** column value of the current row and <global hit count> is the number
     ** of instances of the phrase in the same column of all rows in the FTS
     ** table. The <column weight> is a weighting factor assigned to each
     ** column by the caller (see below).
     **
     ** The first argument to this function must be the return value of the FTS
     ** matchinfo() function. Following this must be one argument for each column
     ** of the FTS table containing a numeric weight factor for the corresponding
     ** column. Example:
     **
     **     CREATE VIRTUAL TABLE documents USING fts3(title, content)
     **
     ** The following query returns the docids of documents that match the full-text
     ** query <query> sorted from most to least relevant. When calculating
     ** relevance, query term instances in the 'title' column are given twice the
     ** weighting of those in the 'content' column.
     **
     **     SELECT docid FROM documents
     **     WHERE documents MATCH <query>
     **     ORDER BY rank(matchinfo(documents), 1.0, 0.5) DESC
     */
    static void rankfunc(sqlite3_context* pCtx, int nVal, sqlite3_value** apVal) {
        int32_t* aMatchinfo;  /* Return value of matchinfo() */
        int32_t  nCol;        /* Number of columns in the table */
        int32_t  nPhrase;     /* Number of phrases in the query */
        int32_t  iPhrase;     /* Current phrase */
        double   score = 0.0; /* Value to return */

        /* Check that the number of arguments passed to this function is correct.
         ** If not, jump to wrong_number_args. Set aMatchinfo to point to the array
         ** of unsigned integer values returned by FTS function matchinfo. Set
         ** nPhrase to contain the number of reportable phrases in the users full-text
         ** query, and nCol to the number of columns in the table.
         */
        if ( nVal != 1 ) goto wrong_number_args;
        aMatchinfo = (int32_t*)sqlite3_value_blob(apVal[0]);
        if ( !aMatchinfo ) {
            sqlite3_result_error(pCtx, "nothing for rank() to match", -1);
            return;
        }
        nPhrase = aMatchinfo[0];
        nCol    = aMatchinfo[1];
        //        if( nVal!=(1+nCol) ) goto wrong_number_args;

        /* Iterate through each phrase in the users query. */
        for ( iPhrase = 0; iPhrase < nPhrase; iPhrase++ ) {
            int iCol; /* Current column */

            /* Now iterate through each column in the users query. For each column,
             ** increment the relevancy score by:
             **
             **   (<hit count> / <global hit count>) * <column weight>
             **
             ** aPhraseinfo[] points to the start of the data for phrase iPhrase. So
             ** the hit count and global hit counts for each column are found in
             ** aPhraseinfo[iCol*3] and aPhraseinfo[iCol*3+1], respectively.
             */
            int* aPhraseinfo = &aMatchinfo[2 + iPhrase * nCol * 3];
            for ( iCol = 0; iCol < nCol; iCol++ ) {
                int    nHitCount       = aPhraseinfo[3 * iCol];
                int    nGlobalHitCount = aPhraseinfo[3 * iCol + 1];
                double weight          = 1.0;  // sqlite3_value_double(apVal[iCol+1]);
                if ( nHitCount > 0 ) { score += ((double)nHitCount / (double)nGlobalHitCount) * weight; }
            }
        }

        sqlite3_result_double(pCtx, score);
        return;

        /* Jump here if the wrong number of arguments are passed to this function */
    wrong_number_args:
        sqlite3_result_error(pCtx, "wrong number of arguments to function rank()", -1);
    }

    const SQLiteFunctionSpec kRankFunctionsSpec[] = {{"rank", 1, rankfunc}, {}};

}  // namespace litecore
