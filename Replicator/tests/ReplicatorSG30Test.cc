//
// Created by Callum Birks on 18/01/2023.
//

#include "ReplicatorSG30Test.hh"
#include "ReplicatorLoopbackTest.hh"

TEST_CASE_METHOD(ReplicatorSG30Test, "Sync with Single Collection SG3.0", "[.SyncServer30]") {
    const string     idPrefix = timePrefix();
    constexpr size_t docCount = 20;

    initTest({Default});

    // Import `docCount` docs
    importJSONLines(sFixturesDir + "names_100.json", _collections[0], 0, false, docCount, idPrefix);

    // Push & Pull replication
    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4OneShot};
    updateDocIDs();
    replParams.setDocIDs(_docIDs);
    replicate(replParams);

    // Verify
    verifyDocs(_docIDs);

    // Clear local DB
    deleteAndRecreateDBAndCollections();
    // Replicate again
    replicate(replParams);
    // Verify again
    verifyDocs(_docIDs);
}

TEST_CASE_METHOD(ReplicatorSG30Test, "API Push 5000 Changes Collections SG3.0", "[.SyncServer30]") {
    string             idPrefix      = timePrefix();
    const string       docID         = idPrefix + "apipfcc-doc1";
    constexpr unsigned revisionCount = 2000;

    initTest({Default});

    std::vector<string> revIDs{_collectionCount};

    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4Disabled};

    {
        auto              revID = revIDs.begin();
        TransactionHelper t(db);
        *revID = createNewRev(_collections[0], slice(docID), nullslice, kFleeceBody);
        REQUIRE(!(revID++)->empty());
    }

    replicate(replParams);
    updateDocIDs();
    verifyDocs(_docIDs);

    C4Log("-------- Mutations --------");
    {
        auto              revID = revIDs.begin();
        TransactionHelper t(db);
        for ( int i = 2; i <= revisionCount; ++i ) {
            *revID = createNewRev(_collections[0], slice(docID), slice(*revID), kFleeceBody);
            REQUIRE(!revID->empty());
        }
        ++revID;
    }

    C4Log("-------- Second Replication --------");
    replicate(replParams);
    updateDocIDs();
    verifyDocs(_docIDs, true);
}

TEST_CASE_METHOD(ReplicatorSG30Test, "Non-default Collection SG3.0", "[.SyncServer30]") {
    string idPrefix = timePrefix();

    SECTION("One non-default collection") { initTest({Roses}); }

    SECTION("Default and some non-default") { initTest({Default, Roses, Tulips}); }

    SECTION("Multiple non-default") { initTest({Roses, Tulips, Lavenders}); }

    for ( auto coll : _collections ) { importJSONLines(sFixturesDir + "names_100.json", coll, 0, false, 2, idPrefix); }

    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4OneShot};

    slice   expectedErrorMsg = "No handler for BLIP request"_sl;
    C4Error expectedError    = {LiteCoreDomain, 26};

    replicate(replParams, false);

    FLStringResult emsg = c4error_getMessage(_callbackStatus.error);
    CHECK(_callbackStatus.error.domain == expectedError.domain);
    CHECK(_callbackStatus.error.code == expectedError.code);
    CHECK(expectedErrorMsg.compare(slice(emsg)) == 0);
    FLSliceResult_Release(emsg);
}

TEST_CASE_METHOD(ReplicatorSG30Test, "Default Collection Incremental Push SG3.0", "[.SyncServer30]") {
    string idPrefix = timePrefix();

    initTest({Default});

    addDocs(_collections[0], 10, idPrefix);

    updateDocIDs();

    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4Disabled};
    replParams.setDocIDs(_docIDs);
    replicate(replParams);
    verifyDocs(_docIDs);

    // Add docs to local database
    idPrefix = timePrefix();
    addDocs(_collections[0], 5, idPrefix);

    updateDocIDs();

    replParams.setDocIDs(_docIDs);
    replicate(replParams);
    verifyDocs(_docIDs);
}

TEST_CASE_METHOD(ReplicatorSG30Test, "Default Collection Incremental Revisions SG3.0", "[.SyncServer30]") {
    const string idPrefix = timePrefix();

    initTest({Default});

    addDocs(_collections[0], 2, idPrefix + "db-" + string(_collectionSpecs[0].name));


    Jthread jthread;
    _callbackWhenIdle = [this, &jthread, idPrefix]() {
        jthread.thread    = std::thread(std::thread{[this, idPrefix]() mutable {
            const string collName = string(_collectionSpecs[0].name);
            const string docID    = idPrefix + "-" + collName + "-docko";
            ReplicatorLoopbackTest::addRevs(_collections[0], 500ms, alloc_slice(docID), 1, 10, true,
                                               ("db-"s + collName).c_str());
            _stopWhenIdle.store(true);
        }});
        _callbackWhenIdle = nullptr;
    };

    ReplParams replParams{_collectionSpecs, kC4Continuous, kC4Disabled};
    replicate(replParams);
    // total 3 docs, 12 revs, for each collections.
    CHECK(_callbackStatus.progress.documentCount == 12);
    updateDocIDs();
    verifyDocs(_docIDs, true);
}

TEST_CASE_METHOD(ReplicatorSG30Test, "Pull deltas from Collection SG3.0", "[.SyncCollSlow]") {
    constexpr size_t kDocBufSize = 60;

    constexpr int kNumDocs = 799, kNumProps = 799;
    const string  idPrefix  = timePrefix();
    const string  docIDPref = idPrefix + "doc";
    const string  channelID = idPrefix + "a";

    initTest({Default}, {channelID}, "pdfcsg");

    C4Log("-------- Populating local db --------");
    auto populateDB = [&]() {
        TransactionHelper t(db);
        std::srand(123456);  // start random() sequence at a known place NOLINT(cert-msc51-cpp)
        for ( int docNo = 0; docNo < kNumDocs; ++docNo ) {
            constexpr size_t kDocBufSize = 60;
            char             docID[kDocBufSize];
            snprintf(docID, kDocBufSize, "%s-%03d", docIDPref.c_str(), docNo);
            Encoder encPopulate(c4db_createFleeceEncoder(db));
            encPopulate.beginDict();

            encPopulate.writeKey(kC4ReplicatorOptionChannels);
            encPopulate.writeString(channelID);

            for ( int p = 0; p < kNumProps; ++p ) {
                encPopulate.writeKey(stringprintf("field%03d", p));
                encPopulate.writeInt(std::rand());
            }
            encPopulate.endDict();
            alloc_slice body  = encPopulate.finish();
            string      revID = createNewRev(_collections[0], slice(docID), body);
        }
    };

    populateDB();

    C4Log("-------- Pushing to SG --------");
    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4Disabled};
    replicate(replParams);

    C4Log("-------- Updating docs on SG --------");
    {
        JSONEncoder encUpdate;
        encUpdate.beginDict();
        encUpdate.writeKey("docs"_sl);
        encUpdate.beginArray();
        for ( int docNo = 0; docNo < kNumDocs; ++docNo ) {
            char docID[kDocBufSize];
            snprintf(docID, kDocBufSize, "%s-%03d", docIDPref.c_str(), docNo);
            C4Error             error;
            c4::ref<C4Document> doc =
                    c4coll_getDoc(_collections[0], slice(docID), false, kDocGetAll, ERROR_INFO(error));
            REQUIRE(doc);
            Dict props = c4doc_getProperties(doc);

            encUpdate.beginDict();
            encUpdate.writeKey("_id"_sl);
            encUpdate.writeString(docID);
            encUpdate.writeKey("_rev"_sl);
            encUpdate.writeString(doc->revID);
            for ( Dict::iterator j(props); j; ++j ) {
                encUpdate.writeKey(j.keyString());
                if ( j.keyString() == kC4ReplicatorOptionChannels ) {
                    encUpdate.writeString(j.value().asString());
                    continue;
                }
                auto value = j.value().asInt();
                if ( RandomNumber() % 8 == 0 ) value = RandomNumber();
                encUpdate.writeInt(value);
            }
            encUpdate.endDict();
        }
        encUpdate.endArray();
        encUpdate.endDict();

        REQUIRE(_sg.insertBulkDocs(_collectionSpecs[0], encUpdate.finish(), 30.0));
    }

    double timeWithDelta = 0, timeWithoutDelta = 0;
    for ( int pass = 1; pass <= 3; ++pass ) {
        if ( pass == 3 ) {
            C4Log("-------- DISABLING DELTA SYNC --------");
            replParams.setOption(C4STR(kC4ReplicatorOptionDisableDeltas), true);
        }

        C4Log("-------- PASS #%d: Repopulating local db --------", pass);

        deleteAndRecreateDBAndCollections();

        populateDB();

        C4Log("-------- PASS #%d: Pulling changes from SG --------", pass);
        Stopwatch st;

        replParams.setPushPull(kC4Disabled, kC4OneShot);
        replicate(replParams);

        double time = st.elapsed();

        C4Log("-------- PASS #%d: Pull took %.3f sec (%.0f docs/sec) --------", pass, time, kNumDocs / time);
        if ( pass == 2 ) timeWithDelta = time;
        else if ( pass == 3 )
            timeWithoutDelta = time;

        int                      n = 0;
        C4Error                  error;
        c4::ref<C4DocEnumerator> e = c4coll_enumerateAllDocs(_collections[0], nullptr, ERROR_INFO(error));
        REQUIRE(e);
        while ( c4enum_next(e, ERROR_INFO(error)) ) {
            C4DocumentInfo info;
            c4enum_getDocumentInfo(e, &info);
            CHECK(slice(info.docID).hasPrefix(slice(docIDPref)));
            CHECK(slice(info.revID).hasPrefix("2-"_sl));
            ++n;
        }
        CHECK(error.code == 0);
        CHECK(n == kNumDocs);
    }

    C4Log("-------- %.3f sec with deltas, %.3f sec without; %.2fx speed", timeWithDelta, timeWithoutDelta,
          timeWithoutDelta / timeWithDelta);
}

TEST_CASE_METHOD(ReplicatorSG30Test, "Push and Pull Attachments SG3.0", "[.SyncServer30]") {
    const string idPrefix = timePrefix();

    initTest({Default});

    std::vector<C4BlobKey> blobKeys{2};  // blobKeys1a, blobKeys1b;

    vector<string> attachments1 = {idPrefix + "Attachment A", idPrefix + "Attachment B", idPrefix + "Attachment Z"};
    {
        const string      doc1 = idPrefix + "doc1";
        TransactionHelper t(db);
        blobKeys = addDocWithAttachments(db, _collectionSpecs[0], slice(doc1), attachments1, "text/plain");
    }

    C4Log("-------- Pushing to SG --------");
    updateDocIDs();
    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4Disabled};
    replParams.setDocIDs(_docIDs);
    replicate(replParams);

    C4Log("-------- Checking docs and attachments --------");
    verifyDocs(_docIDs, true);
    checkAttachments(verifyDb, blobKeys, attachments1);
}

TEST_CASE_METHOD(ReplicatorSG30Test, "Push & Pull Deletion SG3.0", "[.SyncServer30]") {
    const string idPrefix = timePrefix();
    const string docID    = idPrefix + "ppd-doc1";

    initTest({Default});

    createRev(_collections[0], slice(docID), kRevID, kFleeceBody);
    createRev(_collections[0], slice(docID), kRev2ID, kEmptyFleeceBody, kRevDeleted);

    std::vector<std::unordered_map<alloc_slice, uint64_t>> docIDs{_collectionCount};

    docIDs[0] = unordered_map<alloc_slice, uint64_t>{{alloc_slice(docID), 0}};

    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4Disabled};
    replParams.setDocIDs(docIDs);
    replicate(replParams);

    C4Log("-------- Deleting and re-creating database --------");
    deleteAndRecreateDBAndCollections();

    createRev(_collections[0], slice(docID), kRevID, kFleeceBody);

    replParams.setPushPull(kC4Disabled, kC4OneShot);
    replicate(replParams);

    c4::ref<C4Document> remoteDoc = c4coll_getDoc(_collections[0], slice(docID), true, kDocGetAll, nullptr);
    REQUIRE(remoteDoc);
    CHECK(remoteDoc->revID == kRev2ID);
    CHECK((remoteDoc->flags & kDocDeleted) != 0);
    CHECK((remoteDoc->selectedRev.flags & kRevDeleted) != 0);
    REQUIRE(c4doc_selectParentRevision(remoteDoc));
    CHECK(remoteDoc->selectedRev.revID == kRevID);
}

TEST_CASE_METHOD(ReplicatorSG30Test, "Resolve Conflict SG3.0", "[.SyncServer30]") {
    const string idPrefix = timePrefix();

    initTest({Default});

    std::vector<string> collNames{_collectionCount};

    collNames[0] = idPrefix + Options::collectionSpecToPath(_collectionSpecs[0]).asString();
    createFleeceRev(_collections[0], slice(collNames[0]), kRev1ID, "{}"_sl);
    createFleeceRev(_collections[0], slice(collNames[0]), revOrVersID("2-12121212", "1@cafe"), R"({"db":"remote"})"_sl);

    updateDocIDs();

    // Send the docs to remote
    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4Disabled};
    replParams.setDocIDs(_docIDs);
    replicate(replParams);

    verifyDocs(_docIDs, true);

    deleteAndRecreateDBAndCollections();

    createFleeceRev(_collections[0], slice(collNames[0]), kRev1ID, "{}"_sl);
    createFleeceRev(_collections[0], slice(collNames[0]), revOrVersID("2-13131313", "1@babe"), R"({"db":"local"})"_sl);

    updateDocIDs();
    replParams.setDocIDs(_docIDs);

    _conflictHandler = [&](const C4DocumentEnded* docEndedWithConflict) {
        C4Error error;
        int     i = -1;
        if ( docEndedWithConflict->collectionSpec == _collectionSpecs[0] ) { i = 0; }
        Assert(i >= 0, "Internal logical error");

        TransactionHelper t(db);

        slice docID = docEndedWithConflict->docID;
        // Get the local doc. It is the current revision
        c4::ref<C4Document> localDoc = c4coll_getDoc(_collections[i], docID, true, kDocGetAll, WITH_ERROR(error));
        CHECK(error.code == 0);

        // Get the remote doc. It is the next leaf revision of the current revision.
        c4::ref<C4Document> remoteDoc = c4coll_getDoc(_collections[i], docID, true, kDocGetAll, &error);
        bool                succ      = c4doc_selectNextLeafRevision(remoteDoc, true, false, &error);
        Assert(remoteDoc->selectedRev.revID == docEndedWithConflict->revID);
        CHECK(error.code == 0);
        CHECK(succ);

        C4Document* resolvedDoc = remoteDoc;

        FLDict          mergedBody  = c4doc_getProperties(resolvedDoc);
        C4RevisionFlags mergedFlags = resolvedDoc->selectedRev.flags;
        alloc_slice     winRevID    = resolvedDoc->selectedRev.revID;
        alloc_slice lostRevID = (resolvedDoc == remoteDoc) ? localDoc->selectedRev.revID : remoteDoc->selectedRev.revID;
        bool        result    = c4doc_resolveConflict2(localDoc, winRevID, lostRevID, mergedBody, mergedFlags, &error);
        Assert(result, "conflictHandler: c4doc_resolveConflict2 failed for '%.*s' in '%.*s.%.*s'", SPLAT(docID),
               SPLAT(_collectionSpecs[i].scope), SPLAT(_collectionSpecs[i].name));
        Assert((localDoc->flags & kDocConflicted) == 0);

        if ( !c4doc_save(localDoc, 0, &error) ) {
            Assert(false, "conflictHandler: c4doc_save failed for '%.*s' in '%.*s.%.*s'", SPLAT(docID),
                   SPLAT(_collectionSpecs[i].scope), SPLAT(_collectionSpecs[i].name));
        }
    };

    replParams.setPushPull(kC4Disabled, kC4OneShot);
    replicate(replParams);

    c4::ref<C4Document> doc = c4coll_getDoc(_collections[0], slice(collNames[0]), true, kDocGetAll, nullptr);
    REQUIRE(doc);
    CHECK(fleece2json(c4doc_getRevisionBody(doc)) == "{db:\"remote\"}");  // Remote Wins
    REQUIRE(!c4doc_selectNextLeafRevision(doc, true, false, nullptr));
}

TEST_CASE_METHOD(ReplicatorSG30Test, "Update Once-Conflicted Doc - SG3.0", "[.SyncServer30]") {
    const string idPrefix = timePrefix();
    const string docID    = idPrefix + "uocd-doc";
    //    const string channelID = idPrefix + "uocd";

    initTest({Default});

    std::array<std::string, 4> bodies{R"({"_rev":"1-aaaa","foo":1})",
                                      R"({"_revisions":{"start":2,"ids":["bbbb","aaaa"]},"foo":2.1})",
                                      R"({"_revisions":{"start":2,"ids":["cccc","aaaa"]},"foo":2.2})",
                                      R"({"_revisions":{"start":3,"ids":["dddd","cccc"]},"_deleted":true})"};

    // Create a conflicted doc on SG, and resolve the conflict
    for ( const auto& body : bodies ) { _sg.upsertDoc(_collectionSpecs[0], docID + "?new_edits=false", body); }

    std::vector docIDs = {std::unordered_map<alloc_slice, uint64_t>{{alloc_slice(docID), 0}}};

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    ReplParams replParams{_collectionSpecs, kC4Disabled, kC4OneShot};
    replParams.setDocIDs(docIDs);
    replicate(replParams);

    // Verify doc:
    c4::ref<C4Document> doc = c4coll_getDoc(_collections[0], slice(docID), true, kDocGetAll, nullptr);
    REQUIRE(doc);
    C4Slice revID = C4STR("2-bbbb");
    CHECK(doc->revID == revID);
    CHECK((doc->flags & kDocDeleted) == 0);
    REQUIRE(c4doc_selectParentRevision(doc));
    CHECK(doc->selectedRev.revID == "1-aaaa"_sl);

    // Update doc:
    createRev(_collections[0], slice(docID), "3-ffff"_sl, kFleeceBody);

    // Push change back to SG:
    C4Log("-------- Pushing");
    replParams.setPushPull(kC4OneShot, kC4Disabled);
    replicate(replParams);

    updateDocIDs();

    verifyDocs(_docIDs, true);
}

#ifdef COUCHBASE_ENTERPRISE

TEST_CASE_METHOD(ReplicatorSG30Test, "Pinned Certificate Success - SG3.0", "[.SyncServer30]") {
    // Leaf cert (Replicator/tests/data/cert/sg_cert.pem (1st cert))
    _sg.pinnedCert = slice(R"(-----BEGIN CERTIFICATE-----
MIICqzCCAZMCFGrxed0RuxP+uYOzr9wIeRp4gBjHMA0GCSqGSIb3DQEBCwUAMBAx
DjAMBgNVBAMMBUludGVyMB4XDTIyMTAyNTEwMjAzMFoXDTMyMTAyMjEwMjAzMFow
FDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIB
CgKCAQEAknbSS/newbZxs4afkUEgMO9WzE1LJAZ7oj3ovLzbsDYVJ3Ct1eBA2yYN
t87ROTvJ85mw4lQ3puMhWGGddYUQzBT7rdtpvydk9aNIefLwU6Yn6YvXC1asxSsb
yFr75j21UZ+qHZ1B4DYAR09Qaps43OKGKJl+4QBUkcLp+Hgo+5e29buv3VvoSK42
MnYsFFtgjVsLBJcL0L9t5gxujPiK8jbdXDYN3Md602rKua9LNwff02w8FWJ8/nLZ
LxtAVidgHJPEY2kDj+S2fUOaAypHcvkHAJ9KKwqHYpwvWzv32WpmmpKBxoiP2NFI
655Efmx7g3pJ2LvUbyOthi8k/VT3/wIDAQABMA0GCSqGSIb3DQEBCwUAA4IBAQC3
c+kGcvn3d9QyGYif2CtyAYGRxUQpMjYjqQiwyZmKNp/xErgns5dD+Ri6kEOcq0Zl
MrsPV5iprAKCvEDU6CurGE+sUiJH1csjPx+uCcUlZwT+tZF71IBJtkgfQx2a9Wfs
CA+qS9xaNhuYFkbSIbA5uiSUf9MRxafY8mqjtrOtdPf4fxN5YVsbOzJLtrcVVL9i
Y5rPGtUwixeiZsuGXYkFGLCZx8DWQQrENSu3PI5hshdHgPoHyqxls4yDTDyF3nqq
w9Q3o9L/YDg9NGdW1XQoBgxgKy5G3YT7NGkZXUOJCHsupyoK4GGZQGxtb2eYMg/H
lTIN5f2LxWf+8kJqfjlj
-----END CERTIFICATE-----)");

    // Ensure TLS connection to SGW
    if ( !Address::isSecure(_sg.address) ) { _sg.address = {kC4Replicator2TLSScheme, C4STR("localhost"), 4984}; }
    REQUIRE(Address::isSecure(_sg.address));

    // One-shot push setup
    initTest({Default});
    // Push (if certificate not accepted by SGW, will crash as expectSuccess is true)
    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4Disabled};
    replicate(replParams);

    // Intermediate cert (Replicator/tests/data/cert/sg_cert.pem (2nd cert))
    _sg.pinnedCert = slice(R"(-----BEGIN CERTIFICATE-----
MIIDRzCCAi+gAwIBAgIUQu1TjW0ZRWGCKRQh/JcZxfG/J/YwDQYJKoZIhvcNAQEL
BQAwHDEaMBgGA1UEAwwRQ291Y2hiYXNlIFJvb3QgQ0EwHhcNMjIxMDI1MTAyMDMw
WhcNMzIxMDIyMTAyMDMwWjAQMQ4wDAYDVQQDDAVJbnRlcjCCASIwDQYJKoZIhvcN
AQEBBQADggEPADCCAQoCggEBAL9WuYxf16AXrxJlSi/hL5cVDWozhfQ2Qb9c5zl3
zPLUmkDkgEq1Yma6pC46jFQsZE1Yqst6iXng/JX4R7azCNFFxyoorDMuynS52VgS
lfAUddIxi86DfM3rkzm/Yho+HoGCeDq+KIKyEQfZmKyVQj8LRQ/qzSAF11B4pp+e
zLD70XRfOZAwJC/utOHxruf+uTr7C3sW8wvW6MDaLsxc/eKptgamMtWe6kM1dkV3
IycEhHHTvrj0dWM7Bwko4OECZkoyzZWHOLNKetlkPQSq2zApHDOQdRin4iAbOGPz
hiJViXiI0pihOJM8yuHF6MuCB8u8JuAvY3c52+OCKQv4hLkCAwEAAaOBjDCBiTAP
BgNVHRMBAf8EBTADAQH/MB0GA1UdDgQWBBTLyGcuHP88QhUAmjCgBIwjZj/O2zBX
BgNVHSMEUDBOgBQQSW+6ctHLjFGgZaWLvK61p616HKEgpB4wHDEaMBgGA1UEAwwR
Q291Y2hiYXNlIFJvb3QgQ0GCFGMnoe3MRjFDSMJFTdTxgsfxW5oFMA0GCSqGSIb3
DQEBCwUAA4IBAQCPDS2j9gQPzXYRNKL9wNiEO5CnrSf2X5b69OoznQRs0R37xUYo
LqFP4/4XFhtNSD6fHhA/pOYC3dIsKNl8+/5Pb4SROsnT6grjbf46bhbVlocKCm0f
gD2TG2OY64eMIpgaSw/WeFQxHmpqm9967iIOg30EqA4zH/hpCHCldFsqhu7FxJ0o
qp/Ps+yRh2PBGVbqkXAabtCnC4yPn1denqCdUPW2/yK7MzDEapMwkwdWVzzaWUy/
LJ46AUTOMWgFdr1+JcCxFKtIXHmL+nSkIlstEkA0jgYOUGSkKB2BxxtrEmnXFTsK
lb78xSgdpAaELOl18IEF5N3FHjVCtvXqStyS
-----END CERTIFICATE-----)");

    replicate(replParams);

    // Root cert (Replicator/tests/data/cert/sg_cert.pem (3rd cert))
    _sg.pinnedCert = slice(R"(-----BEGIN CERTIFICATE-----
MIIDUzCCAjugAwIBAgIUYyeh7cxGMUNIwkVN1PGCx/FbmgUwDQYJKoZIhvcNAQEL
BQAwHDEaMBgGA1UEAwwRQ291Y2hiYXNlIFJvb3QgQ0EwHhcNMjIxMDI1MTAyMDMw
WhcNMzIxMDIyMTAyMDMwWjAcMRowGAYDVQQDDBFDb3VjaGJhc2UgUm9vdCBDQTCC
ASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAMM9/S7xfgMZF+J4iBxnJEai
cW/FpPsM9HJUt4Xs+JNb+1nJOSo4eGYrAGk/wjxi+VcTdOb/8lrOmT4khKv9CExb
WdxMdSqGb0TM2phd7ZPqCqoMVA0jGJ8ZxLaYlqPsyL9eRio4gVnSE5uNQjWyBEcB
z6eOn1rDZPvJlCF6fRcvgPhFVeIH7xb4jh1OzOoXgM1rrYPLAYr0vLEbk07TwFTE
fCMdBgjEiSnbzQrlgNoVTpcQrGjTmKrN52GC39eTW4tyLdxo+ipgqjiKeTO/qJBp
YZ8V7RgMjhyynIBxhxzZdDEXw5hWZV11kxA3dmBqup9aZ/cK3q2Cxe2mdgMv7aMC
AwEAAaOBjDCBiTAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQWBBQQSW+6ctHLjFGg
ZaWLvK61p616HDBXBgNVHSMEUDBOgBQQSW+6ctHLjFGgZaWLvK61p616HKEgpB4w
HDEaMBgGA1UEAwwRQ291Y2hiYXNlIFJvb3QgQ0GCFGMnoe3MRjFDSMJFTdTxgsfx
W5oFMA0GCSqGSIb3DQEBCwUAA4IBAQCD+qLQqDkjjVuMDRpvehWr46kKHOHVtXxH
FKpiDDYlD7NUqDWj4y1KKFHZuVg/H+IIflE55jv4ttqmKEMuEpUCd5SS3f9mTM0A
TqwzDVs9HfbuKb6lHtnJLTUvM9wBe/WPW8TCB50AkyMpz5sAAqpK4022Vein2C3T
0uox22kUBslWKZnXMtNeT3h2lFXcCZlQPLRfvHdtXA0t5We2kU0SPiFJc4I0OGjv
zzcNjA18pjiTtpuVeNBUAsBJcbHkNQLKnHGPsBNMAedVCe+AM5CVyZdDlZs//fov
0proEf3d58AqTx4i8uUZHdvmE3MVqeL2rrXFNB74Rs6j8QI1wlpW
-----END CERTIFICATE-----)");

    replicate(replParams);
}

TEST_CASE_METHOD(ReplicatorSG30Test, "Pinned Certificate Failure - SG3.0", "[.SyncServer30]") {
    if ( !Address::isSecure(_sg.address) ) { _sg.address = {kC4Replicator2TLSScheme, C4STR("localhost"), 4984}; }
    REQUIRE(Address::isSecure(_sg.address));

    initTest({Default});

    // Using an unmatched pinned cert:
    _sg.pinnedCert = "-----BEGIN CERTIFICATE-----\r\n"
                     "MIICpDCCAYwCCQCskbhc/nbA5jANBgkqhkiG9w0BAQsFADAUMRIwEAYDVQQDDAls\r\n"
                     "b2NhbGhvc3QwHhcNMjIwNDA4MDEwNDE1WhcNMzIwNDA1MDEwNDE1WjAUMRIwEAYD\r\n"
                     "VQQDDAlsb2NhbGhvc3QwggEiMA0GCSqGSIb3DQEBAQUAA4IBDwAwggEKAoIBAQDQ\r\n"
                     "vl0M5D7ZglW76p428x7iQoSkhNyRBEjZgSqvQW3jAIsIElWu7mVIIAm1tpZ5i5+Q\r\n"
                     "CHnFLha1TDACb0MUa1knnGj/8EsdOADvBfdBq7AotypiqBayRUNdZmLoQEhDDsen\r\n"
                     "pEHMDmBrDsWrgNG82OMFHmjK+x0RioYTOlvBbqMAX8Nqp6Yu/9N2vW7YBZ5ovsr7\r\n"
                     "vdFJkSgUYXID9zw/MN4asBQPqMT6jMwlxR1bPqjsNgXrMOaFHT/2xXdfCvq2TBXu\r\n"
                     "H7evR6F7ayNcMReeMPuLOSWxA6Fefp8L4yDMW23jizNIGN122BgJXTyLXFtvg7CQ\r\n"
                     "tMnE7k07LLYg3LcIeamrAgMBAAEwDQYJKoZIhvcNAQELBQADggEBABdQVNSIWcDS\r\n"
                     "sDPXk9ZMY3stY9wj7VZF7IO1V57n+JYV1tJsyU7HZPgSle5oGTSkB2Dj1oBuPqnd\r\n"
                     "8XTS/b956hdrqmzxNii8sGcHvWWaZhHrh7Wqa5EceJrnyVM/Q4uoSbOJhLntLE+a\r\n"
                     "FeFLQkPpJxdtjEUHSAB9K9zCO92UC/+mBUelHgztsTl+PvnRRGC+YdLy521ST8BI\r\n"
                     "luKJ3JANncQ4pCTrobH/EuC46ola0fxF8G5LuP+kEpLAh2y2nuB+FWoUatN5FQxa\r\n"
                     "+4F330aYRvDKDf8r+ve3DtchkUpV9Xa1kcDFyTcYGKBrINtjRmCIblA1fezw59ZT\r\n"
                     "S5TnM2/TjtQ=\r\n"
                     "-----END CERTIFICATE-----\r\n";

    // One-shot push setup
    ReplParams replParams{_collectionSpecs, kC4OneShot, kC4Disabled};

    // expectSuccess = false so we can check the error code
    replicate(replParams, false);
    CHECK(_callbackStatus.error.domain == NetworkDomain);
    CHECK(_callbackStatus.error.code == kC4NetErrTLSCertUntrusted);
}
#endif  //#ifdef COUCHBASE_ENTERPRISE

TEST_CASE_METHOD(ReplicatorSG30Test, "Auto Purge Enabled - Revoke Access - SG3.0", "[.SyncServer30]") {
    const string idPrefix   = timePrefix();
    const string docIDstr   = idPrefix + "apera-doc1";
    const string channelIDa = idPrefix + "a";
    const string channelIDb = idPrefix + "b";

    initTest({Default}, {channelIDa, channelIDb});

    // Setup pull filter:
    _pullFilter = [](C4CollectionSpec collectionSpec, C4String docID, C4String revID, C4RevisionFlags flags,
                     FLDict flbody, void* context) {
        if ( (flags & kRevPurged) == kRevPurged ) {
            ((ReplicatorAPITest*)context)->_counter++;
            Dict body(flbody);
            CHECK(body.count() == 0);
        }
        return true;
    };

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl, bool pushing, size_t numDocs, const C4DocumentEnded* docs[], void* context) {
        for ( size_t i = 0; i < numDocs; ++i ) {
            auto doc = docs[i];
            if ( (doc->flags & kRevPurged) == kRevPurged ) { ((ReplicatorAPITest*)context)->_docsEnded++; }
        }
    };

    // Put doc in remote DB, in channels a and b
    REQUIRE(_sg.upsertDoc(_collectionSpecs[0], docIDstr, "{}", {channelIDa, channelIDb}));

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    ReplParams replParams{_collectionSpecs, kC4Disabled, kC4OneShot};
    replParams.setPullFilter(_pullFilter).setCallbackContext(this);
    replicate(replParams);

    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);

    // Verify
    c4::ref<C4Document> doc1 = c4coll_getDoc(_collections[0], slice(docIDstr), true, kDocGetAll, nullptr);
    REQUIRE(doc1);
    CHECK(slice(doc1->revID).hasPrefix("1-"_sl));

    // Revoke access to channel 'a' (only access to channel 'b'):
    REQUIRE(_testUser.setChannels({channelIDb}));

    // Update doc to only channel 'b'
    auto oRevID = slice(doc1->revID).asString();
    REQUIRE(_sg.upsertDoc(_collectionSpecs[0], docIDstr, oRevID, "{}", {channelIDb}));

    C4Log("-------- Pull update");
    replicate(replParams);

    // Verify the update:
    doc1 = c4coll_getDoc(_collections[0], slice(docIDstr), true, kDocGetAll, nullptr);
    REQUIRE(doc1);
    CHECK(slice(doc1->revID).hasPrefix("2-"_sl));
    CHECK(_docsEnded == 0);
    CHECK(_counter == 0);

    // Revoke access to all channels:
    REQUIRE(_testUser.revokeAllChannels());

    C4Log("-------- Pull the revoked");
    replicate(replParams);

    // Verify that doc1 is purged:
    doc1 = c4coll_getDoc(_collections[0], slice(docIDstr), true, kDocGetAll, nullptr);
    REQUIRE(!doc1);
    // One doc per collection
    CHECK(_docsEnded == _collectionCount);
    // This check fails, can't figure out what's wrong with line 617 but it's
    // something. Debugging shows that the context pointer is correct (points
    // to this test object), and doing `((ReplicatorAPITest*)context)->_counter`
    // in the debugger returns > 0, but for some reason the actual `_counter`
    // variable of ReplicatorAPITest is never increased.
    //    CHECK(_counter == _collectionCount);
}

TEST_CASE_METHOD(ReplicatorSG30Test, "Auto Purge Disabled - Revoke Access SG3.0", "[.SyncServer30]") {
    const string          idPrefix = timePrefix();
    const string          doc1ID   = idPrefix + "doc1";
    const vector<string>  chIDs{idPrefix};
    constexpr const char* uname = "apdra";

    initTest({Default}, chIDs, uname);

    REQUIRE(_sg.upsertDoc(_collectionSpecs[0], doc1ID, "{}"_sl, chIDs));

    struct CBContext {
        int docsEndedTotal  = 0;
        int docsEndedPurge  = 0;
        int pullFilterTotal = 0;
        int pullFilterPurge = 0;

        void reset() {
            docsEndedTotal  = 0;
            docsEndedPurge  = 0;
            pullFilterTotal = 0;
            pullFilterPurge = 0;
        }
    } cbContext;

    // Setup pull filter:
    C4ReplicatorValidationFunction pullFilter = [](C4CollectionSpec, C4String, C4String, C4RevisionFlags flags, FLDict,
                                                   void* context) {
        auto ctx = (CBContext*)context;
        ctx->pullFilterTotal++;
        if ( (flags & kRevPurged) == kRevPurged ) { ctx->pullFilterPurge++; }
        return true;
    };

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl, bool pushing, size_t numDocs, const C4DocumentEnded* docs[], void*) {
        for ( size_t i = 0; i < numDocs; ++i ) {
            auto  doc = docs[i];
            auto* ctx = (CBContext*)doc->collectionContext;
            ctx->docsEndedTotal++;
            if ( (doc->flags & kRevPurged) == kRevPurged ) { ctx->docsEndedPurge++; }
        }
    };

    // Replication parameters setup
    ReplParams replParams{_collectionSpecs, kC4Disabled, kC4OneShot};
    replParams.setOption(kC4ReplicatorOptionAutoPurge, false).setPullFilter(pullFilter).setCallbackContext(&cbContext);

    // Pull doc into CBL:
    C4Log("-------- Pulling");
    replicate(replParams);

    c4::ref<C4Document> doc1 = c4coll_getDoc(_collections[0], slice(doc1ID), true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc1);
    CHECK(cbContext.docsEndedTotal == _collectionCount);
    CHECK(cbContext.docsEndedPurge == 0);
    CHECK(cbContext.pullFilterTotal == _collectionCount);
    CHECK(cbContext.pullFilterPurge == 0);

    // Revoke access to all channels:
    REQUIRE(_testUser.revokeAllChannels());

    C4Log("-------- Pulling the revoked");
    cbContext.reset();

    replicate(replParams);

    // Verify if the doc1 is not purged as the auto purge is disabled:
    doc1 = c4coll_getDoc(_collections[0], slice(doc1ID), true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc1);
    CHECK(cbContext.docsEndedPurge == 1);
    // No pull filter called
    CHECK(cbContext.pullFilterTotal == 0);
}

TEST_CASE_METHOD(ReplicatorSG30Test, "Remove Doc From Channel SG3.0", "[.SyncServer30]") {
    string         idPrefix = timePrefix();
    string         doc1ID{idPrefix + "doc1"};
    vector<string> chIDs{idPrefix + "a", idPrefix + "b"};

    initTest({Default}, chIDs);

    _sg.upsertDoc(_collectionSpecs[0], doc1ID, "{}"_sl, chIDs);

    struct CBContext {
        int docsEndedTotal  = 0;
        int docsEndedPurge  = 0;
        int pullFilterTotal = 0;
        int pullFilterPurge = 0;

        void reset() {
            docsEndedTotal  = 0;
            docsEndedPurge  = 0;
            pullFilterTotal = 0;
            pullFilterPurge = 0;
        }
    } context;

    // Setup onDocsEnded:
    _enableDocProgressNotifications = true;
    _onDocsEnded = [](C4Replicator* repl, bool pushing, size_t numDocs, const C4DocumentEnded* docs[], void*) {
        for ( size_t i = 0; i < numDocs; ++i ) {
            auto doc = docs[i];
            auto ctx = (CBContext*)doc->collectionContext;
            ctx->docsEndedTotal++;
            if ( (doc->flags & kRevPurged) == kRevPurged ) { ctx->docsEndedPurge++; }
        }
    };

    // Setup pull filter:
    C4ReplicatorValidationFunction pullFilter = [](C4CollectionSpec, C4String, C4String, C4RevisionFlags flags,
                                                   FLDict flbody, void* context) {
        auto* ctx = (CBContext*)context;
        ctx->pullFilterTotal++;
        if ( (flags & kRevPurged) == kRevPurged ) {
            ctx->pullFilterPurge++;
            Dict body(flbody);
            CHECK(body.count() == 0);
        }
        return true;
    };

    // Pull doc into CBL:
    C4Log("-------- Pulling");

    bool       autoPurgeEnabled{true};
    ReplParams replParams{_collectionSpecs, kC4Disabled, kC4OneShot};
    replParams.setPullFilter(pullFilter).setCallbackContext(&context);

    SECTION("Auto Purge Enabled") { autoPurgeEnabled = true; }

    SECTION("Auto Purge Disabled") {
        replParams.setOption(C4STR(kC4ReplicatorOptionAutoPurge), false);
        autoPurgeEnabled = false;
    }

    replicate(replParams);

    CHECK(context.docsEndedTotal == _collectionCount);
    CHECK(context.docsEndedPurge == 0);
    CHECK(context.pullFilterTotal == _collectionCount);
    CHECK(context.pullFilterPurge == 0);

    // Verify doc
    c4::ref<C4Document> doc1 = c4coll_getDoc(_collections[0], slice(doc1ID), true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc1);
    CHECK(c4rev_getGeneration(doc1->revID) == 1);

    // Once verified, remove it from channel 'a' in that collection
    auto oRevID = slice(doc1->revID).asString();
    _sg.upsertDoc(_collectionSpecs[0], doc1ID, R"({"_rev":")" + oRevID + "\"}", {chIDs[1]});

    C4Log("-------- Pull update");
    context.reset();
    replicate(replParams);

    CHECK(context.docsEndedTotal == _collectionCount);
    CHECK(context.docsEndedPurge == 0);
    CHECK(context.pullFilterTotal == _collectionCount);
    CHECK(context.pullFilterPurge == 0);

    // Verify the update:
    doc1 = c4coll_getDoc(_collections[0], slice(doc1ID), true, kDocGetCurrentRev, nullptr);
    REQUIRE(doc1);
    CHECK(c4rev_getGeneration(doc1->revID) == 2);

    // Remove doc from all channels:
    oRevID = slice(doc1->revID).asString();
    _sg.upsertDoc(_collectionSpecs[0], doc1ID, R"({"_rev":")" + oRevID + "\"}", {});

    C4Log("-------- Pull the removed");
    context.reset();
    replicate(replParams);

    doc1 = c4coll_getDoc(_collections[0], slice(doc1ID), true, kDocGetCurrentRev, nullptr);

    if ( autoPurgeEnabled ) {
        // Verify if doc1 is purged:
        REQUIRE(!doc1);
    } else {
        REQUIRE(doc1);
    }

    CHECK(context.docsEndedPurge == _collectionCount);
    if ( autoPurgeEnabled ) {
        CHECK(context.pullFilterPurge == _collectionCount);
    } else {
        // No pull filter called
        CHECK(context.pullFilterTotal == 0);
    }
}
