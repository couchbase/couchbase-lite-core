//
//  RevisionTest.cc
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 7/11/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

#include "Revision.hh"
#include "LiteCoreTest.hh"
using namespace litecore;


inline const VersionVector kTestVers() {
    return VersionVector{"2@*,3@$,1@bob"_sl};
}
inline const VersionVector kTestCASVers() {
    return VersionVector{"3@$,2@*"_sl};
}

static void verifyRev(const Revision &rev) {
    REQUIRE(rev.docID() == "DOC"_sl);
    REQUIRE(rev.version() == kTestVers());
    REQUIRE(rev.flags() == kHasAttachments);
    REQUIRE(rev.hasAttachments());
    REQUIRE_FALSE(rev.isDeleted());
    REQUIRE_FALSE(rev.isConflicted());
    REQUIRE(rev.docType() == "O-"_sl);
    REQUIRE(rev.isCurrent());
}

TEST_CASE("CreateRev", "[Revision]") {
    Revision rev("DOC"_sl, kTestVers(),
                 Revision::BodyParams{"{\"foo\":true}"_sl, "O-"_sl, false, true},
                 true);
    verifyRev(rev);
    REQUIRE(rev.record().key() == "DOC"_sl);

    // Copy record:
    const Record &rec = rev.record();
    Record record2(rec.key());
    record2.setMeta(rec.meta());
    record2.setBody(rec.body());

    Revision rev2(std::move(record2));
    verifyRev(rev2);

    rev2.setCurrent(false);
    REQUIRE_FALSE(rev2.isCurrent());
    REQUIRE(rev2.record().key() == alloc_slice("DOC\t*,\02"));
}
