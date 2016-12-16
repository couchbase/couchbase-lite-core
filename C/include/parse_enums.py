#!/usr/bin/python

import glob
import re
from datetime import date

type_map = {"uint32_t":"uint","int32_t":"int","uint8_t":"byte","uint64_t":"ulong","uint16_t":"ushort"}
def parse_enum(filename):
    fin = open(filename, "r")
    name_to_type = {}
    name_to_entries = {}
    current_name = ""
    entries = []
    flags = []
    in_enum = False
    in_comment = 0
    for line in fin:
        if line.strip().startswith("//"):
            continue
        if in_enum:
            if "};" in line:
                name_to_entries[current_name] = entries
                entries = [] 
                in_enum = False
            else:
                if in_comment > 0:
                    if "*/" in line:
                        in_comment -= 1
                    
                    continue
                    
                if "/*" in line:
                    in_comment += 1
                        
                stripped = re.search("\\s*(.*?)(?:,|\\/)", line)
                if not stripped:
                    continue
                    
                entry = stripped.group(1)
                stripped = re.search("(?:k)?(?:C4|Rev)?(?:Log|DB_|Encryption)?(.*)", entry)
                entries.append(stripped.group(1).rstrip())
        else:
            definition = re.search("typedef C4_(ENUM|OPTIONS)\((.*?), (.*?)\) {", line)
            if definition:
                in_enum = True
                current_name = definition.group(3)
                name_to_type[current_name] = type_map[definition.group(2)]
                if definition.group(1) == "OPTIONS":
                    flags.append(current_name)

        if len(name_to_type) == 0:
            continue
            
    out_text = ""
    for name in name_to_type:
        if name in flags:
            out_text += "    [Flags]\n"
        out_text += "    public enum {} : {}\n    {{\n".format(name, name_to_type[name])
        for entry in name_to_entries[name]:
            out_text += "        {},\n".format(entry)

        out_text += "    }\n\n"
                
    return out_text[:-2]
