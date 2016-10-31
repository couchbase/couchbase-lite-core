//
//  Key.swift
//  SwiftForest
//
//  Created by Jens Alfke on 4/10/16.
//  Copyright (c) 2016 Couchbase. All rights reserved.
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


/** Error code for C4Error */
let InvalidKey: Int32 = 2000


/** Internal class representing a view index key in Collatable format */
class Key {

    init(handle: OpaquePointer) {
        self.handle = handle
    }

    init(_ val: Val) {
        handle = c4key_new()
        add(val)
    }

    init(_ obj: Any?) throws {
        if obj != nil {
            handle = c4key_new()
            do {
                try add(obj!)
            } catch {
                c4key_free(handle)
                throw error
            }
        } else {
            handle = nil
        }
    }

    deinit {
        c4key_free(handle)
    }

    // Convert a Val to a Key
    fileprivate func add(_ val: Val) {
        switch val {
        case .null:
            c4key_addNull(handle)
        case .bool(let b):
            c4key_addBool(handle, b)
        case .int(let i):
            c4key_addNumber(handle, Double(i))
        case .float(let f):
            c4key_addNumber(handle, f)
        case .string(let s):
            c4key_addString(handle, C4Slice(s))
        case .data(let d):
            c4key_addString(handle, C4Slice(d))
        case .array(let a):
            c4key_beginArray(handle)
            for item in a {
                add(item)
            }
            c4key_endArray(handle)
        case .dict(let d):
            c4key_beginMap(handle)
            for (k, v) in d {
                c4key_addMapKey(handle, C4Slice(k))
                add(v)
            }
            c4key_endMap(handle)
        }
    }

    // Try toonvert anything to a Key; works for Val and for JSON-compatible objects
    fileprivate func add(_ obj: Any) throws {
        switch obj {
        case is NSNull:
            c4key_addNull(handle)

        case is NSNumber:
            // Don't use `case let n as NSNumber`, or Swift will happily coerce native numbers
            let n = obj as! NSNumber
            if n.isBool {
                c4key_addBool(handle, n.boolValue)
            } else {
                c4key_addNumber(handle, n.doubleValue)
            }

        case let b as Bool:
            c4key_addBool(handle, b)

        case let i as Int:
            c4key_addNumber(handle, Double(i))

        case let d as Double:
            c4key_addNumber(handle, d)

        case let s as String:
            c4key_addString(handle, C4Slice(s))

        case let a as [Any]:
            c4key_beginArray(handle)
            for item in a {
                try add(item)
            }
            c4key_endArray(handle)

        case let a as NSArray:
            c4key_beginArray(handle)
            for item in a {
                try add(item)
            }
            c4key_endArray(handle)

        case let d as [String:Any]:
            c4key_beginMap(handle)
            for (k, v) in d {
                c4key_addMapKey(handle, C4Slice(k))
                try add(v)
            }
            c4key_endMap(handle)

        case let d as NSDictionary:
            c4key_beginMap(handle)
            for (k, v) in d {
                guard let keyStr = k as? NSString else {
                    throw C4Error(domain: .LiteCoreDomain, code: InvalidKey)
                }
                c4key_addMapKey(handle, C4Slice(keyStr))
                try add(v)
            }
            c4key_endMap(handle)

        case let v as Val:
            add(v);
            
        default:
            throw C4Error(domain: .LiteCoreDomain, code: Int32(InvalidKey))
        }
    }

    /** The internal C4Key* handle */
    let handle: OpaquePointer?


    var JSON: String {
        var r = reader()
        return c4key_toJSON(&r).asString()!
    }

    func reader() -> C4KeyReader {  // only needed for tests
        return c4key_read(handle)
    }


    /** Reads key data from a KeyReader and parses it into Swift objects. */
    class func readFrom(_ reader: C4KeyReader) -> Val? {
        guard reader.bytes != nil else { return nil }
        var r = reader
        return readFrom(&r)
    }

    fileprivate class func readFrom(_ reader: inout C4KeyReader) -> Val {
        switch c4key_peek(&reader) {
        case .null:     return Val.null
        case .bool:     return Val.bool(c4key_readBool(&reader))
        case .number:
            let n: Double = c4key_readNumber(&reader)
            let i = Int(n)
            if Double(i) == n {
                return Val.int(i)
            } else {
                return Val.float(n)
            }
        case .string:   return Val.string(c4key_readString(&reader).asString()!)
        case .array:
            var result = [Val]()
            c4key_skipToken(&reader)
            while c4key_peek(&reader) != .endSequence {
                result.append(readFrom(&reader))
            }
            return Val.array(result)
        case .map:
            var result = [String:Val]()
            c4key_skipToken(&reader)
            while c4key_peek(&reader) != .endSequence {
                let key = c4key_readString(&reader).asString()!
                result[key] = readFrom(&reader)
            }
            return Val.dict(result)
        default:
            abort()
        }
    }
}



// MARK: - NSNumber: Comparable

private let trueNumber = NSNumber(value: true)
private let falseNumber = NSNumber(value: false)
private let trueObjCType = String(cString: trueNumber.objCType)
private let falseObjCType = String(cString: falseNumber.objCType)

extension NSNumber {
    var isBool:Bool {
        // An NSNumber is a boolean if its type-encoding is "B", 
        // _or_ if it's "c" and it's equal to one of the canonical true or false NSNumber values.
        switch self.objCType[0] {
        case 66:   // 'B'
            return true
        case 99: // 'c'
            return self.compare(trueNumber) == ComparisonResult.orderedSame
                || self.compare(falseNumber) == ComparisonResult.orderedSame
        default:
            return false
        }
    }
}
