//
//  KeyTests.swift
//  CBForest
//
//  Created by Jens Alfke on 4/13/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

import XCTest
@testable import SwiftForest


class KeyTests : XCTestCase {

    func testScalars() throws {
        try check(Key(NSNull()).JSON) == "null"
        try check(Key(false).JSON) == "false"
        try check(Key(true).JSON)  == "true"
        try check(Key(0).JSON)  == "0"
        try check(Key(0.00).JSON)  == "0"
        try check(Key(1).JSON)  == "1"
        try check(Key(-1).JSON)  == "-1"
        try check(Key(3.125).JSON)  == "3.125"
        try check(Key("").JSON)  == "\"\""
        try check(Key("hi").JSON)  == "\"hi\""
    }

    func testCompound() throws {
        try check(Key([]).JSON)  == "[]"
        try check(Key([99]).JSON)  == "[99]"
        try check(Key([false, 0, true, 17.17, "x", []]).JSON)  == "[false,0,true,17.17,\"x\",[]]"
        try check(Key([:]).JSON)  == "{}"
        try check(Key(["key":true]).JSON)  == "{\"key\":true}"
        try check(Key(["key":true, "":"x", "y":123.456]).JSON)  == "{\"key\":true,\"\":\"x\",\"y\":123.456}"
    }
}