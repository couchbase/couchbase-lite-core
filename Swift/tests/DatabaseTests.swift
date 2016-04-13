//
//  SwiftForestTests.swift
//  SwiftForestTests
//
//  Created by Jens Alfke on 4/11/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

import XCTest
@testable import SwiftForest


func mkdata(str: String) -> NSData {
    return str.dataUsingEncoding(NSUTF8StringEncoding)!
}


class DatabaseTests: XCTestCase {

    var db: Database! = nil
    
    override func setUp() {
        super.setUp()

        try! Database.delete("/tmp/swiftforest.db")
        db = try! Database(path: "/tmp/swiftforest.db", create: true)
    }

    override func tearDown() {
        db = nil  // closes database
        super.tearDown()
    }

    func test01_RawDataMissing() {
        let testStore = db.rawDocs("test")
        let (meta, data) = try! testStore.get("nope")
        check(meta).isNil()
        check(data).isNil()
    }

    func test02_RawData() {
        let testStore = db.rawDocs("test")
        let payload = mkdata("Hi there")
        try! testStore.set("k", body: payload)

        let (meta, data) = try! testStore.get("k")

        check(meta).isNil()
        check(data) == payload

        // Single-return-value get:
        check(try! testStore.get("k")) == payload
}

    func test03_RawDataWithMeta() {
        let testStore = db.rawDocs("test")
        let payloadMeta = mkdata("wow")
        let payload = mkdata("Hi there")
        try! testStore.set("k", meta: payloadMeta, body: payload)

        let (meta, data) = try! testStore.get("k")

        check(meta) == payloadMeta
        check(data) == payload
    }

    func test04_GetMissingDoc() {
        check(try! db.getExistingDoc("mydoc")).isNil()
        let noDoc = try! db.getDoc("mydoc")
        check(noDoc.exists).isFalse()
        check(noDoc.docID) == "mydoc"
        check(noDoc.revID) == ""
    }

    func test05_CreateDoc() {
        check(db.documentCount) == 0
        check(db.lastSequence) == 0

        var seq: Sequence = 0
        try! db.inTransaction {
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

        var rev = try! db.getRev("mydoc")
        print("rev = \(rev)")
        check(rev).notNil()

        rev = try! db.getRev(seq)
        print("rev = \(rev)")
        check(rev).notNil()
        check(rev?.docID) == "mydoc"
    }

    func test06_CreateMultipleRevs() {
        try! db.inTransaction {
            let body1: JSONDict = ["Key": "Value"]
            var rev = try db.newRev("multi", body: body1)
            check(rev.docID) == "multi"
            check(rev.revID) == "1-29ad6c711c8c520131753825727241c7cc680ac3"
            check(rev.properties as NSDictionary) == body1 as NSDictionary
            check(rev.property("Key")) == "Value"
            XCTAssertNil(rev.property("X"))

            let body2: JSONDict = ["Key": 1337, "something": "else"]
            rev = try rev.update(body2)
            check(rev.revID) == "2-a1efd3b85c63542e1cde47cb7251295f4f126725"
            check(rev.properties as NSDictionary) == body2 as NSDictionary
            check(rev.property("Key")) == 1337
        }
    }

    func setupAllDocs() {
        try! db.inTransaction {
            for i in 1...100 {
                let docID = String(format: "doc-%03d", i)
                try! db.newRev(docID, body: ["i": i])
            }
        }
    }

    func test07_AllDocs() {
        setupAllDocs()
        var i = 0
        for doc in db.documents() {
            i += 1
            check(doc.docID) == String(format: "doc-%03d", i)
        }
        check(i) == 100
    }

    func test07_RangeOfDocs() {
        setupAllDocs()
        var i = 9
        for doc in db.documents(start: "doc-010", end: "doc-019") {
            i += 1
            check(doc.docID) == String(format: "doc-%03d", i)
        }
        check(i) == 19
    }

    func test08_SkipAndLimit() {
        setupAllDocs()
        var i = 14
        for doc in db.documents(start: "doc-010", skip: 5, limit: 8) {
            i += 1
            check(doc.docID) == String(format: "doc-%03d", i)
        }
        check(i) == 22
    }

    func test09_Descending() {
        setupAllDocs()
        var i = 11
        for doc in db.documents(start: "doc-010", descending: true) {
            i -= 1
            check(doc.docID) == String(format: "doc-%03d", i)
        }
        check(i) == 1
    }

    func test09_SpecificDocs() {
        setupAllDocs()
        let docIDs = ["doc-073", "doc-001", "doc-100", "doc-055"]
        var i = 0
        for doc in db.documents(docIDs) {
            check(doc.docID) == docIDs[i]
            i += 1
        }
        check(i) == docIDs.count
    }

}
