//
//  SwiftForestTests.swift
//  SwiftForestTests
//
//  Created by Jens Alfke on 4/11/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

@testable import SwiftForest


class DatabaseTests: SwiftForestTestCase {

    func test01_RawDataMissing() throws {
        let testStore = db.rawDocs("test")
        let (meta, data) = try! testStore.get("nope")
        check(meta).isNil()
        check(data).isNil()
    }

    func test02_RawData() throws {
        let testStore = db.rawDocs("test")
        let payload = mkdata("Hi there")
        try testStore.set("k", body: payload)

        let (meta, data) = try testStore.get("k")

        check(meta).isNil()
        check(data) == payload

        // Single-return-value get:
        check(try testStore.get("k")) == payload
}

    func test03_RawDataWithMeta() throws {
        let testStore = db.rawDocs("test")
        let payloadMeta = mkdata("wow")
        let payload = mkdata("Hi there")
        try testStore.set("k", meta: payloadMeta, body: payload)

        let (meta, data) = try testStore.get("k")

        check(meta) == payloadMeta
        check(data) == payload
    }

    func test04_GetMissingDoc() throws {
        check(try db.getExistingDoc("mydoc")).isNil()
        let noDoc = try db.getDoc("mydoc")
        check(noDoc.exists).isFalse()
        check(noDoc.docID) == "mydoc"
        check(noDoc.revID) == ""
    }

    func test05_CreateDoc() throws {
        check(db.documentCount) == 0
        check(db.lastSequence) == 0

        var seq: Sequence = 0
        try db.inTransaction {
            let doc = try db.putDoc("mydoc", parentRev: nil, body: mkdata("{\"msg\":\"hello\"}"))
            print ("Doc exists=\(doc.exists). docID=\(doc.docID), revID=\(doc.revID).")
            check(doc.exists).isTrue()
            check(doc.docID) == "mydoc"
            check(doc.revID).notNil()
            seq = doc.sequence
            check(seq) == 1
        }

        check(db.documentCount) == 1
        check(db.lastSequence) == 1

        var rev = try db.getRev("mydoc")
        print("rev = \(rev)")
        check(rev).notNil()

        rev = try db.getRev(seq)
        print("rev = \(rev)")
        check(rev).notNil()
        check(rev?.docID) == "mydoc"
    }

    func test06_CreateMultipleRevs() throws {
        try db.inTransaction {
            let body1: JSONDict = ["Key": "Value"]
            var rev = try db.newRev("multi", body: body1)
            check(rev.docID) == "multi"
            check(rev.revID) == "1-29ad6c711c8c520131753825727241c7cc680ac3"
            check(rev.properties as NSDictionary) == body1 as NSDictionary
            check(rev.property("Key")) == "Value"
            let x: Int? = rev.property("X")
            check(x).isNil()
            check(rev.property("X", default: -17)) == -17

            let body2: JSONDict = ["Key": 1337, "something": "else"]
            rev = try rev.update(body2)
            check(rev.revID) == "2-a1efd3b85c63542e1cde47cb7251295f4f126725"
            check(rev.properties as NSDictionary) == body2 as NSDictionary
            check(rev.property("Key")) == 1337
            check(rev.property("Key", default: -1)) == 1337
        }
    }

    func test07_AllDocs() throws {
        createNumberedDocs(1...100)
        var i = 0
        for doc in db.documents() {
            i += 1
            check(doc.docID) == String(format: "doc-%03d", i)
        }
        check(i) == 100
    }

    func test07_RangeOfDocs() throws {
        createNumberedDocs(1...100)
        var i = 9
        for doc in db.documents(start: "doc-010", end: "doc-019") {
            i += 1
            check(doc.docID) == String(format: "doc-%03d", i)
        }
        check(i) == 19
    }

    func test08_SkipAndLimit() throws {
        createNumberedDocs(1...100)
        var i = 14
        for doc in db.documents(start: "doc-010", skip: 5, limit: 8) {
            i += 1
            check(doc.docID) == String(format: "doc-%03d", i)
        }
        check(i) == 22
    }

    func test09_Descending() throws {
        createNumberedDocs(1...100)
        var i = 11
        for doc in db.documents(start: "doc-010", descending: true) {
            i -= 1
            check(doc.docID) == String(format: "doc-%03d", i)
        }
        check(i) == 1
    }

    func test09_SpecificDocs() throws {
        createNumberedDocs(1...100)
        let docIDs = ["doc-073", "doc-001", "doc-100", "doc-055"]
        var i = 0
        for doc in db.documents(docIDs) {
            check(doc.docID) == docIDs[i]
            i += 1
        }
        check(i) == docIDs.count
    }

}
