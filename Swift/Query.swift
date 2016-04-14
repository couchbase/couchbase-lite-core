//
//  Query.swift
//  SwiftForest
//
//  Created by Jens Alfke on 4/8/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

import Foundation


public class Query: LazySequenceType {

    public typealias Generator = QueryEnumerator

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
        var enumHandle: UnsafeMutablePointer<C4QueryEnumerator> = nil

        if encodedKeys == nil {
            options.keys = nil
            enumHandle = c4view_query(view.handle, &options, &err)
        } else {
            var keyHandles: [COpaquePointer] = encodedKeys!.map {$0.handle}
            withUnsafeMutablePointer(&keyHandles[0]) { keysPtr in
                options.keys = keysPtr
                enumHandle = c4view_query(view.handle, &options, &err)
            }
        }
        guard enumHandle != nil else { throw err }
        return QueryEnumerator(enumHandle)
    }

    public func generate() -> QueryEnumerator {
        return try! run()
    }

    private var options = kC4DefaultQueryOptions

    // These properties hold references to the Key objects whose handles are stored in `options`
    private var encodedStartKey: Key?
    private var encodedEndKey: Key?
    private var encodedKeys: [Key]?
}


public class QueryEnumerator: GeneratorType {

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
        return QueryRow(handle.memory)
    }

    private let handle: UnsafeMutablePointer<C4QueryEnumerator>

}


public class QueryRow {

    init(_ e: C4QueryEnumerator) {
        key = Key.readFrom(e.key)!
        valueJSON = e.value.asData()
        docID = e.docID.asString()
    }

    public let docID: String?

    public let key: AnyObject

    public lazy var value: AnyObject? = {
        guard self.valueJSON != nil else {return nil}
        let value = try! NSJSONSerialization.JSONObjectWithData(self.valueJSON!, options: [.AllowFragments])
        self.valueJSON = nil
        return value
    }()

    private var valueJSON: NSData?
}
