//
//  Query.swift
//  SwiftForest
//
//  Created by Jens Alfke on 4/8/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

/********
 NOTE: THIS IS A PROVISIONAL, PLACEHOLDER API, NOT THE OFFICIAL COUCHBASE LITE 2.0 API.
 It's for prototyping, experimentation, and performance testing. It will change without notice.
 Once the 2.0 API is designed, we will begin implementing that and remove these classes.
 ********/


import Foundation


public final class Query: LazySequenceProtocol {

    public typealias Iterator = QueryEnumerator

    init(_ view: View) {
        self.view = view
    }

    public let view: View

    public var skip: UInt64 {
        get {return options.skip}
        set {options.skip = newValue}
    }

    public var limit: UInt64 {
        get {return options.limit}
        set {options.limit = newValue}
    }

    public var descending: Bool {
        get {return options.descending}
        set {options.descending = newValue}
    }

    public var inclusiveStart: Bool {
        get {return options.inclusiveStart}
        set {options.inclusiveStart = newValue}
    }

    public var inclusiveEnd: Bool {
        get {return options.inclusiveEnd}
        set {options.inclusiveEnd = newValue}
    }

    public var startKey: AnyObject? {
        didSet {
            encodedStartKey = try! Key(startKey)
            options.startKey = encodedStartKey!.handle
        }
    }

    public var endKey: AnyObject? {
        didSet {
            encodedEndKey = try! Key(endKey)
            options.endKey = encodedEndKey!.handle
        }
    }

    public var startKeyDocID: String?
    public var endKeyDocID: String?

    public var keys: [AnyObject]? {
        didSet {
            encodedKeys = keys?.map {try! Key($0)}
            options.keysCount = keys?.count ?? 0
        }
    }

    public func run() throws -> QueryEnumerator {
        options.startKeyDocID = C4Slice(startKeyDocID)
        options.endKeyDocID = C4Slice(endKeyDocID)

        var err = C4Error()
        var enumHandle: UnsafeMutablePointer<C4QueryEnumerator>? = nil

        if encodedKeys == nil {
            options.keys = nil
            enumHandle = c4view_query(view.handle, &options, &err)
        } else {
            var keyHandles: [OpaquePointer?] = encodedKeys!.map {$0.handle}
            withUnsafeMutablePointer(to: &keyHandles[0]) { keysPtr in
                options.keys = keysPtr
                enumHandle = c4view_query(view.handle, &options, &err)
            }
        }
        guard enumHandle != nil else { throw err }
        return QueryEnumerator(enumHandle!)
    }

    public func makeIterator() -> QueryEnumerator {
        return try! run()
    }

    fileprivate var options = kC4DefaultQueryOptions

    // These properties hold references to the Key objects whose handles are stored in `options`
    fileprivate var encodedStartKey: Key?
    fileprivate var encodedEndKey: Key?
    fileprivate var encodedKeys: [Key]?
}


public final class QueryEnumerator: IteratorProtocol {

    public typealias Element = QueryRow

    init(_ handle: UnsafeMutablePointer<C4QueryEnumerator>) {
        self.handle = handle
    }

    deinit {
        c4queryenum_free(handle)
    }

    public func next() -> QueryRow? {
        return try! nextRow()
    }

    public func nextRow() throws -> QueryRow? {
        var err = C4Error()
        guard c4queryenum_next(handle, &err) else {
            if err.code == 0 {
                return nil
            }
            throw err
        }
        return QueryRow(handle.pointee)
    }

    fileprivate let handle: UnsafeMutablePointer<C4QueryEnumerator>

}


public final class QueryRow {

    init(_ e: C4QueryEnumerator) {
        key = Key.readFrom(e.key)!
        valueJSON = e.value.asData()
        docID = e.docID.asString()
    }

    public let docID: String?

    public let key: Val

    public lazy var value: Val? = {
        guard let json = self.valueJSON else {return nil}
        let value = try! Val(json: json)
        self.valueJSON = nil
        return value
    }()

    fileprivate var valueJSON: Data?
}
