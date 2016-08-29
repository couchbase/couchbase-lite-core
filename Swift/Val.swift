//
//  Value.swift
//  LiteCore
//
//  Created by Jens Alfke on 8/29/16.
//  Copyright © 2016 Couchbase. All rights reserved.
//

import Foundation


/** A JSON-compatible value. Used as the basic storage type of documents and views. */
public enum Val : Equatable, ExpressibleByIntegerLiteral, ExpressibleByArrayLiteral, ExpressibleByDictionaryLiteral, CustomDebugStringConvertible {

    case null
    case bool   (Bool)
    case int    (Int)
    case float  (Double)
    case string (String)
    case data   (Data)
    case array  ([Val])
    case dict   ([String:Val])


//    public typealias StringLiteralType = String
//    public typealias UnicodeScalarLiteralType = Character
//    public init(stringLiteral value: String) {
//        self = Val.string(value)
//    }
//    public init(extendedGraphemeClusterLiteral value: StaticString) {
//        self = Val.string(value.description)
//    }

    public init(integerLiteral i: Int) {
        self = Val.int(i)
    }
    
    public init(arrayLiteral elements: Val...) {
        self = Val.array(elements)
    }

    public typealias Key = String
    public typealias Value = Val

    public init(dictionaryLiteral elements: (String, Val)...) {
        var dict = [String:Val](minimumCapacity: elements.count)
        for e in elements {
            dict[e.0] = e.1
        }
        self = Val.dict(dict)
    }


    public init(json: Data) throws {
        let j = try JSONSerialization.jsonObject(with: json, options: [])
        try self = Val.withJSONObject(j)
    }


    public init(json: String) throws {
        try self.init(json: json.data(using: String.Encoding.utf8)!)
    }


    public static func withJSONObject(_ obj: Any) throws -> Val {
        switch obj {
        case is NSNull:
            return Val.null

        case is NSNumber:
            // Don't use `let n as NSNumber`, or Swift will happily coerce native numbers.
            // Also, this case has to come before the native Swift ones (Bool, Int, etc)
            // otherwise those cases will mis-trigger, e.g. case Bool will handle an int. 
            let n = obj as! NSNumber
            if n.isBool {
                return Val.bool(n.boolValue)
            } else {
                let d = n.doubleValue
                if d == Double(Int(d)) {
                    return Val.int(Int(d))
                } else {
                    return Val.float(n.doubleValue)
                }
            }

        case let b as Bool:
            return Val.bool(b)

        case let i as Int:
            return Val.int(i)

        case let d as Double:
            return Val.float(d)

        case let s as String:
            return Val.string(s)

        case let a as [Any]:
            let va: [Val] = try a.map({try withJSONObject($0)})
            return Val.array(va)

        case let a as NSArray:
            let va: [Val] = try a.map({try withJSONObject($0)})
            return Val.array(va)

        case let d as [String:Any]:
            var dd = [String:Val]()
            for (k,v) in d {
                dd[k] = try withJSONObject(v)
            }
            return Val.dict(dd)

        case let d as NSDictionary:
            var dd = [String:Val]()
            for (k,v) in d {
                guard let keyStr = k as? String else {
                    throw C4Error(domain: .LiteCoreDomain, code: Int32(InvalidKey))
                }
                dd[keyStr] = try withJSONObject(v)
            }
            return Val.dict(dd)

        default:
            throw C4Error(domain: .LiteCoreDomain, code: Int32(InvalidKey))
        }
    }


    public func asJSON() -> Data {
        return asJSON().data(using: String.Encoding.utf8)!
    }


    public func asJSON() -> String {
        var json: String = ""
        toJSON(&json)
        return json;
    }


    private func toJSON(_ json: inout String) {
        switch self {
        case .null:
            json += "null"
        case .bool(let b):
            json += (b ? "true" : "false")
        case .int(let i):
            json += String(i)
        case .float(let f):
            json += String(f)
        case .string(let s):
            Val.writeQuotedString(s, to: &json)
        case .data(let d):
            json += "\"" + d.base64EncodedString() + "\""
        case .array(let a):
            json += "["
            var first = true
            for item in a {
                if first {
                    first = false
                } else {
                    json += ","
                }
                item.toJSON(&json)
            }
            json += "]"
        case .dict(let d):
            json += "{"
            var first = true
            for (k, v) in d {
                if first {
                    first = false
                } else {
                    json += ","
                }
                Val.writeQuotedString(k, to: &json)
                json += ":"
                v.toJSON(&json)
            }
            json += "}"
        }
    }


    private static func writeQuotedString(_ str :String, to: inout String) {
        to += "\"" + str + "\""     //FIX: Escape quotes!
    }


    public var debugDescription: String {
        return asJSON()
    }


    func asDict() -> [String:Val]? {
        switch self {
        case .dict(let d):
            return d
        default:
            return nil
        }
    }


    func unwrap() -> Any {
        switch self {
        case .null:
            return NSNull()
        case .bool(let b):
            return b
        case .int(let i):
            return i
        case .float(let f):
            return f
        case .string(let s):
            return s
        case .data(let d):
            return d
        case .array(let a):
            return a
        case .dict(let d):
            return d
        }
    }


    public static func == (a: Val, b: Val) -> Bool {
        switch (a, b) {
        case (.null, .null):
            return true
        case let (.bool(b1), .bool(b2)):
            return b1 == b2
        case let (.int(b1), .int(b2)):
            return b1 == b2
        case let (.float(b1), .float(b2)):
            return b1 == b2
        case let (.string(b1), .string(b2)):
            return b1 == b2
        case let (.data(b1), .data(b2)):
            return b1 == b2
        case let (.array(b1), .array(b2)):
            return b1 == b2
        case let (.dict(b1), .dict(b2)):
            return b1 == b2
        default:
            return false
        }
    }
}
