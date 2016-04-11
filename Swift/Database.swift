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

    public func newDoc(docID: String?, rawBody: NSData, hasAttachments: Bool = false, allowConflict: Bool = false) throws -> Revision {
        let doc = try putDoc(docID, parentRev: nil, body: rawBody, hasAttachments: hasAttachments, allowConflict: allowConflict)
        return Revision(database: self, doc: doc, rawBody: rawBody)
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


    // MARK:- ENUMERATION:

    // common subroutine for creating enumerators; provides C4EnumeratorOptions and checks errors
    private func mkEnumerator(options: EnumeratorOptions?, @noescape block: (UnsafePointer<C4EnumeratorOptions>, inout C4Error)->COpaquePointer) throws -> DocEnumerator {
        var result: COpaquePointer = nil
        var err = C4Error()
        if options != nil {
            var c4opt = options!.asC4Options
            withUnsafePointer(&c4opt) { c4optPtr in
                result = block(c4optPtr, &err)
            }
        } else {
            result = block(nil, &err)
        }
        guard result != nil else {
            throw err
        }
        return DocEnumerator(c4enum: result)
    }

    public func enumerateDocs(startDocID: String? = nil, endDocID: String? = nil, options: EnumeratorOptions? = nil) throws -> DocEnumerator
    {
        return try mkEnumerator(options) { (c4opts, inout err: C4Error) in
            c4db_enumerateAllDocs(dbHandle, C4Slice(startDocID), C4Slice(endDocID), c4opts, &err)
        }
    }
    
    public func enumerateDocs(docIDs: [String], options: EnumeratorOptions? = nil) throws -> DocEnumerator
    {
        var docIDSlices = docIDs.map {C4Slice($0)}

        return try mkEnumerator(options) { (c4opts, inout err: C4Error) in
            c4db_enumerateSomeDocs(dbHandle, &docIDSlices, docIDSlices.count, c4opts, &err)
        }
    }
    
    public func enumerateChanges(since: Sequence, options: EnumeratorOptions? = nil) throws -> DocEnumerator
    {
        return try mkEnumerator(options) { (c4opts, inout err: C4Error) in
            c4db_enumerateChanges(dbHandle, since, c4opts, &err)
        }
    }
    

    var dbHandle: COpaquePointer
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
