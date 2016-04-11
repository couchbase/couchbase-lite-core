//
//  View.swift
//  SwiftForest
//
//  Created by Jens Alfke on 11/5/15.
//  Copyright © 2015 Couchbase. All rights reserved.
//

import Foundation


public typealias EmitFunction = (AnyObject, AnyObject?) throws -> ()
public typealias MapFunction = (NSData, EmitFunction) throws -> ()


public class View {

    public init(database: Database, path: String, name: String, create: Bool, map: MapFunction, version: String) throws {
        let flags: C4DatabaseFlags = create ? C4DatabaseFlags.DB_Create : C4DatabaseFlags()
        var err = C4Error()
        self.database = database
        self.map = map
        self.handle = c4view_open(database.dbHandle, C4Slice(path), C4Slice(name), C4Slice(version), flags, nil, &err)
        guard handle != nil else {
            throw err
        }
    }

    deinit {
        c4view_close(handle, nil)
    }

    public let database: Database

    public func eraseIndex() throws {
        var err = C4Error()
        guard c4view_eraseIndex(handle, &err) else {
            throw err
        }
    }

    public func deleteView() throws {
        var err = C4Error()
        guard c4view_delete(handle, &err) else {
            throw err
        }
        handle = nil
    }

    public var totalRows: UInt64                {return c4view_getTotalRows(handle)}
    public var lastSequenceIndexed: Sequence    {return c4view_getLastSequenceIndexed(handle)}
    public var lastSequenceChangedAt: Sequence  {return c4view_getLastSequenceChangedAt(handle)}


    public var map: MapFunction


    var handle: COpaquePointer
}


public class ViewIndexer {

    public init(views: [View]) throws {
        self.views = views
        var c4views = views.map { $0.handle }
        var err = C4Error()
        indexer = c4indexer_begin(views[0].database.dbHandle, &c4views, c4views.count, &err)
        guard indexer != nil else {
            throw err
        }
    }

    public func run() throws {
        // Define the emit function that will be passed to the map function:
        var keys = [Key]()
        var values = [NSData?]()
        func emit(keyObj: AnyObject, valueObj: AnyObject?) throws {
            let key = try Key(obj: keyObj)
            var value: NSData?
            if valueObj != nil {
                value = try NSJSONSerialization.dataWithJSONObject(valueObj!, options: [])
                //FIX: Allow fragments
            }
            keys.append(key)
            values.append(value)
        }

        // Enumerate the new revisions:
        var err = C4Error()
        let e = try enumerator()
        while let doc = e.next() {
            guard let body = try doc.selectedRevBody() else {
                continue
            }
            var viewNumber: UInt32 = 0
            for view in views {
                keys.removeAll(keepCapacity: true)
                values.removeAll(keepCapacity: true)

                // Call the map function!
                do {
                    try view.map(body, emit)
                } catch let x {
                    print("WARNING: View map function failed with error \(x)")
                }

                // Send the emitted keys/values to the indexer:
                if keys.count > 0 {
                    guard c4indexer_emit(indexer, doc.doc, viewNumber,
                                         UInt32(keys.count),
                                         keys.map {$0.handle},
                                         values.map {C4Slice($0)},
                                         &err)
                    else {
                        throw err
                    }
                }
                viewNumber += 1
            }
        }
    }

    private func enumerator() throws -> DocEnumerator {
        var err = C4Error()
        let result = c4indexer_enumerateDocuments(indexer, &err)
        guard result != nil else {
            throw err
        }
        return DocEnumerator(c4enum: result)
    }

    private let views: [View]
    private let indexer: COpaquePointer
}
