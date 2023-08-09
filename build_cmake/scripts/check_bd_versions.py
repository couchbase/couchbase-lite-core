#!/usr/bin/env python3

from pathlib import Path

import os, sys, yaml, subprocess, argparse

def is_changed(git: str) -> bool:
    if len(git) == 0:
        return False
        
    difflines = git.splitlines()[-2:]
    if not difflines[0].startswith("-Subproject"):
        return True

    before = difflines[0].split(" ")[2]
    after = difflines[1].split(" ")[2].replace("-dirty", "")
    return before != after

def get_submodule_base(git: str):
    if len(git) == 0:
        return None
        
    difflines = git.splitlines()[-2:]
    before = difflines[0].split(" ")[2]
    after = difflines[1].split(" ")[2].replace("-dirty", "")
    if before == after:
        return None

    return before


def check_component(branch: str, title: str, component, expectChange: bool) -> bool:
    if "src-path" not in component:
        print(f"No src-path listed for {title}, skipping check...")
        return True
    
    print(f"Checking for changes in {title}...")
    cwd = os.getcwd()
    srcPath = str(component["src-path"])
    submoduleBase = branch
    if "parent-repo" in component:
        git = subprocess.check_output(["git", "diff", branch, component["parent-repo"]]).decode("ascii")
        submoduleBase = get_submodule_base(git)
        if not submoduleBase:
            if expectChange:
                print("! No source change to accompany change in blackduck manifest")
                return False
            else:
                print("Parent repo unchanged, skipping check...")
                return True

        
        os.chdir(component["parent-repo"])
        srcPath = srcPath.replace(component["parent-repo"], "").lstrip("/")

    git = subprocess.check_output(["git", "diff", submoduleBase, srcPath]).decode("ascii")
    os.chdir(cwd)
    if expectChange and not is_changed(git):
        print("! No source change to accompany change in blackduck manifest")
        return False

    if not expectChange and is_changed(git):
        print("! No manifest change to accompany change in source")
        print(git)
        return False

    return True

def main(manifest_path: Path, branch: str) -> int:
    git = subprocess.check_output(["git", "log", '--pretty="format:%B"', f"{branch}..HEAD"]).decode("ascii")
    for line in git.splitlines():
        if line.lstrip().startswith("!NO_BD_GITHUB"):
            print("Scan disabled by commit message, skipping...")
            return 0
    
    if not manifest_path.exists() or not manifest_path.is_file():
        print("!!! Blackduck manifest not found, aborting...")
        return 1

    manifest = yaml.load(manifest_path.read_bytes(), Loader=yaml.CLoader)
    subprocess.check_call(["git", "fetch", "origin", f"{branch}:{branch}"])
    subprocess.check_call(["git", "restore", "-s", branch, "-W", manifest_path.relative_to(os.getcwd())])
    manifest_old = yaml.load(manifest_path.read_bytes(), Loader=yaml.CLoader)
    subprocess.check_call(["git", "restore", manifest_path.relative_to(os.getcwd())])

    failCount = 0
    for component in manifest["components"]:
        if component not in manifest_old["components"]:
            print(f"{component} is newly added, skipping check...")
            continue

        versionBefore = manifest_old["components"][component]["versions"][0]
        versionAfter = manifest["components"][component]["versions"][0]
        if versionBefore == versionAfter:
            expectChange = False
            print(f"No blackduck manifest change for {component} (remains at {versionBefore})")
        else:
            expectChange = True
            print(f"Blackduck manifest change for {component} ({versionBefore} -> {versionAfter})")
        if not check_component(branch, component, manifest["components"][component], expectChange):
            failCount += 1

    return failCount

    

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Detect version changes in blackduck scanned deps')
    parser.add_argument('branch', type=str, help="The target branch of the PR")

    args = parser.parse_args()
    os.chdir((Path(os.path.dirname(__file__)) / ".." / ".." ).resolve())
    sys.exit(main(Path(os.getcwd()) / "jenkins" / "couchbase-lite-core-black-duck-manifest.yaml", args.branch))