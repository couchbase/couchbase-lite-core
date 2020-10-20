//
// NuDocumentTest.cc
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

#include "NuDocument.hh"
#include "fleece/Mutable.hh"
#include <iostream>

namespace fleece {
    static inline std::ostream& operator<< (std::ostream &out, fleece::Array array) {
        return out << array.toJSONString();
    }
    static inline std::ostream& operator<< (std::ostream &out, fleece::Dict dict) {
        return out << dict.toJSONString();
    }
    static inline std::ostream& operator<< (std::ostream &out, const litecore::NuDocument &doc) {
        doc.dump(out); return out;
    }
}


#include "LiteCoreTest.hh"

using namespace litecore;
using namespace fleece;


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "Untitled NuDocument", "[NuDocument]") {
    NuDocument doc(*store, "Nuu");
    cerr << "Doc is: " << doc << "\n";

    CHECK(!doc.exists());
    CHECK(doc.sequence() == 0);
    CHECK(doc.docID() == "Nuu");
    CHECK(doc.revID() == nullslice);
    CHECK(doc.flags() == DocumentFlags::kNone);

    Dict properties = doc.properties();
    CHECK(properties != nullptr);
    CHECK(properties.empty());
    CHECK(!doc.changed());

    CHECK(doc.currentRevision().properties == properties);
    CHECK(doc.currentRevision().revID == doc.revID());
    CHECK(doc.currentRevision().flags == doc.flags());
    CHECK(doc.remoteRevision(NuDocument::kLocal)->properties == properties);
    CHECK(doc.remoteRevision(1) == nullopt);
    CHECK(doc.remoteRevision(2) == nullopt);

    MutableDict mutableProps = doc.mutableProperties();
    CHECK(mutableProps == properties);
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "Save NuDocument", "[NuDocument]") {
    {
        NuDocument doc(*store, "Nuu");

        doc.mutableProperties()["year"] = 2525;
        CHECK(doc.mutableProperties() == doc.properties());
        CHECK(doc.changed());

        {
            Transaction t(db);
            CHECK(doc.save(t, revidBuffer("1-f000"), DocumentFlags::kHasAttachments) == NuDocument::kNewSequence);
            t.commit();
        }

        cerr << "Doc is: " << doc << "\n";
        cerr << "Revisions: " << doc.revisionStorage() << "\n";
        CHECK(doc.sequence() == 1);
        CHECK(doc.revID().str() == "1-f000");
        CHECK(doc.flags() == DocumentFlags::kHasAttachments);
        CHECK(doc.properties().toJSON(true, true) == "{year:2525}");
        CHECK(!doc.changed());
        CHECK(doc.mutableProperties() == doc.properties());
        CHECK(doc.remoteRevision(NuDocument::kLocal)->properties == doc.properties());

        {
            Transaction t(db);
            CHECK(doc.save(t, doc.revID(), doc.flags()) == NuDocument::kNoSave);

            doc.mutableProperties()["weekday"] = "Friday";
            CHECK(doc.save(t, revidBuffer("2-5252"), DocumentFlags::kNone) == NuDocument::kNewSequence);
            t.commit();
        }

        cerr << "Doc is: " << doc << "\n";
        cerr << "Revisions: " << doc.revisionStorage() << "\n";
        CHECK(doc.sequence() == 2);
        CHECK(doc.revID().str() == "2-5252");
        CHECK(doc.flags() == DocumentFlags::kNone);
        CHECK(doc.properties().toJSON(true, true) == "{weekday:\"Friday\",year:2525}");
        CHECK(!doc.changed());
        CHECK(doc.mutableProperties() == doc.properties());
        CHECK(doc.remoteRevision(NuDocument::kLocal)->properties == doc.properties());

        cerr << "Storage:\n" << doc.dumpStorage();
    }
    {
        NuDocument readDoc(*store, store->get("Nuu"));
        CHECK(readDoc.docID() == "Nuu");
        CHECK(readDoc.sequence() == 2);
        CHECK(readDoc.revID().str() == "2-5252");
        CHECK(readDoc.flags() == DocumentFlags::kNone);
        CHECK(readDoc.properties().toJSON(true, true) == "{weekday:\"Friday\",year:2525}");
        CHECK(!readDoc.changed());
        CHECK(readDoc.mutableProperties() == readDoc.properties());
        CHECK(readDoc.remoteRevision(NuDocument::kLocal)->properties == readDoc.properties());
    }
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "NuDocument Remotes", "[NuDocument]") {
    Transaction t(db);
    NuDocument doc(*store, "Nuu");

    doc.mutableProperties()["rodent"] = "mouse";
    CHECK(doc.save(t, revidBuffer("1-f000"), DocumentFlags::kNone) == NuDocument::kNewSequence);

    // Add a remote revision:
    MutableDict remoteProps = MutableDict::newDict();
    remoteProps["rodent"] = "capybara";
    revidBuffer remoteRev("2-eeee");
    doc.setRemoteRevision(1, Revision{remoteProps, remoteRev, DocumentFlags::kHasAttachments});
    CHECK(doc.changed());
    CHECK(doc.remoteRevision(1)->properties == remoteProps);
    CHECK(doc.remoteRevision(1)->revID == remoteRev);
    CHECK(doc.remoteRevision(1)->flags == DocumentFlags::kHasAttachments);

    CHECK(doc.save(t) == NuDocument::kNoNewSequence);
    cerr << "Doc is: " << doc << "\n";
    cerr << "Revisions: " << doc.revisionStorage() << "\n";

    CHECK(doc.sequence() == 1);
    CHECK(doc.revID().str() == "1-f000");
    CHECK(doc.flags() == DocumentFlags::kNone);
    CHECK(doc.properties().toJSON(true, true) == "{rodent:\"mouse\"}");
    CHECK(!doc.changed());

    auto remote1 = doc.remoteRevision(1);
    CHECK(remote1->revID.str() == "2-eeee");
    CHECK(remote1->flags == DocumentFlags::kHasAttachments);
    CHECK(remote1->properties.toJSON(true, true) == "{rodent:\"capybara\"}");

    cerr << "Storage:\n" << doc.dumpStorage();
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "NuDocument Remote Update", "[NuDocument]") {
    Transaction t(db);
    NuDocument doc(*store, "Nuu");

    // Create doc, as if pulled from a remote:
    revidBuffer rev1("1-1111");
    doc.mutableProperties()["rodent"] = "mouse";
    doc.mutableProperties()["age"] = 1;
    doc.setRemoteRevision(1, Revision{doc.properties(), rev1, DocumentFlags::kNone});
    CHECK(doc.save(t, rev1, DocumentFlags::kNone) == NuDocument::kNewSequence);
    cerr << "Storage after pull:\n" << doc.dumpStorage();
    CHECK(doc.properties() == doc.remoteRevision(1)->properties);

    // Update doc locally:
    doc.mutableProperties()["age"] = 2;
    revidBuffer rev2("2-2222");
    CHECK(doc.save(t, rev2, DocumentFlags::kNone) == NuDocument::kNewSequence);
    CHECK(doc.properties().toJSON(true, true) == "{age:2,rodent:\"mouse\"}"_sl);
    CHECK(doc.remoteRevision(1)->properties.toJSON(true, true) == "{age:1,rodent:\"mouse\"}"_sl);

    cerr << "Storage after save:\n" << doc.dumpStorage();
}
