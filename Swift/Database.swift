//
//  Database.swift
//  SwiftForest
//
//  Created by Jens Alfke on 6/10/15.
//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
//

import Foundation


/** A Couchbase Lite Core database. (This is lower-level than a Couchbase Lite database.) */
open class Database {

    public init(path: String, create: Bool = true) throws {
        var config = C4DatabaseConfig()
        config.flags = create ? C4DatabaseFlags.db_Create : C4DatabaseFlags()
        var err = C4Error()
        dbHandle = c4db_open(C4Slice(path), &config, &err)
        guard dbHandle != nil else {
            throw err
        }
        self.path = path
    }

    deinit {
        c4db_close(dbHandle, nil)
    }

    /** Closes the database and deletes its file(s). You may NOT use this object thereafter. */
    open func delete() throws {
        var err = C4Error()
        guard c4db_delete(dbHandle, &err) else {
            throw err
        }
        dbHandle = nil
    }

    /** Deletes the database file at the given path, without opening it. */
    open static func delete(_ path: String) throws {
        var config = C4DatabaseConfig()
        var err = C4Error()
        guard c4db_deleteAtPath(C4Slice(path), &config, &err) else {
            throw err
        }
    }

    open func compact() throws {
        var err = C4Error()
        guard c4db_compact(dbHandle, &err) else {
            throw err
        }
    }

    open let path: String

    open var documentCount: UInt64 {
        return c4db_getDocumentCount(dbHandle)
    }

    open var lastSequence: Sequence {
        return c4db_getLastSequence(dbHandle)
    }

    open func inTransaction(_ block: ()throws->()) throws {
        var err = C4Error()
        guard c4db_beginTransaction(dbHandle, &err) else {
            throw err
        }

        do {
            try block()
        } catch let x {
            c4db_endTransaction(dbHandle, false, nil)
            throw x
        }

        guard c4db_endTransaction(dbHandle, true, &err) else {
            throw err
        }
    }


    // MARK:-  RAW DOCS:

    open func rawDocs(_ store: String) -> RawDocs {
        return RawDocs(db: self, store: store)
    }


    // MARK:- VERSIONED DOCS:

    fileprivate func getDoc(_ docID: String, mustExist: Bool = true) throws -> Document? {
        var err = C4Error()
        let doc = c4doc_get(dbHandle, C4Slice(docID), mustExist, &err)
        guard doc != nil else {
            if err.domain == .LiteCoreDomain && err.code == Int32(kC4ErrorNotFound) {
                return nil
            }
            throw err
        }
        return Document(handle: doc!)
    }

    /** Returns the Document with the given ID, or nil if none exists. */
    open func getExistingDoc(_ docID: String) throws -> Document? {
        return try getDoc(docID, mustExist: true)
    }

    /** Returns the Document with the given ID,
        or a new empty/unsaved Document if none exists yet. */
    open func getDoc(_ docID: String) throws -> Document {
        return try getDoc(docID, mustExist: false)!
    }

    /** Returns the Document with the given sequence, or nil if none exists. */
    func getDoc(_ sequence: Sequence) throws -> Document? {
        var err = C4Error()
        let doc = c4doc_getBySequence(dbHandle, sequence, &err)
        guard doc != nil else {
            throw err
        }
        return doc != nil ? Document(handle: doc!) : nil
    }

    open func getRev(_ docID: String, revID: String? = nil, withBody: Bool = true) throws -> Revision? {
        guard let doc = try getExistingDoc(docID) else {
            return nil
        }
        if revID != nil {
            guard try doc.selectRevision(revID!) else {
                return nil
            }
        }
        let body: Data? = withBody ? try doc.selectedRevBody() : nil
        return Revision(database: self, doc: doc, rawBody: body as Data?)
    }

    open func getRev(_ sequence: Sequence, withBody: Bool = true) throws -> Revision? {
        guard let doc = try getDoc(sequence) else {
            return nil
        }
        let body: Data? = withBody ? try doc.selectedRevBody() : nil
        return Revision(database: self, doc: doc, rawBody: body as Data?)
    }

    open func newRev(_ docID: String?, rawBody: Data, hasAttachments: Bool = false, allowConflict: Bool = false) throws -> Revision {
        let doc = try putDoc(docID, parentRev: nil, body: rawBody, hasAttachments: hasAttachments, allowConflict: allowConflict)
        return Revision(database: self, doc: doc, rawBody: rawBody as Data?)
    }

    open func newRev(_ docID: String?, body: [String:Val], hasAttachments: Bool = false, allowConflict: Bool = false) throws -> Revision {
        let rawBody: Data = Val.dict(body).asJSON()
        return try newRev(docID, rawBody: rawBody, hasAttachments: hasAttachments, allowConflict: allowConflict)
    }

    func putDoc(_ docID: String?, parentRev: String?, body: Data,
                deletion: Bool = false, hasAttachments: Bool = false,
                allowConflict: Bool = false) throws -> Document {
        var history = C4Slice(parentRev)
        return try withUnsafePointer(to: &history) { historyPtr in
            var rq = C4DocPutRequest()
            rq.docID = C4Slice(docID)
            rq.body = C4Slice(body)
            rq.deletion = deletion
            rq.hasAttachments = hasAttachments
            rq.allowConflict = allowConflict
            rq.history = historyPtr
            rq.historyCount = (parentRev != nil ? 1 : 0)
            rq.save = true

            var err = C4Error()
            let d = c4doc_put(dbHandle, &rq, nil, &err)
            guard d != nil else {
                throw err
            }
            return Document(handle: d!)
        }
    }

    open func documents(start: String? = nil, end: String? = nil, skip: UInt64 = 0, limit: UInt64 = UInt64.max, descending: Bool = false) -> DocumentRange {
        var r = DocumentRange(database: self)
        r.start = start
        r.end = end
        r.skip = skip
        r.limit = limit
        if descending {
            r.flags.formUnion([.Descending])
        }
        return r
    }

    open func documents(_ docIDs: [String]) -> DocumentRange {
        return DocumentRange(database: self, docIDs: docIDs)
    }

//    public func enumerateChanges(since: Sequence, options: EnumeratorOptions? = nil) throws -> DocEnumerator
//    {
//        return try mkEnumerator(options) { (c4opts, inout err: C4Error) in
//            c4db_enumerateChanges(dbHandle, since, c4opts, &err)
//        }
//    }


    var dbHandle: OpaquePointer?
}


extension Database : CustomStringConvertible {
    public var description: String {
        return "Database{\(path)}"
    }
}




/** Represents a single raw-document store in a Database. */
public struct RawDocs {

    let db: Database
    let store: String

    /** Get (metadata, body) */
    public func get(_ key: String) throws -> (Data?, Data?) {
        var err = C4Error()
        let raw = c4raw_get(db.dbHandle, C4Slice(store), C4Slice(key), &err)
        guard raw != nil else {
            if err.code == Int32(kC4ErrorNotFound) && err.domain == .LiteCoreDomain {
                return (nil, nil)
            }
            throw err
        }
        let result = (raw?.pointee.meta.toData(), raw?.pointee.body.toData())
        c4raw_free(raw)
        return result
    }

    public func get(_ key: String) throws -> Data? {
        let (_, body) = try get(key)
        return body
    }

    public func set(_ key: String, meta: Data? = nil, body: Data?) throws {
        var err = C4Error()
        guard c4raw_put(db.dbHandle, C4Slice(store), C4Slice(key), C4Slice(meta), C4Slice(body), &err) else {
            throw err
        }
    }
}
