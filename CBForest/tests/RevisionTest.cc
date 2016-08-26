//
//  RevisionTest.cc
//  CBForest
//
//  Created by Jens Alfke on 7/11/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

#include "Revision.hh"
#include "CBForestTest.hh"
using namespace cbforest;


const VersionVector kTestVers {slice("2@*,3@$,1@bob")};
const VersionVector kTestCASVers {slice("3@$,2@*")};

static void verifyRev(const Revision &rev) {
    REQUIRE(rev.docID() == slice("DOC"));
    REQUIRE(rev.version() == kTestVers);
    REQUIRE(rev.flags() == Revision::kHasAttachments);
    REQUIRE(rev.hasAttachments());
    REQUIRE_FALSE(rev.isDeleted());
    REQUIRE_FALSE(rev.isConflicted());
    REQUIRE(rev.docType() == slice("O-"));
    REQUIRE(rev.isCurrent());
}

TEST_CASE("CreateRev", "[Revision]") {
    Revision rev(slice("DOC"), kTestVers,
                 Revision::BodyParams{slice("{\"foo\":true}"), slice("O-"), false, true},
                 true);
    verifyRev(rev);
    REQUIRE(rev.document().key() == slice("DOC"));

    // Copy document:
    const Document &doc = rev.document();
    Document doc2(doc.key());
    doc2.setMeta(doc.meta());
    doc2.setBody(doc.body());

    Revision rev2(std::move(doc2));
    verifyRev(rev2);

    rev2.setCurrent(false);
    REQUIRE_FALSE(rev2.isCurrent());
    REQUIRE(rev2.document().key() == alloc_slice("DOC\t*,\02"));
}
