//
//  View.swift
//  SwiftForest
//
//  Created by Jens Alfke on 11/5/15.
//  Copyright (c) 2015-2016 Couchbase. All rights reserved.
//

import Foundation


public typealias EmitFunction = (Val, Val?) -> ()
public typealias MapFunction = (Body, EmitFunction) throws -> ()


open class View {

    public init(database: Database, path: String, name: String, create: Bool, map: MapFunction, version: String) throws {
        var config = C4DatabaseConfig()
        config.flags = create ? C4DatabaseFlags.db_Create : C4DatabaseFlags()
        var err = C4Error()
        self.database = database
        self.name = name
        self.map = map
        self.handle = c4view_open(database.dbHandle,
                                  C4Slice(path),
                                  C4Slice(name),
                                  C4Slice(version),
                                  &config,
                                  &err)
        guard handle != nil else {
            throw err
        }
    }

    deinit {
        print("DEINIT view")//TEMP
        guard c4view_close(handle, nil) else {
            print("WARNING: \(self) is busy, couldn't be closed")
            return
        }
    }

    open let database: Database
    open let name: String

    open func eraseIndex() throws {
        var err = C4Error()
        guard c4view_eraseIndex(handle, &err) else {
            throw err
        }
    }

    open func delete() throws {
        var err = C4Error()
        guard c4view_delete(handle, &err) else {
            throw err
        }
        handle = nil
    }

    open class func delete(_ path: String) throws {
        var config = C4DatabaseConfig()
        var err = C4Error()
        guard c4view_deleteAtPath(C4Slice(path), &config, &err) else {
            throw err
        }
    }

    open var totalRows: UInt64                {return c4view_getTotalRows(handle)}
    open var lastSequenceIndexed: Sequence    {return c4view_getLastSequenceIndexed(handle)}
    open var lastSequenceChangedAt: Sequence  {return c4view_getLastSequenceChangedAt(handle)}


    open var map: MapFunction


    var handle: OpaquePointer?
}


extension View : CustomStringConvertible {
    public var description: String {
        return "View{\(name)}"
    }
}




open class ViewIndexer {

    public init(views: [View]) throws {
        self.views = views
        var c4views = views.map { $0.handle }
        var err = C4Error()
        indexer = c4indexer_begin(views[0].database.dbHandle, &c4views, c4views.count, &err)
        guard indexer != nil else {
            throw err
        }
    }

    deinit {
        if indexer != nil {
            c4indexer_end(indexer, false, nil)  // run must have failed, or was never called
        }
    }

    open func run() throws {
        // Define the emit function that will be passed to the map function:
        var keys = [Key]()
        var values = [Data?]()
        func emit(_ keyObj: Val, valueObj: Val?) {
            print("    emit(\(keyObj), \(valueObj))")
            keys.append(Key(keyObj))
            values.append(valueObj?.asJSON())
        }

        // Enumerate the new revisions:
        var err = C4Error()
        let e = try enumerator()
        while let doc = e.next() {
            guard let body = try doc.selectedRevBody() else {
                continue
            }
            let props = try Val(json: body).asDict()!

            var viewNumber: UInt32 = 0
            for view in views {
                keys.removeAll(keepingCapacity: true)
                values.removeAll(keepingCapacity: true)

                // Call the map function!
                do {
                    try view.map(props, emit)
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

        let ok = c4indexer_end(indexer, true, &err)
        indexer = nil
        guard ok else { throw err }
    }

    fileprivate func enumerator() throws -> DocEnumerator {
        var err = C4Error()
        let result = c4indexer_enumerateDocuments(indexer, &err)
        guard result != nil else {
            throw err
        }
        return DocEnumerator(c4enum: result)
    }

    fileprivate let views: [View]
    fileprivate var indexer: OpaquePointer?
}
