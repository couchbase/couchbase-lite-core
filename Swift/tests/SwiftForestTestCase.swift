//
//  SwiftForestTestCase.swift
//  CBForest
//
//  Created by Jens Alfke on 4/13/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

import XCTest
@testable import SwiftForest


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


    func mkdata(str: String) -> NSData {
        return str.dataUsingEncoding(NSUTF8StringEncoding)!
    }
    
    func createNumberedDocs(range: Range<Int>) {
        try! db.inTransaction {
            for i in range {
                let docID = String(format: "doc-%03d", i)
                try! db.newRev(docID, body: ["i": i])
            }
        }
    }

}

