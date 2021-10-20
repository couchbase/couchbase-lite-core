#!/usr/bin/env python3

import subprocess
import os
import sys
from termcolor import colored, cprint
import colorama

def get_repo_url(dir:str):
    git_result = subprocess.run("git remote get-url origin".split(), capture_output=True, cwd=dir)
    return git_result.stdout.decode("utf-8").strip()

def get_relevant_submodules(dir: str):
    ret_val = []
    git_result = subprocess.run("git submodule status".split(), capture_output=True, cwd=dir)
    for line in git_result.stdout.decode("utf-8").split("\n"):
        if not line:
            continue

        path = line.split()[1]
        url = get_repo_url(dir + path)
        if "couchbasedeps" in url:
            ret_val.append(path)
        else:
            cprint(f'{path} is not in couchbasedeps, skipping...', 'cyan')

    return ret_val

def check_commit(dir: str):
    git_result = subprocess.run("git branch -r --contains HEAD".split(), capture_output=True, cwd=dir)
    return "couchbase-master" in git_result.stdout.decode("utf-8")

def main(dir: str):
    colorama.init()
    submodule_list = get_relevant_submodules(dir)
    print()
    fail_count = 0
    for submodule in submodule_list:
        if not check_commit(dir + submodule):
            print(submodule.ljust(40, "."), colored("[FAIL]", "red"))
            git_result = subprocess.run("git branch --show-current".split(), capture_output=True, cwd=dir+submodule)
            print(colored('\tcurrent branch is', 'cyan'), git_result.stdout.decode("utf-8").strip())
            fail_count += 1
        else:
            print(submodule.ljust(40, "."), colored("[OK]", "green"))
        
    return fail_count

if __name__ == '__main__':
    input_dir = os.path.dirname(os.path.realpath(__file__)) + os.sep + ".." + os.sep + ".." + os.sep
    sys.exit(main(input_dir))