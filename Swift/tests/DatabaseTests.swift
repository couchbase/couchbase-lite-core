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
        XCTAssertNil(meta)
        XCTAssertNil(data)
    }

    func test02_RawData() {
        let testStore = db.rawDocs("test")
        let payload = mkdata("Hi there")
        try! testStore.set("k", body: payload)

        let (meta, data) = try! testStore.get("k")

        XCTAssertNil(meta)
        XCTAssertEqual(data, payload)

        // Single-return-value get:
        XCTAssertEqual(try! testStore.get("k"), payload)
}

    func test03_RawDataWithMeta() {
        let testStore = db.rawDocs("test")
        let payloadMeta = mkdata("wow")
        let payload = mkdata("Hi there")
        try! testStore.set("k", meta: payloadMeta, body: payload)

        let (meta, data) = try! testStore.get("k")

        XCTAssertEqual(meta, payloadMeta)
        XCTAssertEqual(data, payload)
    }

    func test04_GetMissingDoc() {
        XCTAssertNil(try! db.getExistingDoc("mydoc"))
        let noDoc = try! db.getDoc("mydoc")
        XCTAssertNotNil(noDoc)
        XCTAssertFalse(noDoc.exists)
        XCTAssertEqual(noDoc.docID, "mydoc")
        XCTAssertEqual(noDoc.revID, "")
    }

    func test05_CreateDoc() {
        XCTAssertEqual(db.documentCount, 0)
        XCTAssertEqual(db.lastSequence, 0)

        try! db.inTransaction {
            let doc = try db.putDoc("mydoc", parentRev: nil, body: mkdata("{\"msg\":\"hello\"}"))
            print ("Doc exists=\(doc.exists). docID=\(doc.docID), revID=\(doc.revID).")
            XCTAssertTrue(doc.exists)
            XCTAssertEqual(doc.docID, "mydoc")
            XCTAssertNotNil(doc.revID)
        }

        XCTAssertEqual(db.documentCount, 1)
        XCTAssertEqual(db.lastSequence, 1)

        try! db.inTransaction {
            let rev = try! db.getRev("mydoc")
            print("rev = \(rev)")
            XCTAssertNotNil(rev)
        }
    }

}
