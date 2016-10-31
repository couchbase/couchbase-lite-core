//
//  Database.swift
//  SwiftForest
//
//  Created by Jens Alfke on 6/10/15.
//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

import Foundation

/********
 NOTE: THIS IS A PROVISIONAL, PLACEHOLDER API, NOT THE OFFICIAL COUCHBASE LITE 2.0 API.
 It's for prototyping, experimentation, and performance testing. It will change without notice.
 Once the 2.0 API is designed, we will begin implementing that and remove these classes.
 ********/



/** A Couchbase Lite Core database. (This is lower-level than a Couchbase Lite database.) */
public final class Database {

    public init(path: String,
                create: Bool = true,
                readOnly: Bool = false,
                bundled: Bool = false) throws
    {
        var config = C4DatabaseConfig(create: create, readOnly: readOnly, bundled: bundled)
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
    public func delete() throws {
        var err = C4Error()
        guard c4db_delete(dbHandle, &err) else {
            throw err
        }
        dbHandle = nil
    }

    /** Deletes the database file at the given path, without opening it. */
    public static func delete(_ path: String) throws {
        var config = C4DatabaseConfig()
        var err = C4Error()
        guard c4db_deleteAtPath(C4Slice(path), &config, &err) else {
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

    public func inTransaction(_ block: ()throws->()) throws {
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

    public func rawDocs(_ store: String) -> RawDocs {
        return RawDocs(db: self, store: store)
    }


    // MARK:- VERSIONED DOCS:

    fileprivate func getDoc(_ docID: String, mustExist: Bool = true) throws -> Document? {
        var err = C4Error()
        guard let doc = c4doc_get(dbHandle, C4Slice(docID), mustExist, &err) else {
            if err.isNotFound {
                return nil
            }
            throw err
        }
        return Document(database: self, handle: doc)
    }

    /** Returns the Document with the given ID, or nil if none exists. */
    public func getExistingDoc(_ docID: String) throws -> Document? {
        return try getDoc(docID, mustExist: true)
    }

    /** Returns the Document with the given ID,
        or a new empty/unsaved Document if none exists yet. */
    public func getDoc(_ docID: String) throws -> Document {
        return try getDoc(docID, mustExist: false)!
    }

    /** Returns the Document with the given sequence, or nil if none exists. */
    func getDoc(_ sequence: Sequence) throws -> Document? {
        var err = C4Error()
        guard let doc = c4doc_getBySequence(dbHandle, sequence, &err) else {
            if err.isNotFound {
                return nil
            }
            throw err
        }
        return Document(database: self, handle: doc)
    }

    public func documents(start: String? = nil, end: String? = nil, skip: UInt64 = 0, limit: UInt64 = UInt64.max, descending: Bool = false) -> DocumentRange {
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

    public func documents(_ docIDs: [String]) -> DocumentRange {
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
            if err.isNotFound {
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


extension C4DatabaseConfig {
    init(create: Bool = true,
         readOnly: Bool = false,
         bundled: Bool = false)
    {
        self.init()
        if create {
            flags.insert(.db_Create)
        }
        if readOnly {
            flags.insert(.db_ReadOnly)
        }
        if bundled {
            flags.insert(.db_Bundled)
        }

    }
}
