//
//  DocEnumerator.swift
//  SwiftForest
//
//  Created by Jens Alfke on 11/5/15.
//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
//

import Foundation


/** Represents a range of docIDs in a Database, plus other query options.
    Acts as a Swift Sequence that generates a DocEnumerator for the actual enumeration.
    As a Sequence, this type can be directly used in a "for...in..." loop. */
public struct DocumentRange: LazySequenceProtocol {

    public init(database: Database) {
        self.database = database
    }

    public init(database: Database, docIDs: [String]) {
        self.database = database
        self.docIDs = docIDs
    }

    public let database: Database

    public var start: String? = nil
    public var end: String? = nil
    public var docIDs: [String]? = nil
    public var skip: UInt64 = 0
    public var limit: UInt64 = UInt64.max
    public var flags: EnumeratorFlags = [.InclusiveStart, .InclusiveEnd, .IncludeNonConflicted, .IncludeBodies]

    public func enumerateDocs() throws -> DocEnumerator {
        return try withC4Opts() { (c4opts, err: inout C4Error) in
            if docIDs == nil {
                return c4db_enumerateAllDocs(database.dbHandle, C4Slice(start), C4Slice(end), c4opts, &err)
            } else {
                var docIDSlices = docIDs!.map {C4Slice($0)}
                return c4db_enumerateSomeDocs(database.dbHandle, &docIDSlices, docIDSlices.count, c4opts, &err)
            }
        }
    }

    // common subroutine for creating enumerators; provides C4EnumeratorOptions, checks errors,
    // and wraps enumerator in a DocEnumerator
    fileprivate func withC4Opts(_ block: (UnsafePointer<C4EnumeratorOptions>, inout C4Error)->OpaquePointer) throws -> DocEnumerator {
        var result: OpaquePointer? = nil
        var err = C4Error()
        var c4opt = C4EnumeratorOptions(skip: skip, flags: C4EnumeratorFlags(rawValue: flags.rawValue))
        withUnsafePointer(to: &c4opt) { c4optPtr in
            result = block(c4optPtr, &err)
        }
        guard result != nil else {
            throw err
        }
        return DocEnumerator(database: database, c4enum: result, limit: limit)
    }

    // SEQUENCETYPE API:

    public typealias Iterator = DocEnumerator
    public typealias Subsequence = DocumentRange

    public func dropFirst(_ n: Int) -> DocumentRange {
        var r = self
        r.skip = UInt64(Int64(r.skip) + Int64(n))
        return r
    }

    func prefix(_ maxLength: Int) -> DocumentRange {
        var r = self
        r.limit = Swift.min(r.limit, UInt64(maxLength))
        return r
    }

    public func makeIterator() -> DocEnumerator {
        return try! enumerateDocs()     //FIX: Can't throw error
    }
}


open class DocEnumerator: IteratorProtocol, Swift.Sequence {

    public typealias Element = Document

    init(database: Database, c4enum: OpaquePointer?, limit: UInt64 = UInt64.max) {
        assert(c4enum != nil)
        self.database = database
        e = c4enum!
        self.limit = limit
    }

    deinit {
        c4enum_free(e)
    }

    open func next() -> Document? {
        guard limit > 0 else {
            return nil
        }
        limit -= 1

        do {
            return try nextDoc()
        } catch {
            return nil
        }
    }

    /** The same as next() except that it can throw an error. */
    open func nextDoc() throws -> Document? {
        var err = C4Error()
        let c4doc = c4enum_nextDocument(e, &err)
        guard c4doc != nil else {
            if err.code != 0 {
                self.error = err
                throw err
            }
            return nil
        }
        return Document(database: database, handle: c4doc!)
    }

    /** Check this after next() returns nil, to see if it stopped due to an error. */
    open var error: C4Error?

    /** Throws if next() stopped due to an error. */
    open func checkError() throws {
        if error != nil {
            throw error!
        }
    }

    fileprivate let database: Database
    fileprivate var e: OpaquePointer
    fileprivate var limit: UInt64
}


public struct EnumeratorFlags : OptionSet {
    public let rawValue: UInt16
    public init(rawValue: UInt16) { self.rawValue = rawValue }
    init(_ flag: C4EnumeratorFlags) { self.rawValue = flag.rawValue }

    public static let Descending        = EnumeratorFlags(C4EnumeratorFlags.descending)
    public static let InclusiveStart    = EnumeratorFlags(C4EnumeratorFlags.inclusiveStart)
    public static let InclusiveEnd      = EnumeratorFlags(C4EnumeratorFlags.inclusiveEnd)
    public static let IncludeDeleted    = EnumeratorFlags(C4EnumeratorFlags.includeDeleted)
    public static let IncludeNonConflicted = EnumeratorFlags(C4EnumeratorFlags.includeNonConflicted)
    public static let IncludeBodies     = EnumeratorFlags(C4EnumeratorFlags.includeBodies)
}
