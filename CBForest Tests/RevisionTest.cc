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


class RevisionTest : public CppUnit::TestFixture {

    const VersionVector kTestVers {slice("2@*,3@$,1@bob")};
    const VersionVector kTestCASVers {slice("3@$,2@*")};

    void testCreateRev() {
        Revision rev(slice("DOC"), kTestVers,
                     Revision::BodyParams{slice("{\"foo\":true}"), slice("O-"), false, true},
                     true);
        verifyRev(rev);
        AssertEqual(rev.document().key(), slice("DOC"));

        // Copy document:
        const Document &doc = rev.document();
        Document doc2(doc.key());
        doc2.setMeta(doc.meta());
        doc2.setBody(doc.body());

        Revision rev2(std::move(doc2));
        verifyRev(rev2);

        rev2.setCurrent(false);
        Assert(!rev2.isCurrent());
        AssertEqual(rev2.document().key(), slice("DOC\t*,\02"));
    }

    void testCASRev() {
        Revision rev(slice("DOC"), kTestCASVers,
                     Revision::BodyParams{slice("{\"foo\":true}"), slice("O-"), false, true},
                     true);
        Assert(rev.isFromCASServer());
        AssertEqual(rev.CAS(), 3ull);
    }

    void verifyRev(const Revision &rev) {
        AssertEqual(rev.docID(), slice("DOC"));
        AssertEqual(rev.version(), kTestVers);
        Assert(!rev.isFromCASServer());
        AssertEqual(rev.CAS(), 0ull);
        AssertEqual(rev.version().CASBase(), 3ull);
        AssertEqual(rev.flags(), Revision::kHasAttachments);
        Assert(rev.hasAttachments());
        Assert(!rev.isDeleted());
        Assert(!rev.isConflicted());
        AssertEqual(rev.docType(), slice("O-"));
        Assert(rev.isCurrent());
    }

    CPPUNIT_TEST_SUITE( RevisionTest );
    CPPUNIT_TEST( testCreateRev );
    CPPUNIT_TEST( testCASRev );
    CPPUNIT_TEST_SUITE_END();
};

CPPUNIT_TEST_SUITE_REGISTRATION(RevisionTest);
