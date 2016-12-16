import os
import glob
from datetime import date

TEMPLATE = """//
// %(filename)s
//
// Author:
// 	Jim Borden  <jim.borden@couchbase.com>
//
// Copyright (c) %(year)d Couchbase, Inc All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

using System;
using System.Linq;
using System.Runtime.InteropServices;

using LiteCore.Util;

namespace LiteCore.Interop
{
    public unsafe static partial class Native
    {
%(native)s
    }
    
    public unsafe static partial class NativeRaw
    {
%(native_raw)s
    }
}
"""

METHOD_DECORATION = "        [DllImport(Constants.DllName, CallingConvention = CallingConvention.Cdecl)]\n"
bridge_literals = {}
raw_literals = {}

def transform_raw(arg_info):
    if(arg_info[0] == "bool"):
        return "[MarshalAs(UnmanagedType.U1)]bool {}".format(arg_info[1])

    if(arg_info[0] == "C4Slice_b"):
        return "C4Slice {}".format(arg_info[1])
        
    if arg_info[0].endswith("*[]"):
        return "{}** {}".format(arg_info[0][:-3], arg_info[1])

    return " ".join(arg_info)
    
def transform_raw_return(arg_type):
    if(arg_type.endswith("_b")):
        return arg_type[:-2]
        
    return arg_type

def transform_bridge(arg_type):
    if(arg_type == "C4SliceResult" or arg_type == "C4Slice"):
        return "string"
        
    if(arg_type == "C4SliceResult_b" or arg_type == "C4Slice_b"):
        return "byte[]"
    
    if arg_type == "UIntPtr":
        return "ulong"
        
    return arg_type
    
def transform_using(arg_info):
    tmp = arg_info.split(':')
    arg_type = tmp[0]
    arg_name = tmp[1]
    if(arg_type == "C4SliceResult" or arg_type == "C4Slice"):
        return "            using(var {0}_ = new C4String({0}))".format(arg_name)
    
    if arg_type == "C4SliceResult_b" or arg_type == "C4Slice_b":
        return "            fixed(byte* {0}_ = {0})".format(arg_name)
        
    return ""
    
def generate_bridge_sig(pieces, bridge_args):
    retVal = ""
    if(len(pieces) > 2):
        for args in pieces[2:-1]:
            if(args.startswith("C4Slice")):
                bridge_args.append(args)
                
            arg = "{} {}".format(transform_bridge(args[:args.index(':')]), args[args.index(':') + 1:])   
            retVal += "{}, ".format(arg)
        
        args = pieces[-1]
        if(args.startswith("C4Slice")):
            bridge_args.append(args)

        retVal += "{} {}".format(transform_bridge(args[:args.index(':')]), args[args.index(':') + 1:]) 
       
    retVal += ")\n        {\n"
    return retVal
    
def generate_using_parameters_begin(bridge_args):
    retVal = ""
    if len(bridge_args) > 0:
        for bridge_arg in bridge_args[:-1]:
            retVal += "{}\n".format(transform_using(bridge_arg))
        retVal += "{} {{\n".format(transform_using(bridge_args[-1]))
        
    return retVal
        
def generate_using_parameters_end(bridge_args):
    retVal = ""
    if len(bridge_args) > 0:
        retVal += "            }\n"
    
    return retVal
    
def bridge_parameter(param, return_space):
    splitPiece = param.split(':')
    if splitPiece[0] == "C4Slice":
        return_space[0] = "                "
        return "{}_.AsC4Slice(), ".format(splitPiece[1])
    elif splitPiece[0] == "C4Slice_b":
        return_space[0] = "                "
        return "new C4Slice({0}_, (ulong){0}.Length), ".format(splitPiece[1])
    elif splitPiece[0] == "UIntPtr":
        return "(UIntPtr){}, ".format(splitPiece[1])
    
    return "{}, ".format(splitPiece[1])
    
def generate_return_value(pieces):
    raw_call_params = "("
    return_space = ["            "]
    retval_type = pieces[0][1:]
    if len(pieces) > 2:
        for piece in pieces[2:-1]:
            raw_call_params += bridge_parameter(piece, return_space)
        
        raw_call_params += bridge_parameter(pieces[-1], return_space)[:-2]
         
    raw_call_params += ")"
    if retval_type == "void":
        return "{}NativeRaw.{}{};\n".format(return_space[0], pieces[1], raw_call_params)
        
    if retval_type == "UIntPtr":
        return "{}return (NativeRaw.{}{}).ToUInt64();\n".format(return_space[0], pieces[1], raw_call_params)
        
    if not retval_type.startswith("C4Slice"):
        return "{}return NativeRaw.{}{};\n".format(return_space[0], pieces[1], raw_call_params)
 
    return_statement = "{0}using(var retVal = NativeRaw.{1}{2}) {{\n{0}    ".format(return_space[0], pieces[1], raw_call_params)
    if retval_type == "C4SliceResult":
        return_statement += "return ((C4Slice)retVal).CreateString();\n{}}}\n".format(return_space[0])
    elif retval_type == "C4SliceResult_b":
        return_statement += "return ((C4Slice)retVal).ToArrayFast();\n{}}}\n".format(return_space[0])
    elif retval_type == "C4Slice_b":
        return_statement += "return retVal.ToArrayFast();\n{}}}\n".format(return_space[0])
    else:
        return_statement += "return retVal.CreateString();\n{}}}\n".format(return_space[0])
        
    return return_statement
    
def insert_bridge(collection, pieces):
    if pieces[1] in bridge_literals:
        collection.append(bridge_literals[pieces[1]])
        return
    
    line = "        public static {} {}(".format(transform_bridge(pieces[0][1:]), pieces[1]) 
    bridge_args = []
    line += generate_bridge_sig(pieces, bridge_args)
    line += generate_using_parameters_begin(bridge_args)
    line += generate_return_value(pieces)
    line += generate_using_parameters_end(bridge_args)
    line += "        }\n\n"
    collection.append(line)
    
def insert_raw(collection, pieces):
    collection.append(METHOD_DECORATION)
    if(pieces[1] in raw_literals):
        collection.append(raw_literals[pieces[1]])
        return
        
    if(pieces[0] == ".bool"):
        collection.append("        [return: MarshalAs(UnmanagedType.U1)]\n")
    
    line = "        public static extern {} {}(".format(transform_raw_return(pieces[0][1:]), pieces[1])
    if(len(pieces) > 2):
        for args in pieces[2:-1]:
            arg = args.split(':')
            line += "{}, ".format(transform_raw(arg))
        
        line += transform_raw(pieces[-1].split(':'))
    
    line += ');\n\n'
    collection.append(line)
    
def read_literals(filename, collection):
    try:
        fin = open(filename, "r")
    except IOError:
        return

    key = ""
    value = ""
    for line in fin:
        if not line.startswith(" ") and len(line) > 2:
            if len(value) > 0:
                collection[key] = value
            
            key = line.strip("\n")
            value = ""
        else:
            value += line
            
    fin.close()
    collection[key] = value
        
read_literals("bridge_literals.txt", bridge_literals)
read_literals("raw_literals.txt", raw_literals)
for filename in glob.iglob("*.template"):
    native = []
    native_raw = []
    out_filename = os.path.splitext(filename)[0]
    outs = open(out_filename, "w")
    
    ins = open(filename, "r")
    for line in ins:
        pieces = line.split()
        if(pieces[0] == ".bridge"):
            insert_bridge(native, pieces[1:])
            insert_raw(native_raw, pieces[1:])
        else:
            insert_raw(native, pieces[1:])

    output = TEMPLATE % {"filename":out_filename[2:], "year":date.today().year, "native": ''.join(native), "native_raw": ''.join(native_raw)}
    outs.write(output)
    outs.close()
    
    
