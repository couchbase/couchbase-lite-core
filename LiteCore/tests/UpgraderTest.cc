//
// UpgraderTest.cc
//
// Copyright (c) 2017 Couchbase, Inc All rights reserved.
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

#include "Upgrader.hh"
#include "LiteCoreTest.hh"
#include "Database.hh"
#include "Document.hh"
#include "BlobStore.hh"
#include "Logging.hh"

using namespace std;
using namespace fleece;


class UpgradeTestFixture {
protected:

    Retained<Database> db;

    void upgrade(string oldPath) {
        FilePath newPath = FilePath::tempDirectory()["upgraded.cblite2/"];
        newPath.delRecursive();

        C4DatabaseConfig config { };
        config.flags = kC4DB_Create | kC4DB_SharedKeys;
        config.storageEngine = kC4SQLiteStorageEngine;
        config.versioning = kC4RevisionTrees;

        UpgradeDatabase(oldPath, newPath, config);

        db = new Database(newPath, config);
    }

    void upgradeInPlace(string fixturePath) {
        auto srcPath = FilePath(fixturePath);
        FilePath dbPath = FilePath::tempDirectory()[srcPath.fileOrDirName() + "/"];
        dbPath.delRecursive();
        srcPath.copyTo(dbPath);

        C4DatabaseConfig config { };
        config.flags = kC4DB_SharedKeys | kC4DB_NoUpgrade;
        config.storageEngine = kC4SQLiteStorageEngine;
        config.versioning = kC4RevisionTrees;

        // First check that NoUpgrade flag correctly triggers an exception:
        ExpectException(error::LiteCore, error::DatabaseTooOld, [&]{
            db = new Database(dbPath, config);
        });

        // Now allow the upgrade:
        ++gC4ExpectExceptions;
        config.flags &= ~kC4DB_NoUpgrade;
        db = new Database(dbPath, config);
        --gC4ExpectExceptions;
    }

    void verifyDoc(slice docID, slice bodyJSON, vector<slice> revIDs) {
        unique_ptr<Document> doc1( db->documentFactory().newDocumentInstance(toc4slice(docID)) );
        CHECK(doc1->exists());
        CHECK(doc1->bodyAsJSON() == bodyJSON);
        int i = 0;
        for (slice revID : revIDs) {
            if (i++ > 0)
                CHECK(doc1->selectNextRevision());
            CHECK(slice(doc1->selectedRev.revID) == revID);
        }
        CHECK(!doc1->selectNextRevision());
    }

    void verifyAttachment(string digest) {
        CHECK(db->blobStore()->get(blobKey(digest)).exists());
    }

};


TEST_CASE_METHOD(UpgradeTestFixture, "Upgrade from Android 1.2", "[Upgrade]") {
    upgrade(DataFileTestFixture::sFixturesDir + "replacedb/android120/androiddb.cblite2/");
    verifyDoc("doc1"_sl,
              "{\"key\":\"1\",\"_attachments\":{\"attach1\":{\"length\":7,\"digest\":\"sha1-P1i5kI/sosq745/9BDR7kEghKps=\",\"revpos\":2,\"content_type\":\"text/plain; charset=utf-8\",\"stub\":true}}}"_sl,
              {"2-db9941f74d7fd45d60c272b796ae50c7"_sl, "1-e2a2bdc0b00e32ecd0b6bc546024808b"_sl});
    verifyDoc("doc2"_sl,
              "{\"key\":\"2\",\"_attachments\":{\"attach2\":{\"length\":7,\"digest\":\"sha1-iTebnQazmdAhRBH64y9E6JqwSoc=\",\"revpos\":2,\"content_type\":\"text/plain; charset=utf-8\",\"stub\":true}}}"_sl,
              {"2-aaeb2815a598000a2f2afbbbf1ef4a89"_sl, "1-9eb68a4a7b2272dc7a972a3bc136c39d"_sl});
    verifyAttachment("sha1-P1i5kI/sosq745/9BDR7kEghKps=");
    verifyAttachment("sha1-iTebnQazmdAhRBH64y9E6JqwSoc=");
}


TEST_CASE_METHOD(UpgradeTestFixture, "Upgrade from Android 1.3", "[Upgrade]") {
    upgrade(DataFileTestFixture::sFixturesDir + "replacedb/android130/androiddb.cblite2/");
    verifyDoc("doc1"_sl,
              "{\"_attachments\":{\"attach1\":{\"length\":7,\"digest\":\"sha1-P1i5kI/sosq745/9BDR7kEghKps=\",\"revpos\":2,\"content_type\":\"plain/text\",\"stub\":true}},\"key\":\"1\"}"_sl,
              {"2-6422c597f66f74bf73014f78ac85724f"_sl, "1-e2a2bdc0b00e32ecd0b6bc546024808b"_sl});
    verifyDoc("doc2"_sl,
              "{\"_attachments\":{\"attach2\":{\"length\":7,\"digest\":\"sha1-iTebnQazmdAhRBH64y9E6JqwSoc=\",\"revpos\":2,\"content_type\":\"plain/text\",\"stub\":true}},\"key\":\"2\"}"_sl,
              {"2-904737015f5bb329b653aa4d15d2fcde"_sl, "1-9eb68a4a7b2272dc7a972a3bc136c39d"_sl});
    verifyAttachment("sha1-P1i5kI/sosq745/9BDR7kEghKps=");
    verifyAttachment("sha1-iTebnQazmdAhRBH64y9E6JqwSoc=");
}


TEST_CASE_METHOD(UpgradeTestFixture, "Upgrade from iOS 1.2", "[Upgrade]") {
    upgrade(DataFileTestFixture::sFixturesDir + "replacedb/ios120/iosdb.cblite2/");
    verifyDoc("doc1"_sl,
              "{\"_attachments\":{\"attach1\":{\"content_type\":\"text/plain; charset=utf-8\",\"digest\":\"sha1-P1i5kI/sosq745/9BDR7kEghKps=\",\"length\":7,\"revpos\":2,\"stub\":true}},\"boolean\":true,\"date\":\"2016-01-15T23:08:40.803Z\",\"foo\":\"bar\",\"number\":1,\"type\":\"doc\"}"_sl,
              {"2-f34206d6bd05b187b3f4fdd232174ac7"_sl, "1-d24e23f21c4f5b9ee83ce7e2493e0334"_sl});
    verifyDoc("doc2"_sl,
              "{\"_attachments\":{\"attach2\":{\"content_type\":\"text/plain; charset=utf-8\",\"digest\":\"sha1-iTebnQazmdAhRBH64y9E6JqwSoc=\",\"length\":7,\"revpos\":2,\"stub\":true}},\"boolean\":true,\"date\":\"2016-01-15T23:08:40.816Z\",\"foo\":\"bar\",\"number\":2,\"type\":\"doc\"}"_sl,
              {"2-47822c34de88456f589dd1e96cceaa58"_sl, "1-9e4e87929af78cceff5a802a13797fa1"_sl});
    verifyAttachment("sha1-P1i5kI/sosq745/9BDR7kEghKps=");
    verifyAttachment("sha1-iTebnQazmdAhRBH64y9E6JqwSoc=");
}


TEST_CASE_METHOD(UpgradeTestFixture, "Upgrade from iOS 1.3", "[Upgrade]") {
    upgrade(DataFileTestFixture::sFixturesDir + "replacedb/ios130/iosdb.cblite2/");
    verifyDoc("doc1"_sl,
              "{\"_attachments\":{\"attach1\":{\"content_type\":\"text/plain; charset=utf-8\",\"digest\":\"sha1-P1i5kI/sosq745/9BDR7kEghKps=\",\"length\":7,\"revpos\":2,\"stub\":true}},\"boolean\":true,\"date\":\"2016-07-07T03:12:13.471Z\",\"foo\":\"bar\",\"number\":1,\"type\":\"doc\"}"_sl,
              {"2-b9a637ed67d8bd3a34eb85d1ceb2a4b6"_sl, "1-8feb542236ef8bedaf555b57211c5c3e"_sl});
    verifyDoc("doc2"_sl,
              "{\"_attachments\":{\"attach2\":{\"content_type\":\"text/plain; charset=utf-8\",\"digest\":\"sha1-iTebnQazmdAhRBH64y9E6JqwSoc=\",\"length\":7,\"revpos\":2,\"stub\":true}},\"boolean\":true,\"date\":\"2016-07-07T03:12:13.508Z\",\"foo\":\"bar\",\"number\":2,\"type\":\"doc\"}"_sl,
              {"2-49da92f93593ef8a453966bcf6727f01"_sl, "1-418beaeadbceb80da969595cda4638d3"_sl});
    verifyAttachment("sha1-P1i5kI/sosq745/9BDR7kEghKps=");
    verifyAttachment("sha1-iTebnQazmdAhRBH64y9E6JqwSoc=");
}


TEST_CASE_METHOD(UpgradeTestFixture, "Upgrade from .NET 1.2", "[Upgrade]") {
    upgrade(DataFileTestFixture::sFixturesDir + "replacedb/net120/netdb.cblite2/");
    verifyDoc("doc1"_sl,
              "{\"_attachments\":{\"attach1\":{\"content_type\":\"image/png\",\"digest\":\"sha1-1uqCkSGvnQJexh2BV/z46ktEUSk=\",\"length\":38790,\"revpos\":2,\"stub\":true}},\"description\":\"Jim's avatar\"}"_sl,
              {"2-a85b8292de5f5490b3895d76d85f9432"_sl, "1-c84f0703d05821ba47412226ed0bfb20"_sl});
    verifyDoc("doc2"_sl,
              "{\"_attachments\":{\"attach2\":{\"content_type\":\"application/pgp-keys\",\"digest\":\"sha1-aTohES5UC/zBwIXuCNAhQ0BtajQ=\",\"length\":1706,\"revpos\":2,\"stub\":true}},\"description\":\"Jim's public key\"}"_sl,
              {"2-56cd1d1c6b694aabd9d6e341882ddc66"_sl, "1-79e3a86cc8205e91a6458f7f34b451dc"_sl});
    verifyAttachment("sha1-1uqCkSGvnQJexh2BV/z46ktEUSk=");
    verifyAttachment("sha1-aTohES5UC/zBwIXuCNAhQ0BtajQ=");
}


TEST_CASE_METHOD(UpgradeTestFixture, "Upgrade from .NET 1.3", "[Upgrade]") {
    upgrade(DataFileTestFixture::sFixturesDir + "replacedb/net130/netdb.cblite2/");
    verifyDoc("doc1"_sl,
              "{\"_attachments\":{\"attach1\":{\"content_type\":\"image/png\",\"digest\":\"sha1-v1M1+8aDtoX7zr6cJ2O7BlaaPAo=\",\"length\":10237,\"revpos\":2,\"stub\":true}},\"description\":\"Jim's avatar\"}"_sl,
              {"2-0648b6fe63bcc97db824a6d911b6aafc"_sl, "1-cd809becc169215072fd567eebd8b8de"_sl});
    verifyDoc("doc2"_sl,
              "{\"_attachments\":{\"attach1\":{\"content_type\":\"application/pgp-keys\",\"digest\":\"sha1-vX2fVqJf4pIbehLdk0L2cB4QXzI=\",\"length\":1736,\"revpos\":2,\"stub\":true}},\"description\":\"Jim's public key\"}"_sl,
              {"2-acae7bbf5269a5a9be40493e0601b28e"_sl, "1-cd809becc169215072fd567eebd8b8de"_sl});
    verifyAttachment("sha1-v1M1+8aDtoX7zr6cJ2O7BlaaPAo=");
    verifyAttachment("sha1-vX2fVqJf4pIbehLdk0L2cB4QXzI=");
}


#pragma mark - UPGRADING IN PLACE:

TEST_CASE_METHOD(UpgradeTestFixture, "Open and upgrade", "[Upgrade]") {
    upgradeInPlace(DataFileTestFixture::sFixturesDir + "replacedb/android120/androiddb.cblite2/");

    verifyDoc("doc1"_sl,
              "{\"key\":\"1\",\"_attachments\":{\"attach1\":{\"length\":7,\"digest\":\"sha1-P1i5kI/sosq745/9BDR7kEghKps=\",\"revpos\":2,\"content_type\":\"text/plain; charset=utf-8\",\"stub\":true}}}"_sl,
              {"2-db9941f74d7fd45d60c272b796ae50c7"_sl, "1-e2a2bdc0b00e32ecd0b6bc546024808b"_sl});
    verifyDoc("doc2"_sl,
              "{\"key\":\"2\",\"_attachments\":{\"attach2\":{\"length\":7,\"digest\":\"sha1-iTebnQazmdAhRBH64y9E6JqwSoc=\",\"revpos\":2,\"content_type\":\"text/plain; charset=utf-8\",\"stub\":true}}}"_sl,
              {"2-aaeb2815a598000a2f2afbbbf1ef4a89"_sl, "1-9eb68a4a7b2272dc7a972a3bc136c39d"_sl});
    verifyAttachment("sha1-P1i5kI/sosq745/9BDR7kEghKps=");
    verifyAttachment("sha1-iTebnQazmdAhRBH64y9E6JqwSoc=");
}
