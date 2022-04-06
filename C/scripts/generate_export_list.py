#!/usr/bin/env python3

"""
Copyright 2019-Present Couchbase, Inc.
Use of this software is governed by the Business Source License included in
the file licenses/BSL-Couchbase.txt.  As of the Change Date specified in that
file, in accordance with the Business Source License, use of this software will
be governed by the Apache License, Version 2.0, included in the file
licenses/APL2.txt.
"""

import os

__cbl_key = "cbl"
__cbl_ee_key = "cbl_ee"
__c4_key = "c4"
__c4_ee_key = "c4_ee"


def write_line_to(line: str, files: list):
    for fout in files:
        fout.write(line)


def write_exports_to(ce: list, ee: list, files: list, fmt: str):
    for export in ce:
        if len(export) == 0:
            write_line_to("\n", files)
        else:
            write_line_to(fmt.format(export), files)

    if len(ee) == 0:
        return

    files[1].write("\n")
    for export in ee:
        if len(export) == 0:
            files[1].write("\n")
        else:
            files[1].write(fmt.format(export))


def write_def_file(exports: dict):
    with open(output_dir + 'c4.def', 'w') as fout, open(output_dir + 'c4_ee.def', "w") as fout_ee:
        both_files = [fout, fout_ee]
        write_line_to("EXPORTS\n\n", both_files)
        write_line_to("; Couchbase Lite .NET\n", both_files)
        write_exports_to(exports[__cbl_key], exports[__cbl_ee_key], both_files, "{0}\n")

        write_line_to("; C4Tests\n", both_files)
        write_exports_to(exports[__c4_key], exports[__c4_ee_key], both_files, "{0}\n")

        if(os.path.isfile(input_dir + 'c4_def.txt')):
            write_line_to("\n; Windows Specific\n", both_files)
            for line in open(input_dir + 'c4_def.txt', 'r'):
                write_line_to(line, both_files)

        if(os.path.isfile(input_dir + 'c4_ee_def.txt')):
            for line in open(input_dir + 'c4_ee_def.txt', 'r'):
                fout_ee.write(line)

        write_line_to("\n", both_files)


def write_exp_file(exports: dict):
    with open(output_dir + 'c4.exp', 'w') as fout, open(output_dir + 'c4_ee.exp', 'w') as fout_ee:
        both_files = [fout, fout_ee]
        write_line_to("# Couchbase Lite for Apple platforms\n", both_files)
        write_exports_to(exports[__cbl_key], exports[__cbl_ee_key], both_files, "_{0}\n")

        write_line_to("# C4Tests\n", both_files)
        write_exports_to(exports[__c4_key], exports[__c4_ee_key], both_files, "_{0}\n")

        if os.path.isfile(input_dir + 'c4_exp.txt'):
            write_line_to("\n# Apple specific\n", both_files)
            for line in open(input_dir + 'c4_exp.txt', 'r'):
                write_line_to(line, both_files)

        if os.path.isfile(input_dir + 'c4_ee_exp.txt'):
            for line in open(input_dir + 'c4_ee_exp.txt', 'r'):
                fout_ee.write(line)

        write_line_to("\n", both_files)


def write_gnu_file(exports: dict):
    with open(output_dir + 'c4.gnu', 'w') as fout, open(output_dir + 'c4_ee.gnu', 'w') as fout_ee:
        both_files = [fout, fout_ee]
        write_line_to("CBL {\n\tglobal:\n", both_files)
        write_exports_to(exports[__cbl_key], exports[__cbl_ee_key], both_files, "\t\t{0};\n")
        write_exports_to(exports[__c4_key], exports[__c4_ee_key], both_files, "\t\t{0};\n")

        if os.path.isfile(input_dir + 'c4_gnu.txt'):
            for line in open(input_dir + 'c4_gnu.txt', 'r'):
                write_line_to(line, both_files)

        if os.path.isfile(input_dir + 'c4_ee_gnu.txt'):
            for line in open(input_dir + 'c4_ee_gnu.txt', 'r'):
                fout_ee.write(line)

        write_line_to("\tlocal:\n\t\t*;\n};", both_files)


def write_export_files(output_dir: str):
    exports = {
        "cbl": [],
        "cbl_ee": [],
        "c4": [],
        "c4_ee": []
    }

    current_key = "cbl"
    for line in open(input_dir + 'c4.txt', 'r'):
        if line.lstrip(" ").rstrip("\r\n") == "# C4Tests":
            current_key = "c4"
            continue
        elif line.lstrip(" ").startswith("#"):
            continue

        exports[current_key].append(line.lstrip(" ").rstrip("\r\n"))

    current_key = "cbl_ee"
    for line in open(input_dir + 'c4_ee.txt', 'r'):
        if line.lstrip(" ").rstrip("\r\n") == "# C4Tests":
            current_key = "c4_ee"
            continue
        elif line.lstrip(" ").startswith("#"):
            continue

        exports[current_key].append(line.lstrip(" ").rstrip("\r\n"))

    #write_def_file(exports)
    write_exp_file(exports)
    #write_gnu_file(exports)


if __name__ == "__main__":
    input_dir = os.path.dirname(os.path.realpath(__file__)) + os.sep
    output_dir = input_dir + ".." + os.sep
    print("Writing files to " + output_dir)
    write_export_files(output_dir)
