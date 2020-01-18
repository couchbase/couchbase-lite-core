import os

def write_def_file(cbl_export_list, c4_export_list):
    with open(output_dir +  'c4.def', 'w') as fout:
        fout.write("EXPORTS\n\n")
        fout.write("; Couchbase Lite .NET\n")
        for cbl_export in cbl_export_list:
            fout.write("{0}\n".format(cbl_export))

        fout.write("; C4Tests\n")
        for c4_export in c4_export_list:
            fout.write("{0}\n".format(c4_export))

        if(os.path.isfile(input_dir + 'c4_def.txt')):
            fout.write("\n; Windows Specific\n")
            for line in open(input_dir + 'c4_def.txt', 'r'):
                fout.write(line)

        fout.write("\n")

def write_exp_file(cbl_export_list, c4_export_list):
    with open(output_dir + 'c4.exp', 'w') as fout:
        fout.write("# Couchbase Lite for Apple platforms\n")
        for cbl_export in cbl_export_list:
            if len(cbl_export) > 0:
                fout.write("_{0}\n".format(cbl_export))
            else:
                fout.write("\n")

        fout.write("# C4Tests\n")
        for c4_export in c4_export_list:
            if len(c4_export) > 0:
                fout.write("_{0}\n".format(c4_export))
            else:
                fout.write("\n")

        if os.path.isfile(input_dir + 'c4_exp.txt'):
            fout.write("\n# Apple specific\n")
            for line in open(input_dir + 'c4_exp.txt', 'r'):
                fout.write(line)

        fout.write("\n")

def write_gnu_file(cbl_export_list, c4_export_list):
    with open(output_dir + 'c4.gnu', 'w') as fout:
        fout.write("CBL {\n\tglobal:\n")
        for cbl_export in cbl_export_list:
            if len(cbl_export) > 0:
                fout.write("\t\t{0};\n".format(cbl_export))
            else:
                fout.write("\n")

        for c4_export in c4_export_list:
            if len(c4_export) > 0:
                fout.write("\t\t{0};\n".format(c4_export))
            else:
                fout.write("\n")

        if os.path.isfile(input_dir + 'c4_gnu.txt'):
            for line in open(input_dir + 'c4_gnu.txt', 'r'):
                fout.write(line)

        fout.write("\tlocal:\n\t\t*;\n};")

def write_export_files(output_dir):
    cbl_export_list=[]
    c4_export_list=[]
    current_array=cbl_export_list
    for line in open(input_dir + 'c4.txt', 'r'):
        if line.lstrip(" ").rstrip("\r\n") == "# C4Tests":
            current_array = c4_export_list
            continue
        elif line.lstrip(" ").startswith("#"):
            continue

        current_array.append(line.lstrip(" ").rstrip("\r\n"))

    write_def_file(cbl_export_list, c4_export_list)
    write_exp_file(cbl_export_list, c4_export_list)
    write_gnu_file(cbl_export_list, c4_export_list)

if __name__ == "__main__":
    input_dir = os.path.dirname(os.path.realpath(__file__)) + os.sep
    output_dir = input_dir + ".." + os.sep
    print("Writing files to " + output_dir)
    write_export_files(output_dir)