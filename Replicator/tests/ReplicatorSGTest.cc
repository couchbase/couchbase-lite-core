//
// Created by Callum Birks on 24/01/2023.
//

#include "ReplicatorSGTest.hh"
#include "ReplicatorLoopbackTest.hh"

TEST_CASE_METHOD(ReplicatorSGTest, "Simple Push and Verify SG", "[.SyncServerSG]") {
    const string idPrefix = timePrefix();
    constexpr size_t docCount = 20;

    initTest();

    // Import `docCount` docs
    importJSONLines(sFixturesDir + "names_100.json", 0, false, db, docCount, idPrefix);

    // Push & Pull replication
    ReplParams replParams { kC4OneShot, kC4OneShot };
    updateDocIDs();
    replParams.setDocIDs(_docIDs);
    replicate(replParams);

    // Verify
    verifyDocs(_docIDs);
}

TEST_CASE_METHOD(ReplicatorSGTest, "API Push 5000 Changes Collections SG", "[.SyncServerSG]") {
    string idPrefix = timePrefix();
    const string docID = idPrefix + "apipfcc-doc1";
    constexpr unsigned revisionCount = 2000;

    initTest();

    std::string revID;

    ReplParams replParams { kC4OneShot, kC4Disabled };

    {
        TransactionHelper t(db);
        revID = createNewRev(db, slice(docID), nullslice, kFleeceBody);
        REQUIRE(!revID.empty());
    }

    replicate(replParams);
    updateDocIDs();
    verifyDocs(_docIDs);

    C4Log("-------- Mutations --------");
    {
        TransactionHelper t(db);
        for (int i = 2; i <= revisionCount; ++i) {
            revID = createNewRev(db, slice(docID), slice(revID), kFleeceBody);
            REQUIRE(!revID.empty());
        }
    }

    C4Log("-------- Second Replication --------");
    replicate(replParams);
    updateDocIDs();
    verifyDocs(_docIDs, true);
}

TEST_CASE_METHOD(ReplicatorSGTest, "Default Collection Incremental Push SG", "[.SyncServerSG]") {
    string idPrefix = timePrefix();

    initTest();

    addDocs(db, 10, idPrefix);

    updateDocIDs();

    ReplParams replParams { kC4OneShot, kC4Disabled };
    replParams.setDocIDs(_docIDs);
    replicate(replParams);
    verifyDocs(_docIDs);

    // Add docs to local database
    idPrefix = timePrefix();
    addDocs(db, 5, idPrefix);

    updateDocIDs();

    replParams.setDocIDs(_docIDs);
    replicate(replParams);
    verifyDocs(_docIDs);
}

TEST_CASE_METHOD(ReplicatorSGTest, "Default Collection Incremental Revisions SG", "[.SyncServerSG]") {
    const string idPrefix = timePrefix();

    initTest();

    addDocs(db, 2, idPrefix);

    Jthread jthread;
    _callbackWhenIdle = [=, &jthread]() {
        jthread.thread = std::thread(std::thread{[=]() mutable {
            const string docID = idPrefix + "docko";
            ReplicatorLoopbackTest::addRevs(db, 500ms, alloc_slice(docID), 1, 10, true, "db");
            _stopWhenIdle.store(true);
        }});
        _callbackWhenIdle = nullptr;
    };

    ReplParams replParams { kC4Continuous, kC4Disabled };
    replicate(replParams);
    // total 3 docs, 12 revs, for each collections.
    CHECK(_callbackStatus.progress.documentCount == 12);
    updateDocIDs();
    verifyDocs(_docIDs, true);
}

TEST_CASE_METHOD(ReplicatorSGTest, "Pull deltas from Collection SG", "[.SyncCollSlow]") {
    constexpr size_t kDocBufSize = 60;

    constexpr int kNumDocs = 799, kNumProps = 799;
    const string idPrefix = timePrefix();
    const string docIDPref = idPrefix + "doc";
    const string channelID = idPrefix + "a";

    initTest( { channelID }, "pdfcsg");

    C4Log("-------- Populating local db --------");
    auto populateDB = [&]() {
        TransactionHelper t(db);
        std::srand(123456); // start random() sequence at a known place NOLINT(cert-msc51-cpp)
        for (int docNo = 0; docNo < kNumDocs; ++docNo) {
            constexpr size_t kDocBufSize = 60;
            char docID[kDocBufSize];
            snprintf(docID, kDocBufSize, "%s-%03d", docIDPref.c_str(), docNo);
            Encoder encPopulate(c4db_createFleeceEncoder(db));
            encPopulate.beginDict();

            encPopulate.writeKey(kC4ReplicatorOptionChannels);
            encPopulate.writeString(channelID);

            for (int p = 0; p < kNumProps; ++p) {
                encPopulate.writeKey(format("field%03d", p));
                encPopulate.writeInt(std::rand());
            }
            encPopulate.endDict();
            alloc_slice body = encPopulate.finish();
            string revID = createNewRev(db, slice(docID), body);
        }
    };

    populateDB();

    C4Log("-------- Pushing to SG --------");
    ReplParams replParams { kC4OneShot, kC4Disabled };
    replicate(replParams);

    C4Log("-------- Updating docs on SG --------");
    {
        JSONEncoder encUpdate;
        encUpdate.beginDict();
        encUpdate.writeKey("docs"_sl);
        encUpdate.beginArray();
        for (int docNo = 0; docNo < kNumDocs; ++docNo) {
            char docID[kDocBufSize];
            snprintf(docID, kDocBufSize, "%s-%03d", docIDPref.c_str(), docNo);
            C4Error error;
            c4::ref<C4Document> doc = c4db_getDoc(db, slice(docID), false, kDocGetAll, ERROR_INFO(error));
            REQUIRE(doc);
            Dict props = c4doc_getProperties(doc);

            encUpdate.beginDict();
            encUpdate.writeKey("_id"_sl);
            encUpdate.writeString(docID);
            encUpdate.writeKey("_rev"_sl);
            encUpdate.writeString(doc->revID);
            for (Dict::iterator j(props); j; ++j) {
                encUpdate.writeKey(j.keyString());
                if(j.keyString() == kC4ReplicatorOptionChannels){
                    encUpdate.writeString(j.value().asString());
                    continue;
                }
                auto value = j.value().asInt();
                if (RandomNumber() % 8 == 0)
                    value = RandomNumber();
                encUpdate.writeInt(value);
            }
            encUpdate.endDict();
        }
        encUpdate.endArray();
        encUpdate.endDict();

        REQUIRE(_sg.insertBulkDocs(encUpdate.finish(), 30.0));
    }

    double timeWithDelta = 0, timeWithoutDelta = 0;
    for (int pass = 1; pass <= 3; ++pass) {
        if (pass == 3) {
            C4Log("-------- DISABLING DELTA SYNC --------");
            replParams.setOption(C4STR(kC4ReplicatorOptionDisableDeltas), true);
        }

        C4Log("-------- PASS #%d: Repopulating local db --------", pass);

        deleteAndRecreateDB();

        populateDB();

        C4Log("-------- PASS #%d: Pulling changes from SG --------", pass);
        Stopwatch st;

        replParams.setPushPull(kC4Disabled, kC4OneShot);
        replicate(replParams);

        double time = st.elapsed();

        C4Log("-------- PASS #%d: Pull took %.3f sec (%.0f docs/sec) --------", pass, time, kNumDocs/time);
        if (pass == 2)
            timeWithDelta = time;
        else if (pass == 3)
            timeWithoutDelta = time;

        int n = 0;
        C4Error error;
        c4::ref<C4DocEnumerator> e = c4db_enumerateAllDocs(db, nullptr, ERROR_INFO(error));
        REQUIRE(e);
        while (c4enum_next(e, ERROR_INFO(error))) {
            C4DocumentInfo info;
            c4enum_getDocumentInfo(e, &info);
            CHECK(slice(info.docID).hasPrefix(slice(docIDPref)));
            CHECK(slice(info.revID).hasPrefix("2-"_sl));
            ++n;
        }
        CHECK(error.code == 0);
        CHECK(n == kNumDocs);
    }

    C4Log("-------- %.3f sec with deltas, %.3f sec without; %.2fx speed",
          timeWithDelta, timeWithoutDelta, timeWithoutDelta/timeWithDelta);
}

TEST_CASE_METHOD(ReplicatorSGTest, "Push and Pull Attachments SG", "[.SyncServerSG]") {
    const string idPrefix = timePrefix();

    initTest();

    std::vector<C4BlobKey> blobKeys { 2 }; // blobKeys1a, blobKeys1b;

    vector<string> attachments1 = {
            idPrefix + "Attachment A",
            idPrefix + "Attachment B",
            idPrefix + "Attachment Z"
    };
    {
        const string doc1 = idPrefix + "doc1";
        TransactionHelper t(db);
        blobKeys = addDocWithAttachments(slice(doc1), attachments1, "text/plain");
    }

    C4Log("-------- Pushing to SG --------");
    updateDocIDs();
    ReplParams replParams { kC4OneShot, kC4Disabled };
    replParams.setDocIDs(_docIDs);
    replicate(replParams);

    C4Log("-------- Checking docs and attachments --------");
    verifyDocs(_docIDs, true);
    checkAttachments(verifyDb, blobKeys, attachments1);
}

TEST_CASE_METHOD(ReplicatorSGTest, "Push & Pull Deletion SG", "[.SyncServerSG]") {
    const string idPrefix = timePrefix();
    const string docID = idPrefix + "ppd-doc1";

    initTest();

    createRev(db, slice(docID), kRevID, kFleeceBody);
    createRev(db, slice(docID), kRev2ID, kEmptyFleeceBody, kRevDeleted);

    std::unordered_map<alloc_slice, unsigned> docIDs {{ alloc_slice(docID), 0 }};
    
    ReplParams replParams { kC4OneShot, kC4Disabled };
    replParams.setDocIDs(docIDs);
    replicate(replParams);

    C4Log("-------- Deleting and re-creating database --------");
    deleteAndRecreateDB();

    createRev(db, slice(docID), kRevID, kFleeceBody);

    replParams.setPushPull(kC4Disabled, kC4OneShot);
    replicate(replParams);

    c4::ref<C4Document> remoteDoc = c4db_getDoc(db, slice(docID), true, kDocGetAll, nullptr);
    REQUIRE(remoteDoc);
    CHECK(remoteDoc->revID == kRev2ID);
    CHECK((remoteDoc->flags & kDocDeleted) != 0);
    CHECK((remoteDoc->selectedRev.flags & kRevDeleted) != 0);
    REQUIRE(c4doc_selectParentRevision(remoteDoc));
    CHECK(remoteDoc->selectedRev.revID == kRevID);
}

TEST_CASE_METHOD(ReplicatorSGTest, "Resolve Conflict SG", "[.SyncServerSG]") {
    const string idPrefix = timePrefix();
    const string docID = idPrefix + "rcsg";
    initTest();

    createFleeceRev(db, slice(docID), kRev1ID, "{}"_sl);
    createFleeceRev(db, slice(docID), revOrVersID("2-12121212", "1@cafe"),
                    "{\"db\":\"remote\"}"_sl);

    updateDocIDs();

    // Send the docs to remote
    ReplParams replParams { kC4OneShot, kC4Disabled };
    replParams.setDocIDs(_docIDs);
    replicate(replParams);

    verifyDocs(_docIDs, true);

    deleteAndRecreateDB();

    createFleeceRev(db, slice(docID), kRev1ID, "{}"_sl);
    createFleeceRev(db, slice(docID), revOrVersID("2-13131313", "1@babe"),
                    "{\"db\":\"local\"}"_sl);

    updateDocIDs();
    replParams.setDocIDs(_docIDs);

    _conflictHandler = [&](const C4DocumentEnded* docEndedWithConflict) {
        C4Error error;
        TransactionHelper t(db);

        slice docID = docEndedWithConflict->docID;
        // Get the local doc. It is the current revision
        c4::ref<C4Document> localDoc = c4db_getDoc(db, docID, true, kDocGetAll, WITH_ERROR(error));
        CHECK(error.code == 0);

        // Get the remote doc. It is the next leaf revision of the current revision.
        c4::ref<C4Document> remoteDoc = c4db_getDoc(db, docID, true, kDocGetAll, &error);
        bool succ = c4doc_selectNextLeafRevision(remoteDoc, true, false, &error);
        Assert(remoteDoc->selectedRev.revID == docEndedWithConflict->revID);
        CHECK(error.code == 0);
        CHECK(succ);

        C4Document* resolvedDoc = remoteDoc;

        FLDict mergedBody = c4doc_getProperties(resolvedDoc);
        C4RevisionFlags mergedFlags = resolvedDoc->selectedRev.flags;
        alloc_slice winRevID = resolvedDoc->selectedRev.revID;
        alloc_slice lostRevID = (resolvedDoc == remoteDoc) ? localDoc->selectedRev.revID
                                                           : remoteDoc->selectedRev.revID;
        bool result = c4doc_resolveConflict2(localDoc, winRevID, lostRevID,
                                             mergedBody, mergedFlags, &error);
        Assert(result, "conflictHandler: c4doc_resolveConflict2 failed for '%.*s'",
               SPLAT(docID));
        Assert((localDoc->flags & kDocConflicted) == 0);

        if (!c4doc_save(localDoc, 0, &error)) {
            Assert(false, "conflictHandler: c4doc_save failed for '%.*s'",
                   SPLAT(docID));
        }
    };

    replParams.setPushPull(kC4Disabled, kC4OneShot);
    replicate(replParams);

    c4::ref<C4Document> doc = c4db_getDoc(db, slice(docID),
                                            true, kDocGetAll, nullptr);
    REQUIRE(doc);
    CHECK(fleece2json(c4doc_getRevisionBody(doc)) == "{db:\"remote\"}"); // Remote Wins
    REQUIRE(!c4doc_selectNextLeafRevision(doc, true, false, nullptr));
}

TEST_CASE_METHOD(ReplicatorSGTest, "Update Once-Conflicted Doc - SG", "[.SyncServerSG]") {
    const string idPrefix = timePrefix();
    const string docID = idPrefix + "uocd-doc";
//    const string channelID = idPrefix + "uocd";

    initTest();

    std::array<std::string, 4> bodies {
            R"({"_rev":"1-aaaa","foo":1})",
            R"({"_revisions":{"start":2,"ids":["bbbb","aaaa"]},"foo":2.1})",
            R"({"_revisions":{"start":2,"ids":["cccc","aaaa"]},"foo":2.2})",
            R"({"_revisions":{"start":3,"ids":["dddd","cccc"]},"_deleted":true})"
    };

    // Create a conflicted doc on SG, and resolve the conflict
    for(const auto& body : bodies) {
        _sg.upsertDoc(docID + "?new_edits=false", body);
    }

    std::unordered_map<alloc_slice, unsigned> docIDs {{ alloc_slice(docID), 0 }};

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    ReplParams replParams { kC4Disabled, kC4OneShot };
    replParams.setDocIDs(docIDs);
    replicate(replParams);

    // Verify doc:
    c4::ref<C4Document> doc = c4db_getDoc(db, slice(docID), true, kDocGetAll, nullptr);
    REQUIRE(doc);
    C4Slice revID = C4STR("2-bbbb");
    CHECK(doc->revID == revID);
    CHECK((doc->flags & kDocDeleted) == 0);
    REQUIRE(c4doc_selectParentRevision(doc));
    CHECK(doc->selectedRev.revID == "1-aaaa"_sl);

    // Update doc:
    createRev(db, slice(docID), "3-ffff"_sl, kFleeceBody);

    // Push change back to SG:
    C4Log("-------- Pushing");
    replParams.setPushPull(kC4OneShot, kC4Disabled);
    replicate(replParams);

    updateDocIDs();

    verifyDocs(_docIDs, true);
}

#ifdef COUCHBASE_ENTERPRISE

TEST_CASE_METHOD(ReplicatorSGTest, "Pinned Certificate Success - SG", "[.SyncServerSG]") {
    // Leaf cert (Replicator/tests/data/cert/sg_cert.pem (1st cert))
    _sg.pinnedCert = slice(R"(-----BEGIN CERTIFICATE-----
MIICqzCCAZMCFCbvSAAFwn8RVp3Rn26N2VKOc1oGMA0GCSqGSIb3DQEBCwUAMBAx
DjAMBgNVBAMMBUludGVyMB4XDTIzMDEyNTE3MjUzNVoXDTMzMDEyMjE3MjUzNVow
FDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIB
CgKCAQEAt8zuD5uA4gIGVronjX3krmyH34KqD+Gsj6vu5KvFS5+/yJ5DdLZGS7BX
MsGUCfHa6WFalLEfH7BTdaualJyQxGM1qYFOtW5L/5H7x/uJcAtVnrujc/kUAUKW
eI037q+WQmBPvnUxYix5o1qOxjs2F92Loq6UrWZxub/rxkPkLZOAkSfCos00eodO
+Hrbb8HtkW8sJg0nYMYqYiJnBFnN8EMXSLkUQ+8ph4LgYl+8vUX3hdbIRGUUKFjJ
8bAOruThPaUP32JB13b4ww4rZ7rNIqDzJ2TMi+YgetxTdichbwVChcHCGeXIq8DQ
v6Qt8lhD8g74zeMjGlUvrJb5cEhtEQIDAQABMA0GCSqGSIb3DQEBCwUAA4IBAQAK
dPpw5OP8sGocCs/P43o8rSkFJPn7LdTkfCTyBWyjp9WjWztBelPsTw99Stsy/bgr
LOFkNtimtZVlv0SWKO9ZXVjkVF3JdMsy2mRlTy9530Bk9H/UJChJaX2Q9cwNivZX
SJT7Psv+gypR1pwU6Mp0mELXunnQndsuaZ+mzHbzVcci+c3nO/7g4xRNWNbTeCas
gNI1Nqt21+/kWwgpkuBbphSJUrTKE1NkVMsh/bfzDNTe2UiDszuU1Aq1HuctHilJ
I2RIXDu4xLSHFyHtsn2OKQyLzCAUCTOlFzpwUgjj917chG4cLGiy0ARQh+6q1+lM
4oW1jtacEQ0hW1u2y2De
-----END CERTIFICATE-----)");

    // Ensure TLS connection to SGW
    if(!Address::isSecure(_sg.address)) {
        _sg.address = {kC4Replicator2TLSScheme,
                       C4STR("localhost"),
                       4984};
    }
    REQUIRE(Address::isSecure(_sg.address));
    
    initTest();

    // Push (if certificate not accepted by SGW, will crash as expectSuccess is true)
    ReplParams replParams { kC4OneShot, kC4Disabled };
    replicate(replParams);

    // Intermediate cert (Replicator/tests/data/cert/sg_cert.pem (2nd cert))
    _sg.pinnedCert = slice(R"(-----BEGIN CERTIFICATE-----
MIIDRzCCAi+gAwIBAgIUNts/9gIBEy+cXri5JRHZuXbRkPQwDQYJKoZIhvcNAQEL
BQAwHDEaMBgGA1UEAwwRQ291Y2hiYXNlIFJvb3QgQ0EwHhcNMjMwMTI1MTcyNTM1
WhcNMzMwMTIyMTcyNTM1WjAQMQ4wDAYDVQQDDAVJbnRlcjCCASIwDQYJKoZIhvcN
AQEBBQADggEPADCCAQoCggEBAKfT6m0Nby0BMDU/IW4aGqAO5w2i+W5Vn6V2E4Og
lNqweBDg+pPWwGyacaGXgsWMcFtxtxsmBDVRIuLzgo/tXDtN7yNdlGVq9WiOtbWB
ovKq0KiFrOGXbKHLPyRahGulXwZ5eI4nLIwPoxk6+q8jEiRzcvAWbKz+Qy51Iygq
k8MRQ8OZkinmWKcJ31cBjMuPzNgPCWn18iU7jkes5M0rBTK4M98gkR2SaqAo1L1b
QDLiEZRWD0dlwxkLgIWqjFj1yW3iVf/jILPuS4XK4C6byGewSVsS5f7OjXDrAuVI
igEbhRlTNEmsTfYjGBLNkbPRNM0VWEMc9gmtzbT5VZr7Ir8CAwEAAaOBjDCBiTAP
BgNVHRMBAf8EBTADAQH/MB0GA1UdDgQWBBRloKIjYpry1TzFRKj3gMhTfN2fjzBX
BgNVHSMEUDBOgBQWNMmtETrZ1TO4Q6L+7enjksvyGKEgpB4wHDEaMBgGA1UEAwwR
Q291Y2hiYXNlIFJvb3QgQ0GCFEdmMdLR5K2lSu89v4YGnYd/hWQTMA0GCSqGSIb3
DQEBCwUAA4IBAQCORuTuWd2nWEl1DjcpUVXnbE4S6xG4YjC5VfGj36Gj5bjjZj+y
S4TWigwLvc8Rokx+ZqLHyTgrPcLKl/6DrFNNGZC6ByMEDH0XQQWYCLHDAfgkhBng
qD8eZmZ8tYvkZHf4At35RGfiZAtJBNrfxFtKodT0SeUT+qwGcuVLU5B6vgsH/Gib
82cxMLnXcqbyX2rW2yGpypB8Qb+K8qaotFqxxRFRT0+n40Bh86G8ik5/vEuYvlnv
nLMtWOJixTekuOrOh8TB0DgDVIx9gGu4xv4SYGKqseb9z4teJpSaI7LKws0buuHu
G6SJD+EJQ4UPaeYNjnFeh0DNlIHBkkZhdDtw
-----END CERTIFICATE-----)");

    replicate(replParams);

    // Root cert (Replicator/tests/data/cert/sg_cert.pem (3rd cert))
    _sg.pinnedCert = slice(R"(-----BEGIN CERTIFICATE-----
MIIDUzCCAjugAwIBAgIUR2Yx0tHkraVK7z2/hgadh3+FZBMwDQYJKoZIhvcNAQEL
BQAwHDEaMBgGA1UEAwwRQ291Y2hiYXNlIFJvb3QgQ0EwHhcNMjMwMTI1MTcyNTM1
WhcNMzMwMTIyMTcyNTM1WjAcMRowGAYDVQQDDBFDb3VjaGJhc2UgUm9vdCBDQTCC
ASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBANnHe9guNaE6Epcchx72GJy3
Tn4lmd0tcCBviZIti4FfyFu2tFai6S7Mj0JHWltuaLv5AD402dxb8gxG3ZKIPOPt
b38I/yJbQSs+ND3Ee056R5qnV22Fuw37X5Bu9+dZn1YgSM7lt1RnqpgW/yxLii8q
J5pRG6AUsIsr3NAE3EcLWcRA3kW1vinmm9bI1wD+lJBo9v3QJOXw+ndEWtcu5hqC
r4gQcGDvnOGTbaHOrhMIDgkl46gJSi3j2NNX093SlK23/84ZZmJOESHpE+1+JkeL
z6gawOmR8wHBlixOV1Y7SZrGPJ9Vp1cFqeUnDqButad+2C1cXZ2XlTUi5t32IIsC
AwEAAaOBjDCBiTAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQWBBQWNMmtETrZ1TO4
Q6L+7enjksvyGDBXBgNVHSMEUDBOgBQWNMmtETrZ1TO4Q6L+7enjksvyGKEgpB4w
HDEaMBgGA1UEAwwRQ291Y2hiYXNlIFJvb3QgQ0GCFEdmMdLR5K2lSu89v4YGnYd/
hWQTMA0GCSqGSIb3DQEBCwUAA4IBAQBIXmvcoWW0VZmjSEUmwFcyWq+38/AbPfRs
0MbhpHBvCau7/wOyTI/cq838yJYL+71BmXJNKFp8nF7Yc+PU6UkypXCsj2rHpblz
2bkjHJoEGw/HIPFo/ZywUiGfb/Jc6/t2PdHHBSkZO28oRnAt+q2Ehvqf/iT9bHO8
068JQXO5ttsA8JFQu26Thk/37559sruAn8/Lz3b8P6s6Ql3gg2LmCAh9v7gIcj64
kr6iDunu9X9glrd+1DV9otDwXh1iM2kd7MrCituUgTt7tclDFQMxuSSW2mc3k51Y
E1/H1T7j/M/LhIzUPNO80oPxLXl3TQFc+ZYwh5nSHeHbo91dY+vj
-----END CERTIFICATE-----)");

    replicate(replParams);
}

TEST_CASE_METHOD(ReplicatorSGTest, "Pinned Certificate Failure - SG", "[.SyncServerSG]") {
    if(!Address::isSecure(_sg.address)) {
        _sg.address = {kC4Replicator2TLSScheme,
                       C4STR("localhost"),
                       4984 };
    }
    REQUIRE(Address::isSecure(_sg.address));

    // Using an unmatched pinned cert:
    _sg.pinnedCert =                                                               \
        "-----BEGIN CERTIFICATE-----\r\n"                                      \
        "MIICpDCCAYwCCQCskbhc/nbA5jANBgkqhkiG9w0BAQsFADAUMRIwEAYDVQQDDAls\r\n" \
        "b2NhbGhvc3QwHhcNMjIwNDA4MDEwNDE1WhcNMzIwNDA1MDEwNDE1WjAUMRIwEAYD\r\n" \
        "VQQDDAlsb2NhbGhvc3QwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDQ\r\n" \
        "vl0M5D7ZglW76p428x7iQoSkhNyRBEjZgSqvQW3jAIsIElWu7mVIIAm1tpZ5i5+Q\r\n" \
        "CHnFLha1TDACb0MUa1knnGj/8EsdOADvBfdBq7AotypiqBayRUNdZmLoQEhDDsen\r\n" \
        "pEHMDmBrDsWrgNG82OMFHmjK+x0RioYTOlvBbqMAX8Nqp6Yu/9N2vW7YBZ5ovsr7\r\n" \
        "vdFJkSgUYXID9zw/MN4asBQPqMT6jMwlxR1bPqjsNgXrMOaFHT/2xXdfCvq2TBXu\r\n" \
        "H7evR6F7ayNcMReeMPuLOSWxA6Fefp8L4yDMW23jizNIGN122BgJXTyLXFtvg7CQ\r\n" \
        "tMnE7k07LLYg3LcIeamrAgMBAAEwDQYJKoZIhvcNAQELBQADggEBABdQVNSIWcDS\r\n" \
        "sDPXk9ZMY3stY9wj7VZF7IO1V57n+JYV1tJsyU7HZPgSle5oGTSkB2Dj1oBuPqnd\r\n" \
        "8XTS/b956hdrqmzxNii8sGcHvWWaZhHrh7Wqa5EceJrnyVM/Q4uoSbOJhLntLE+a\r\n" \
        "FeFLQkPpJxdtjEUHSAB9K9zCO92UC/+mBUelHgztsTl+PvnRRGC+YdLy521ST8BI\r\n" \
        "luKJ3JANncQ4pCTrobH/EuC46ola0fxF8G5LuP+kEpLAh2y2nuB+FWoUatN5FQxa\r\n" \
        "+4F330aYRvDKDf8r+ve3DtchkUpV9Xa1kcDFyTcYGKBrINtjRmCIblA1fezw59ZT\r\n" \
        "S5TnM2/TjtQ=\r\n"                                                     \
        "-----END CERTIFICATE-----\r\n";

    // One-shot push setup
    ReplParams replParams { kC4OneShot, kC4Disabled };

    // expectSuccess = false so we can check the error code
    replicate(replParams, false);
    CHECK(_callbackStatus.error.domain == NetworkDomain);
    CHECK(_callbackStatus.error.code == kC4NetErrTLSCertUntrusted);
}
#endif //#ifdef COUCHBASE_ENTERPRISE

TEST_CASE_METHOD(ReplicatorSGTest, "Auto Purge Enabled - Revoke Access - SG", "[.SyncServerSG]") {
    const string idPrefix = timePrefix();
    const string docIDstr = idPrefix + "apera-doc1";
    const string channelIDa = idPrefix + "a";
    const string channelIDb = idPrefix + "b";

    initTest( { channelIDa, channelIDb });

    // Setup pull filter:
    _pullFilter = [](C4String collectionName, C4String docID, C4String revID,
                     C4RevisionFlags flags, FLDict flbody, void *context) {
        if ((flags & kRevPurged) == kRevPurged) {
            ((ReplicatorAPITest*)context)->_counter++;
            Dict body(flbody);
            CHECK(body.count() == 0);
        }
        return true;
    };

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl,
                      bool pushing,
                      size_t numDocs,
                      const C4DocumentEnded* docs[],
                      void* context) {
        for (size_t i = 0; i < numDocs; ++i) {
            auto doc = docs[i];
            if ((doc->flags & kRevPurged) == kRevPurged) {
                ((ReplicatorAPITest*)context)->_docsEnded++;
            }
        }
    };

    // Put doc in remote DB, in channels a and b
    REQUIRE(_sg.upsertDoc(docIDstr, "{}", { channelIDa, channelIDb } ));

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    ReplParams replParams { kC4Disabled, kC4OneShot };
    replParams.setPullFilter(_pullFilter).setCallbackContext(this);
    replicate(replParams);

    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);

    // Revoke access to channel 'a':
    REQUIRE(_testUser.setChannels({ channelIDb }));

    // Verify
    c4::ref<C4Document> doc1 = c4db_getDoc(db, slice(docIDstr), true, kDocGetAll, nullptr);
    REQUIRE(doc1);
    CHECK(slice(doc1->revID).hasPrefix("1-"_sl));

    // Update doc to only channel 'b'
    auto oRevID = slice(doc1->revID).asString();
    REQUIRE(_sg.upsertDoc(docIDstr, oRevID, "{}", { channelIDb }));

    C4Log("-------- Pull update");
    replicate(replParams);

    // Verify the update:
    doc1 = c4db_getDoc(db, slice(docIDstr), true, kDocGetAll, nullptr);
    REQUIRE(doc1);
    CHECK(slice(doc1->revID).hasPrefix("2-"_sl));
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);

    // Revoke access to all channels:
    REQUIRE(_testUser.revokeAllChannels());

    C4Log("-------- Pull the revoked");
    replicate(replParams);

    // Verify that doc1 is purged:
    doc1 = c4db_getDoc(db, slice(docIDstr), true, kDocGetAll, nullptr);
    REQUIRE(!doc1);

    CHECK(_docsEnded == 1);
    CHECK(_counter == 1);
}

TEST_CASE_METHOD(ReplicatorSGTest, "Auto Purge Disabled - Revoke Access SG", "[.SyncServerSG]") {
    const string idPrefix = timePrefix();
    const string doc1ID = idPrefix + "doc1";
    const vector<string> chIDs { idPrefix };

    initTest( chIDs );

    REQUIRE(_sg.upsertDoc(doc1ID, "{}"_sl, chIDs));

    // pullFilter and onDocsEnded increment _counter and _docsEnded respectively when a
    // revoked doc is pulled

    // Setup pull filter:
    _pullFilter = [](
            C4String, C4String, C4String, C4RevisionFlags flags, FLDict, void *context)
    {
        if ((flags & kRevPurged) == kRevPurged) {
            ((ReplicatorAPITest*)context)->_counter++;
        }
        return true;
    };

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator*,
                      bool,
                      size_t numDocs,
                      const C4DocumentEnded* docs[],
                      void* context) {
        for (size_t i = 0; i < numDocs; ++i) {
            auto doc = docs[i];
            if ((doc->flags & kRevPurged) == kRevPurged) {
                ((ReplicatorAPITest*)context)->_docsEnded++;
            }
        }
    };

    // Replication parameters setup
    ReplParams replParams { kC4Disabled, kC4OneShot };
    replParams.setOption(kC4ReplicatorOptionAutoPurge, false)
              .setPullFilter(_pullFilter)
              .setDocsEndedCallback(_onDocsEnded)
              .setCallbackContext(this);

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    replicate(replParams);

    c4::ref<C4Document> doc1 = c4db_getDoc(db, slice(doc1ID),
                                             true, kDocGetCurrentRev, nullptr);
    // Verify doc has been pulled and not purged
    REQUIRE(doc1);
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);

    // Revoke access to all channels:
    REQUIRE(_testUser.revokeAllChannels());

    C4Log("-------- Pulling the revoked");
    replicate(replParams);

    // Verify doc1 is not purged as auto purge is disabled:
    doc1 = c4db_getDoc(db, slice(doc1ID), true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc1);
    CHECK(_docsEnded == 1);
    CHECK(_counter == 0);
}


TEST_CASE_METHOD(ReplicatorSGTest, "Remove Doc From Channel SG", "[.SyncServerSG]") {
    string idPrefix = timePrefix();
    string        doc1ID {idPrefix + "doc1"};
    vector<string> chIDs {idPrefix+"a", idPrefix+"b"};

    initTest( chIDs);

    _sg.upsertDoc(doc1ID, "{}"_sl, chIDs);

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl,
                      bool pushing,
                      size_t numDocs,
                      const C4DocumentEnded* docs[],
                      void* context) {
        for (size_t i = 0; i < numDocs; ++i) {
            auto doc = docs[i];
            if ((doc->flags & kRevPurged) == kRevPurged) {
                ((ReplicatorAPITest*)context)->_docsEnded++;
            }
        }
    };

    // Setup pull filter:
    C4ReplicatorValidationFunction pullFilter = [](
            C4String, C4String, C4String, C4RevisionFlags flags, FLDict flbody, void *context)
    {
        if ((flags & kRevPurged) == kRevPurged) {
            ((ReplicatorAPITest*)context)->_counter++;
            Dict body(flbody);
            CHECK(body.count() == 0);
        }
        return true;
    };

    // Pull doc into CBL:
    C4Log("-------- Pulling");

    bool autoPurgeEnabled {true};
    ReplParams replParams { kC4Disabled, kC4OneShot };
    replParams.setPullFilter(pullFilter).setCallbackContext(this);

    SECTION("Auto Purge Enabled") {
        autoPurgeEnabled = true;
    }

    SECTION("Auto Purge Disabled") {
        replParams.setOption(C4STR(kC4ReplicatorOptionAutoPurge), false);
        autoPurgeEnabled = false;
    }

    replicate(replParams);

    // Verify doc
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);
    c4::ref<C4Document> doc1 = c4db_getDoc(db, slice(doc1ID),
                                             true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc1);
    CHECK(c4rev_getGeneration(doc1->revID) == 1);

    // Once verified, remove it from channel 'a' in that collection
    auto oRevID = slice(doc1->revID).asString();
    _sg.upsertDoc(doc1ID, R"({"_rev":")" + oRevID + "\"}", { chIDs[1] });

    C4Log("-------- Pull update");
    replicate(replParams);

    // Verify the update:
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);
    doc1 = c4db_getDoc(db, slice(doc1ID), true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc1);
    CHECK(c4rev_getGeneration(doc1->revID) == 2);

    // Remove doc from all channels:
    oRevID = slice(doc1->revID).asString();
    _sg.upsertDoc(doc1ID, R"({"_rev":")" + oRevID + "\"}", {});

    C4Log("-------- Pull the removed");
    replicate(replParams);

    doc1 = c4db_getDoc(db, slice(doc1ID), true, kDocGetCurrentRev, nullptr);

    if (autoPurgeEnabled) {
        // Verify if doc1 is purged:
        REQUIRE(!doc1);
    } else {
        REQUIRE(doc1);
    }

    CHECK(_docsEnded == 1);
    if(autoPurgeEnabled) {
        CHECK(_counter == 1);
    } else {
        // No pull filter called
        CHECK(_counter == 0);
    }
}
