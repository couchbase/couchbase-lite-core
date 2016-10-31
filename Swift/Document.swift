//
//  Document.swift
//  SwiftForest
//
//  Created by Jens Alfke on 11/4/15.
//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
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


public typealias Body = [String:Val]


public protocol PropertyOwner {
    var rawBody: Data? { get }
    var properties: Body { get }
    func property<T>(_ name: String) -> T?
    func property<T>(_ name: String, default: T) -> T
}


extension PropertyOwner {
    func getProperties() throws -> Body {
        if let json = self.rawBody, let d = try Val(json: json).asDict() {
            return d
        }
        return [:]
    }

    public func property<T>(_ name: String) -> T? {
        return properties[name]?.unwrap() as? T
    }

    public func property<T>(_ name: String, default: T) -> T {
        return (properties[name]?.unwrap() as? T) ?? `default`
    }
}


public final class Document : PropertyOwner {

    init(database: Database, handle: UnsafeMutablePointer<C4Document>) {
        self.database = database
        doc = handle
    }

    deinit {
        c4doc_free(doc)
    }

    public let database: Database

    public var deleted: Bool        {return doc.pointee.flags.contains(C4DocumentFlags.deleted)}
    public var conflicted: Bool     {return doc.pointee.flags.contains(C4DocumentFlags.conflicted)}
    public var hasAttachments: Bool {
        return doc.pointee.flags.contains(C4DocumentFlags.hasAttachments)}
    public var exists: Bool         {return doc.pointee.flags.contains(C4DocumentFlags.exists)}

    public var docID: String        {return doc.pointee.docID.asString()!}
    public var revID: String        {return doc.pointee.revID.asString() ?? ""}
    public var sequence: Sequence   {return doc.pointee.sequence}

    public var docType: String? {
        let t = c4doc_getType(doc)
        let result = t.asString()
        c4slice_free(t)
        return result
    }

    public func reload() throws {
        var err = C4Error()
        guard let doc = c4doc_get(database.dbHandle, doc.pointee.docID, true, &err) else {
            throw err
        }
        self.doc = doc
    }


    // MARK:- PROPERTY ACCESS

    func selectedRevBody() throws -> Data? {
        var body = doc.pointee.selectedRev.body
        if body.buf == nil {
            var err = C4Error()
            guard c4doc_loadRevisionBody(doc, &err) else {
                throw err
            }
            body = doc.pointee.selectedRev.body
        }
        return body.asData()
    }

    
    public var rawBody : Data? {
        return (try? selectedRevBody()) ?? nil
    }

    public var properties: Body {
        if let props = _cachedProperties {
            return props
        }
        let props = (try? self.getProperties()) ?? [:]
        _cachedProperties = props
        return props
    }

    fileprivate var _cachedProperties: Body?    // cleared when self.doc changes (q.v.)


    // MARK:- MAKING CHANGES

    // Internal doc-update method. Leaves new revision selected!
    func putDoc(parentRev: String?, body: Data,
                deletion: Bool = false, hasAttachments: Bool = false,
                allowConflict: Bool = false) throws {
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
            guard let d = c4doc_put(database.dbHandle, &rq, nil, &err) else {throw err}
            self.doc = d
        }
    }


    public func put(_ body: Body, deleted: Bool = false, hasAttachments: Bool = false) throws {
        let rawBody: Data = Val.dict(body).asJSON()
        try putDoc(parentRev: revID, body: rawBody, deletion: deleted)
        selectCurrentRevision()
    }


    public func delete() throws {
        try put([:], deleted: true)
    }


    public func update( _ fn: (inout Body)throws->()) throws {
        while (true) {
            var props = self.properties
            try fn(&props)
            do {
                try put(props, deleted: props.isEmpty)
                return
            } catch let err as C4Error {
                if !err.isConflict {
                    throw err
                }
            }
            try reload()
        }
    }


    public func purge() throws {
        var err = C4Error()
        guard c4doc_purgeRevision(doc, C4Slice(), &err) >= 0 else {throw err}
        try save()
    }


    public func purge(revIDs: [String]) throws -> Int {
        var purged = 0
        for revID in revIDs {
            var err = C4Error()
            let purged1 = c4doc_purgeRevision(doc, C4Slice(revID), &err)
            guard purged1 >= 0 else {
                throw err
            }
            purged += Int(purged1)
        }
        if purged >= 0 {
            try save()
        }
        return purged
    }


    func save(_ maxRevTreeDepth: UInt32 = 20) throws {
        var err = C4Error()
        guard c4doc_save(doc, maxRevTreeDepth, &err) else {
            throw err
        }
    }


    var doc: UnsafeMutablePointer<C4Document> {
        didSet {
            _cachedProperties = nil
        }
    }
}


extension Document : CustomStringConvertible {
    public var description: String {
        return "{\"\(docID)\"}"
    }
}
