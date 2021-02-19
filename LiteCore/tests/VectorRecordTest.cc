//
// VectorRecordTest.cc
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

#include "VectorRecord.hh"
#include "fleece/Mutable.hh"
#include <iostream>

namespace fleece {
    static inline std::ostream& operator<< (std::ostream &out, fleece::Array array) {
        return out << array.toJSONString();
    }
    static inline std::ostream& operator<< (std::ostream &out, fleece::Dict dict) {
        return out << dict.toJSONString();
    }
    static inline std::ostream& operator<< (std::ostream &out, const litecore::VectorRecord &doc) {
        doc.dump(out); return out;
    }
}


namespace litecore {
    static inline std::ostream& operator<< (std::ostream &out, const litecore::Revision &rev) {
        return out << "Revision{" << rev.revID.str() << ", " << int(rev.flags)
                   << ", " << rev.properties.toJSONString() << "}";
    }

    static inline bool operator== (const litecore::Revision &a, const litecore::Revision &b) {
        return a.revID == b.revID && a.flags == b.flags && a.properties.isEqual(b.properties);
    }
}

#include "LiteCoreTest.hh"

using namespace std;
using namespace litecore;
using namespace fleece;


static constexpr auto kRemote1 = RemoteID(1), kRemote2 = RemoteID(2);


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "Untitled VectorRecord", "[VectorRecord]") {
    VectorRecord doc(*store, Versioning::RevTrees, "Nuu");
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
    CHECK(doc.remoteRevision(RemoteID::Local)->properties == properties);
    CHECK(doc.remoteRevision(kRemote1) == nullopt);
    CHECK(doc.remoteRevision(kRemote2) == nullopt);

    MutableDict mutableProps = doc.mutableProperties();
    CHECK(mutableProps == properties);
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "Save VectorRecord", "[VectorRecord]") {
    {
        VectorRecord doc(*store, Versioning::RevTrees, "Nuu");

        doc.mutableProperties()["year"] = 2525;
        CHECK(doc.mutableProperties() == doc.properties());
        doc.setFlags(DocumentFlags::kHasAttachments);
        CHECK(doc.flags() == DocumentFlags::kHasAttachments);
        CHECK(doc.changed());

        {
            Transaction t(db);
            CHECK(doc.save(t) == VectorRecord::kNewSequence);
            CHECK(!doc.changed());
            t.commit();
        }

        cerr << "Doc is: " << doc << "\n";
        cerr << "Revisions: " << doc.revisionStorage() << "\n";
        CHECK(doc.sequence() == 1);
        CHECK(doc.revID().str() == "1-f2e52c9d6f0f40b6303eb0fb58d4ba6dd4521adc");
        CHECK(doc.flags() == DocumentFlags::kHasAttachments);
        CHECK(doc.properties().toJSON(true, true) == "{year:2525}");
        CHECK(!doc.changed());
        CHECK(doc.mutableProperties() == doc.properties());
        CHECK(doc.remoteRevision(RemoteID::Local)->properties == doc.properties());

        {
            Transaction t(db);
            CHECK(doc.save(t) == VectorRecord::kNoSave);

            doc.mutableProperties()["weekday"] = "Friday";
            doc.setFlags(DocumentFlags::kNone);
            CHECK(doc.save(t) == VectorRecord::kNewSequence);
            t.commit();
        }

        cerr << "Doc is: " << doc << "\n";
        cerr << "Revisions: " << doc.revisionStorage() << "\n";
        CHECK(doc.sequence() == 2);
        CHECK(doc.revID().str() == "2-c8eeae1245a44de160c2ca96e448f1650dd901da");
        CHECK(doc.flags() == DocumentFlags::kNone);
        CHECK(doc.properties().toJSON(true, true) == "{weekday:\"Friday\",year:2525}");
        CHECK(!doc.changed());
        CHECK(doc.mutableProperties() == doc.properties());
        CHECK(doc.remoteRevision(RemoteID::Local)->properties == doc.properties());

        cerr << "Storage:\n" << doc.dumpStorage();
    }
    {
        VectorRecord readDoc(*store, Versioning::RevTrees, store->get("Nuu"));
        CHECK(readDoc.docID() == "Nuu");
        CHECK(readDoc.sequence() == 2);
        CHECK(readDoc.revID().str() == "2-c8eeae1245a44de160c2ca96e448f1650dd901da");
        CHECK(readDoc.flags() == DocumentFlags::kNone);
        CHECK(readDoc.properties().toJSON(true, true) == "{weekday:\"Friday\",year:2525}");
        CHECK(!readDoc.changed());
        CHECK(readDoc.mutableProperties() == readDoc.properties());
        CHECK(readDoc.remoteRevision(RemoteID::Local)->properties == readDoc.properties());
    }
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "VectorRecord Empty Properties", "[VectorRecord]") {
    {
        VectorRecord doc(*store, Versioning::RevTrees, "Nuu");
        CHECK(!doc.exists());
        CHECK(doc.properties() != nullptr);
        CHECK(doc.properties().empty());

        Transaction t(db);
        CHECK(doc.save(t) == VectorRecord::kNewSequence);
        CHECK(!doc.changed());
        t.commit();

        CHECK(doc.properties() != nullptr);
        CHECK(doc.properties().empty());
    }
    {
        VectorRecord doc(*store, Versioning::RevTrees, "Nuu", kEntireBody);
        CHECK(doc.exists());
        CHECK(doc.properties() != nullptr);
        CHECK(doc.properties().empty());
    }
    {
        VectorRecord doc(*store, Versioning::RevTrees, "Nuu", kCurrentRevOnly);
        CHECK(doc.exists());
        CHECK(doc.properties() != nullptr);
        CHECK(doc.properties().empty());
    }
    {
        VectorRecord doc(*store, Versioning::RevTrees, "Nuu", kMetaOnly);
        CHECK(doc.exists());
        CHECK(doc.properties() == nullptr);
        doc.loadData(kCurrentRevOnly);
        CHECK(doc.properties() != nullptr);
        CHECK(doc.properties().empty());
    }
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "VectorRecord Remotes", "[VectorRecord]") {
    Transaction t(db);
    VectorRecord doc(*store, Versioning::RevTrees, "Nuu");

    doc.mutableProperties()["rodent"] = "mouse";
    doc.setRevID(revidBuffer("1-f000"));
    CHECK(doc.save(t) == VectorRecord::kNewSequence);

    // Add a remote revision:
    MutableDict remoteProps = MutableDict::newDict();
    remoteProps["rodent"] = "capybara";
    revidBuffer remoteRev("2-eeee");
    doc.setRemoteRevision(kRemote1, Revision{remoteProps, remoteRev, DocumentFlags::kHasAttachments});
    CHECK(doc.changed());
    CHECK(doc.remoteRevision(kRemote1)->properties == remoteProps);
    CHECK(doc.remoteRevision(kRemote1)->revID == remoteRev);
    CHECK(doc.remoteRevision(kRemote1)->flags == DocumentFlags::kHasAttachments);

    CHECK(doc.save(t) == VectorRecord::kNoNewSequence);
    cerr << "Doc is: " << doc << "\n";
    cerr << "Revisions: " << doc.revisionStorage() << "\n";

    CHECK(doc.sequence() == 1);
    CHECK(doc.revID().str() == "1-f000");
    CHECK(doc.flags() == DocumentFlags::kHasAttachments);
    CHECK(doc.properties().toJSON(true, true) == "{rodent:\"mouse\"}");
    CHECK(!doc.changed());

    auto remote1 = doc.remoteRevision(kRemote1);
    CHECK(remote1->revID.str() == "2-eeee");
    CHECK(remote1->flags == DocumentFlags::kHasAttachments);
    CHECK(remote1->properties.toJSON(true, true) == "{rodent:\"capybara\"}");

    cerr << "Storage:\n" << doc.dumpStorage();
}


N_WAY_TEST_CASE_METHOD (DataFileTestFixture, "VectorRecord Remote Update", "[VectorRecord]") {
    Transaction t(db);
    {
        VectorRecord doc(*store, Versioning::RevTrees, "Nuu");
        
        // Create doc, as if pulled from a remote:
        revidBuffer revid1("1-1111");
        doc.mutableProperties()["rodent"] = "mouse";
        doc.mutableProperties()["age"] = 1;
        MutableArray loc = MutableArray::newArray();
        loc.append(-108.3);
        loc.append(37.234);
        doc.mutableProperties()["loc"] = loc;
        doc.setRevID(revid1);
        
        // Make remote 1 the same as local:
        auto local = doc.currentRevision();
        CHECK(local == (Revision{doc.properties(), revid1}));
        doc.setRemoteRevision(kRemote1, local);
        CHECK(doc.save(t) == VectorRecord::kNewSequence);
    }
    {
        VectorRecord doc(*store, Versioning::RevTrees, "Nuu");
        cerr << "\nStorage after pull:\n" << doc.dumpStorage();
        
        CHECK(doc.currentRevision() == *doc.remoteRevision(kRemote1));
        CHECK(doc.properties() == doc.remoteRevision(kRemote1)->properties); // rev body only stored once
        
        // Update doc locally:
        doc.mutableProperties()["age"] = 2;
        revidBuffer revid2("2-2222");
        doc.setRevID(revid2);
        doc.setFlags(DocumentFlags::kNone);
        CHECK(doc.save(t));
    }
    {
        VectorRecord doc(*store, Versioning::RevTrees, "Nuu");
        cerr << "\nStorage after save:\n" << doc.dumpStorage();

        auto props1 = doc.properties(), props2 = doc.remoteRevision(kRemote1)->properties;
        CHECK(props1.toJSON(true, true) == "{age:2,loc:[-108.3,37.234],rodent:\"mouse\"}"_sl);
        CHECK(props2.toJSON(true, true) == "{age:1,loc:[-108.3,37.234],rodent:\"mouse\"}"_sl);
        CHECK(props1["rodent"] == props2["rodent"]);    // string should only be stored once
        CHECK(props1["loc"] == props2["loc"]);          // array should only be stored once
        CHECK(props1["age"] != props2["age"]);
    }
}
