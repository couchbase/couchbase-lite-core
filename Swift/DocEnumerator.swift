//
//  DocEnumerator.swift
//  SwiftForest
//
//  Created by Jens Alfke on 11/5/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

import Foundation


public class DocEnumerator: GeneratorType {

    public typealias Element = Document

    init(c4enum: COpaquePointer) {
        assert(c4enum != nil)
        e = c4enum
    }

    deinit {
        c4enum_free(e)
    }

    public func next() -> Document? {
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

public struct EnumeratorOptions {
    public var skip: UInt64
    public var flags: EnumeratorFlags

    var asC4Options: C4EnumeratorOptions {
        return C4EnumeratorOptions(skip: skip, flags: C4EnumeratorFlags(rawValue: flags.rawValue))
    }

    public static let defaultOptions = EnumeratorOptions(skip: 0,
        flags: [.InclusiveStart, .InclusiveEnd, .IncludeBodies])
}
