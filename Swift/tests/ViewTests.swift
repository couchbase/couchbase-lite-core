//
//  ViewTests.swift
//  CBForest
//
//  Created by Jens Alfke on 4/13/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

import Foundation
@testable import SwiftForest


class ViewTests: SwiftForestTestCase {

    var view: View!

    static let kPath = "/tmp/swiftforest.viewdb"

    override func setUp() {
        super.setUp()

        try! View.delete(ViewTests.kPath)
        let map: MapFunction = {doc, emit in
            print("map(\(doc))")
            emit(doc["i"] as! Int, nil)
        }
        view = try! View(database: db, path: ViewTests.kPath, name: "test", create: true, map: map, version: "1")
    }

    override func tearDown() {
        view = nil
        super.tearDown()
    }


    func test01_Index() throws {
        createNumberedDocs(1...100)
        try ViewIndexer(views:[view]).run()
        check(view.totalRows) == 100
    }

    func test02_BasicQuery() throws {
        createNumberedDocs(1...100)
        try ViewIndexer(views:[view]).run()

        var i = 0
        for row in Query(view) {
            i += 1
            //print("doc = \(row.docID)   key = \(row.key)   value = \(row.value)")
            check(row.key as? Int) == i
            check(row.value).isNil()
            check(row.docID) == String(format: "doc-%03d", i)
        }
        check(i) == 100
    }
}
