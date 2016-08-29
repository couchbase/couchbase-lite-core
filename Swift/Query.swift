//
//  Query.swift
//  SwiftForest
//
//  Created by Jens Alfke on 4/8/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
//

import Foundation


open class Query: LazySequenceProtocol {

    public typealias Iterator = QueryEnumerator

    init(_ view: View) {
        self.view = view
    }

    open let view: View

    open var skip: UInt64 {
        get {return options.skip}
        set {options.skip = newValue}
    }

    open var limit: UInt64 {
        get {return options.limit}
        set {options.limit = newValue}
    }

    open var descending: Bool {
        get {return options.descending}
        set {options.descending = newValue}
    }

    open var inclusiveStart: Bool {
        get {return options.inclusiveStart}
        set {options.inclusiveStart = newValue}
    }

    open var inclusiveEnd: Bool {
        get {return options.inclusiveEnd}
        set {options.inclusiveEnd = newValue}
    }

    open var startKey: AnyObject? {
        didSet {
            encodedStartKey = try! Key(startKey)
            options.startKey = encodedStartKey!.handle
        }
    }

    open var endKey: AnyObject? {
        didSet {
            encodedEndKey = try! Key(endKey)
            options.endKey = encodedEndKey!.handle
        }
    }

    open var startKeyDocID: String?
    open var endKeyDocID: String?

    open var keys: [AnyObject]? {
        didSet {
            encodedKeys = keys?.map {try! Key($0)}
            options.keysCount = keys?.count ?? 0
        }
    }

    open func run() throws -> QueryEnumerator {
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

    open func makeIterator() -> QueryEnumerator {
        return try! run()
    }

    fileprivate var options = kC4DefaultQueryOptions

    // These properties hold references to the Key objects whose handles are stored in `options`
    fileprivate var encodedStartKey: Key?
    fileprivate var encodedEndKey: Key?
    fileprivate var encodedKeys: [Key]?
}


open class QueryEnumerator: IteratorProtocol {

    public typealias Element = QueryRow

    init(_ handle: UnsafeMutablePointer<C4QueryEnumerator>) {
        self.handle = handle
    }

    deinit {
        c4queryenum_free(handle)
    }

    open func next() -> QueryRow? {
        return try! nextRow()
    }

    open func nextRow() throws -> QueryRow? {
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


open class QueryRow {

    init(_ e: C4QueryEnumerator) {
        key = Key.readFrom(e.key)!
        valueJSON = e.value.asData()
        docID = e.docID.asString()
    }

    open let docID: String?

    open let key: Val

    open lazy var value: Val? = {
        guard let json = self.valueJSON else {return nil}
        let value = try! Val(json: json)
        self.valueJSON = nil
        return value
    }()

    fileprivate var valueJSON: Data?
}
