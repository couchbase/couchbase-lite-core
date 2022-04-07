#!/usr/bin/env python3

import subprocess
import os
import sys
from termcolor import colored, cprint
import colorama
import argparse
from enum import Enum

class MatchType(Enum):
    NONE = 0
    BRANCH = 1
    TAG = 2

def get_repo_url(dir:str):
    git_result = subprocess.run("git remote get-url origin".split(), capture_output=True, cwd=dir)
    return git_result.stdout.decode("utf-8").strip()

def get_relevant_submodules(dir: str):
    ret_val = {}
    ret_val["couchbasedeps"] = []
    ret_val["other"] = []

    git_result = subprocess.run("git submodule status".split(), capture_output=True, cwd=dir)
    for line in git_result.stdout.decode("utf-8").split("\n"):
        if not line:
            continue

        path = line.split()[1]
        if path == "wiki":
            continue

        url = get_repo_url(dir + path)
        if "couchbasedeps" in url:
            ret_val["couchbasedeps"].append(path)
        else:
            ret_val["other"].append(path)

    return ret_val

def check_couchbasedeps_commit(dir: str):
    # First check for a tag on the commit.  This is a looser requirement, because a couchbasedeps
    # repo could be based on an unmodified release of an upstream project.  In that case, it will
    # be tagged.
    git_result = subprocess.run("git name-rev --tags --name-only HEAD".split(), capture_output=True, cwd=dir) \
        .stdout.decode("utf-8").strip()

    if git_result != "undefined" and not "~" in git_result:
        return MatchType.TAG

    git_result = subprocess.run("git branch -r --contains HEAD".split(), capture_output=True, cwd=dir)
    branches = git_result.stdout.decode("utf-8").splitlines()
    for branch in branches:
        branch = branch.strip()
        if branch.startswith("origin/couchbase-") or branch.endswith("-couchbase"):
            return MatchType.BRANCH
    
    return MatchType.NONE

def check_other_commit(dir: str, parent_branch: str) -> str:
    git_result = subprocess.run("git branch -r --contains HEAD".split(), capture_output=True, cwd=dir)
    branches = git_result.stdout.decode("utf-8").splitlines()
    for branch in branches:
        branch = branch.strip()
        if branch.endswith(parent_branch):
            return parent_branch

        # Special case reflecting the GitHub shift of master -> main
        if branch.endswith("main") and parent_branch == "master":
            return "main"
    
    return None

def get_current_branch(dir: str):
    git_result = subprocess.run("git branch --show-current".split(), capture_output=True, cwd=dir)
    subbranch = git_result.stdout.decode("utf-8").strip()
    if not subbranch:
        subbranch = "(no branch)"

    return subbranch

def get_nearby_couchbase_branch(dir: str):
    git_result = subprocess.run("git branch -r --contains HEAD".split(), capture_output=True, cwd=dir)
    branches = git_result.stdout.decode("utf-8").splitlines()
    for branch in branches:
        branch = branch.split()[-1].strip()
        if "couchbase" in branch:
            return branch[branch.find('/') + 1:]

    return "(unknown)"

def check_submodules(dir: str, branch: str):
    submodules = get_relevant_submodules(dir)
    fail_count = 0

    for submodule in submodules["couchbasedeps"]:
        check_result = check_couchbasedeps_commit(dir + submodule)
        if check_result == MatchType.NONE:
            print(submodule.ljust(40, "."), colored("[FAIL]", "red"))
            print(colored('\tcurrent branch is', 'cyan'), get_current_branch(dir + submodule))
            fail_count += 1
        else:
            print(submodule.ljust(40, "."), colored("[OK]", "green"))
            if check_result == MatchType.TAG:
                tag = subprocess.run("git name-rev --tags --name-only HEAD".split(), capture_output=True, cwd=dir + submodule) \
                    .stdout.decode("utf-8").strip()
                print(colored('\tOn upstream tag', 'cyan'), tag)
            else:
                print(colored('\tOn branch', 'cyan'), get_nearby_couchbase_branch(dir + submodule))

    for submodule in submodules["other"]:
        child_branch = check_other_commit(dir + submodule, branch)
        if not child_branch:
            print(submodule.ljust(40, "."), colored("[FAIL]", "red"))
            print(colored('\tcurrent branch is', 'cyan'), get_current_branch(dir + submodule), 'expected', branch)
            fail_count += 1
        else:
            print(submodule.ljust(40, "."), colored("[OK]", "green"))
            print(colored('\tOn branch', 'cyan'), child_branch)
        
    return fail_count

def main(dir: str, branch: str):
    colorama.init()
    print()
    return check_submodules(dir, branch)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Validate submodules for PR')
    parser.add_argument('branch', type=str, help="The branch to check for in non-couchbasedep repos")
    parser.add_argument('path', type=str, help="The path to the parent repo to check")

    args = parser.parse_args()
    input_dir = os.path.realpath(args.path) + os.sep
    sys.exit(main(input_dir, args.branch))
