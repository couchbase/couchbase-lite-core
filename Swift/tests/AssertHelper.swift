//
//  AssertHelper.swift
//  CBForest
//
//  Created by Jens Alfke on 4/12/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

import XCTest


// Syntactic sugar for XCTAssert functions:
public struct check<T> {
    public init(_ a: T, file: StaticString = #file, line: UInt = #line) {
        actual = a
        self.file = file
        self.line = line
    }

    public init(_ a: T?, file: StaticString = #file, line: UInt = #line) {
        self.actual = a
        self.file = file
        self.line = line
    }

    let actual: T?
    let file: StaticString
    let line: UInt

    public func isNil()     {XCTAssertNil(actual, file: file, line: line)}
    public func notNil()    {XCTAssertNotNil(actual, file: file, line: line)}

    public func isFalse()   {XCTAssertFalse(actual! as! Bool, file: file, line: line)}
    public func isTrue()    {XCTAssertTrue(actual! as! Bool, file: file, line: line)}
}

public func == <T : Equatable> (c: check<T>, expected: T) {
    XCTAssertEqual(c.actual, expected, file: c.file, line: c.line)
}

public func != <T : Equatable> (c: check<T>, expected: T) {
    XCTAssertNotEqual(c.actual, expected, file: c.file, line: c.line)
}

