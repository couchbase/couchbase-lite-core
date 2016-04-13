//
//  DocEnumerator.swift
//  SwiftForest
//
//  Created by Jens Alfke on 11/5/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

import Foundation


/** Represents a range of docIDs in a Database, plus other query options.
    Acts as a Swift Sequence that generates a DocEnumerator for the actual enumeration.
    As a Sequence, this type can be directly used in a "for...in..." loop. */
public struct DocumentRange: LazySequenceType {

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
        return try withC4Opts() { (c4opts, inout err: C4Error) in
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
    private func withC4Opts(@noescape block: (UnsafePointer<C4EnumeratorOptions>, inout C4Error)->COpaquePointer) throws -> DocEnumerator {
        var result: COpaquePointer = nil
        var err = C4Error()
        var c4opt = C4EnumeratorOptions(skip: skip, flags: C4EnumeratorFlags(rawValue: flags.rawValue))
        withUnsafePointer(&c4opt) { c4optPtr in
            result = block(c4optPtr, &err)
        }
        guard result != nil else {
            throw err
        }
        return DocEnumerator(c4enum: result, limit: limit)
    }

    // SEQUENCETYPE API:

    public typealias Generator = DocEnumerator
    public typealias Subsequence = DocumentRange

    public func dropFirst(n: Int) -> DocumentRange {
        var r = self
        r.skip = UInt64(Int64(r.skip) + Int64(n))
        return r
    }

    func prefix(maxLength: Int) -> DocumentRange {
        var r = self
        r.limit = min(r.limit, UInt64(maxLength))
        return r
    }

    public func generate() -> DocEnumerator {
        return try! enumerateDocs()     //FIX: Can't throw error
    }
}


public class DocEnumerator: GeneratorType, SequenceType {

    public typealias Element = Document

    init(c4enum: COpaquePointer, limit: UInt64 = UInt64.max) {
        assert(c4enum != nil)
        e = c4enum
        self.limit = limit
    }

    deinit {
        c4enum_free(e)
    }

    public func next() -> Document? {
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
    public func nextDoc() throws -> Document? {
        var err = C4Error()
        let c4doc = c4enum_nextDocument(e, &err)
        guard c4doc != nil else {
            if err.code != 0 {
                self.error = err
                throw err
            }
            return nil
        }
        return Document(handle: c4doc)
    }

    /** Check this after next() returns nil, to see if it stopped due to an error. */
    public var error: C4Error?

    /** Throws if next() stopped due to an error. */
    public func checkError() throws {
        if error != nil {
            throw error!
        }
    }

    private var e: COpaquePointer
    private var limit: UInt64
}


public struct EnumeratorFlags : OptionSetType {
    public let rawValue: UInt16
    public init(rawValue: UInt16) { self.rawValue = rawValue }
    init(_ flag: C4EnumeratorFlags) { self.rawValue = flag.rawValue }

    public static let Descending        = EnumeratorFlags(C4EnumeratorFlags.Descending)
    public static let InclusiveStart    = EnumeratorFlags(C4EnumeratorFlags.InclusiveStart)
    public static let InclusiveEnd      = EnumeratorFlags(C4EnumeratorFlags.InclusiveEnd)
    public static let IncludeDeleted    = EnumeratorFlags(C4EnumeratorFlags.IncludeDeleted)
    public static let IncludeNonConflicted = EnumeratorFlags(C4EnumeratorFlags.IncludeNonConflicted)
    public static let IncludeBodies     = EnumeratorFlags(C4EnumeratorFlags.IncludeBodies)
}
