//
//  Database.swift
//  SwiftForest
//
//  Created by Jens Alfke on 6/10/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

import Foundation


/** A CBForest database. (This is lower-level than a Couchbase Lite database.) */
public class Database {

    public init(path: String, create: Bool = true) throws {
        let flags: C4DatabaseFlags = create ? C4DatabaseFlags.DB_Create : C4DatabaseFlags()
        var err = C4Error()
        dbHandle = c4db_open(C4Slice(path), flags, nil, &err)
        guard dbHandle != nil else {
            throw err
        }
        self.path = path
    }

    deinit {
        c4db_close(dbHandle, nil)
    }

    /** Closes the database and deletes its file(s). You may NOT use this object thereafter. */
    public func delete() throws {
        var err = C4Error()
        guard c4db_delete(dbHandle, &err) else {
            throw err
        }
        dbHandle = nil
    }

    /** Deletes the database file at the given path, without opening it. */
    public static func delete(path: String) throws {
        var err = C4Error()
        guard c4db_deleteAtPath(C4Slice(path), C4DatabaseFlags(), &err) else {
            throw err
        }
    }

    public func compact() throws {
        var err = C4Error()
        guard c4db_compact(dbHandle, &err) else {
            throw err
        }
    }

    public let path: String

    public var documentCount: UInt64 {
        return c4db_getDocumentCount(dbHandle)
    }

    public var lastSequence: Sequence {
        return c4db_getLastSequence(dbHandle)
    }

    public func inTransaction(@noescape block: ()throws->()) throws {
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

    public func rawDocs(store: String) -> RawDocs {
        return RawDocs(db: self, store: store)
    }


    // MARK:- VERSIONED DOCS:

    private func getDoc(docID: String, mustExist: Bool = true) throws -> Document? {
        var err = C4Error()
        let doc = c4doc_get(dbHandle, C4Slice(docID), mustExist, &err)
        guard doc != nil else {
            if err.domain == .ForestDBDomain && err.code == -9 {
                return nil
            }
            throw err
        }
        return Document(handle: doc)
    }

    /** Returns the Document with the given ID, or nil if none exists. */
    public func getExistingDoc(docID: String) throws -> Document? {
        return try getDoc(docID, mustExist: true)
    }

    /** Returns the Document with the given ID,
        or a new empty/unsaved Document if none exists yet. */
    public func getDoc(docID: String) throws -> Document {
        return try getDoc(docID, mustExist: false)!
    }

    /** Returns the Document with the given sequence, or nil if none exists. */
    func getDoc(sequence: Sequence) throws -> Document? {
        var err = C4Error()
        let doc = c4doc_getBySequence(dbHandle, sequence, &err)
        guard doc != nil else {
            throw err
        }
        return doc != nil ? Document(handle: doc) : nil
    }

    public func getRev(docID: String, revID: String? = nil, withBody: Bool = true) throws -> Revision? {
        guard let doc = try getExistingDoc(docID) else {
            return nil
        }
        if revID != nil {
            guard try doc.selectRevision(revID!) else {
                return nil
            }
        }
        let body: NSData? = withBody ? try doc.selectedRevBody() : nil
        return Revision(database: self, doc: doc, rawBody: body)
    }

    public func getRev(sequence: Sequence, withBody: Bool = true) throws -> Revision? {
        guard let doc = try getDoc(sequence) else {
            return nil
        }
        let body: NSData? = withBody ? try doc.selectedRevBody() : nil
        return Revision(database: self, doc: doc, rawBody: body)
    }

    public func newRev(docID: String?, rawBody: NSData, hasAttachments: Bool = false, allowConflict: Bool = false) throws -> Revision {
        let doc = try putDoc(docID, parentRev: nil, body: rawBody, hasAttachments: hasAttachments, allowConflict: allowConflict)
        return Revision(database: self, doc: doc, rawBody: rawBody)
    }

    public func newRev(docID: String?, body: JSONDict, hasAttachments: Bool = false, allowConflict: Bool = false) throws -> Revision {
        let rawBody = try NSJSONSerialization.dataWithJSONObject(body, options: [])
        return try newRev(docID, rawBody: rawBody, hasAttachments: hasAttachments, allowConflict: allowConflict)
    }

    func putDoc(docID: String?, parentRev: String?, body: NSData,
                deletion: Bool = false, hasAttachments: Bool = false,
                allowConflict: Bool = false) throws -> Document {
        var history = C4Slice(parentRev)
        return try withUnsafePointer(&history) { historyPtr in
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
            return Document(handle: d)
        }
    }

    public func documents(start start: String? = nil, end: String? = nil, skip: UInt64 = 0, limit: UInt64 = UInt64.max, descending: Bool = false) -> DocumentRange {
        var r = DocumentRange(database: self)
        r.start = start
        r.end = end
        r.skip = skip
        r.limit = limit
        if descending {
            r.flags.unionInPlace([.Descending])
        }
        return r
    }

    public func documents(docIDs: [String]) -> DocumentRange {
        return DocumentRange(database: self, docIDs: docIDs)
    }

//    public func enumerateChanges(since: Sequence, options: EnumeratorOptions? = nil) throws -> DocEnumerator
//    {
//        return try mkEnumerator(options) { (c4opts, inout err: C4Error) in
//            c4db_enumerateChanges(dbHandle, since, c4opts, &err)
//        }
//    }


    var dbHandle: COpaquePointer
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
    public func get(key: String) throws -> (NSData?, NSData?) {
        var err = C4Error()
        let raw = c4raw_get(db.dbHandle, C4Slice(store), C4Slice(key), &err)
        guard raw != nil else {
            if err.code == -9 && err.domain == .ForestDBDomain {
                return (nil, nil)
            }
            throw err
        }
        let result = (raw.memory.meta.toData(), raw.memory.body.toData())
        c4raw_free(raw)
        return result
    }

    public func get(key: String) throws -> NSData? {
        let (_, body) = try get(key)
        return body
    }

    public func set(key: String, meta: NSData? = nil, body: NSData?) throws {
        var err = C4Error()
        guard c4raw_put(db.dbHandle, C4Slice(store), C4Slice(key), C4Slice(meta), C4Slice(body), &err) else {
            throw err
        }
    }
}
