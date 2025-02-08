//
// DBAccessTestWrapper.cc
//
// Copyright (C) 2020 Jens Alfke. All Rights Reserved.
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

#include "DBAccessTestWrapper.hh"
#include "DBAccess.hh"
#include "c4DocEnumerator.hh"

using namespace std;
using namespace litecore::repl;

C4DocEnumerator* DBAccessTestWrapper::unresolvedDocsEnumerator(C4Database* db) {
    return unresolvedDocsEnumerator(db->getDefaultCollection());
}

C4DocEnumerator* DBAccessTestWrapper::unresolvedDocsEnumerator(C4Collection* coll) {
    return DBAccess::unresolvedDocsEnumerator(coll, true).release();
}

unsigned DBAccessTestWrapper::numDeltasApplied() { return DBAccess::gNumDeltasApplied; }
