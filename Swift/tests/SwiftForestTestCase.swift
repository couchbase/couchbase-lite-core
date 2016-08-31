//
//  SwiftForestTestCase.swift
//  Couchbase Lite Core
//
//  Created by Jens Alfke on 4/13/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

import XCTest
@testable import LiteCoreSwift


class SwiftForestTestCase: XCTestCase {

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


    func mkdata(_ str: String) -> Data {
        return str.data(using: String.Encoding.utf8)!
    }
    
    func createNumberedDocs(_ range: CountableClosedRange<Int>) {
        try! db.inTransaction {
            for i in range {
                let docID = String(format: "doc-%03d", i)
                try db.getDoc(docID).put(["i": Val.int(i)])
            }
        }
    }

}

