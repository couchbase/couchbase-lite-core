import sys
import requests
import os
import hashlib
import xml.etree.ElementTree as ET

class ManifestResult:
    def __init__(self, ce: str = None, ee: str = None, amalgamated: str = None, err: int = 0, errMsg: str = None, branch: str = None) -> None:
        self.__ce_hash = ce
        self.__ee_hash = ee
        self.__amalgamated_hash = amalgamated
        self.__err = err
        self.__errMsg = errMsg
        self.__branch = branch

    @property
    def success(self) -> bool:
        return self.__err == 0

    @property
    def errCode(self) -> int:
        return self.__err

    @property
    def errMsg(self) -> str:
        return self.__errMsg

    @property
    def ceHash(self) -> str:
        return self.__ce_hash

    @property
    def eeHash(self) -> str:
        return self.__ee_hash

    @property
    def amalgamatedHash(self) -> str:
        return self.__amalgamated_hash

    @property
    def branchName(self) -> str:
        return self.__branch

# Takes a full version string such as 3.0.0-123 and returns a BuildNumberResult
def parse_build_hashes(version: str):
    version_info = version.split("-")
    if len(version_info) != 2:
        return ManifestResult(err=1, errMsg=f"!!! Invalid version in commit message: {version}, aborting...")

    ce_hash = None
    ee_hash = None
    branch = None
    url = f"http://latestbuilds.service.couchbase.com/builds/latestbuilds/couchbase-lite-core/{version_info[0]}/{version_info[1]}/couchbase-lite-core-{version_info[0]}-{version_info[1]}-manifest.xml"
    with requests.get(url) as r:
        with open("manifest.xml", "wb") as fout:
            fout.write(r.content)
        manifest_tree = ET.parse("manifest.xml")
        os.unlink("manifest.xml")
        for project in manifest_tree.findall("./project"):
            if project.get("name").lower() == "couchbase-lite-core":
                ce_hash = project.get("revision")
            elif project.get("name").lower() == "couchbase-lite-core-ee":
                ee_hash = project.get("revision")

        for default in manifest_tree.findall("./default"):
            if default.get("remote").lower() == "couchbase":
                branch = default.get("revision")
        
    if ce_hash is None or ee_hash is None:
        return ManifestResult(err=2, errMsg="!!! Malformed manifest, aborting...")

    amalgamated_sha = hashlib.sha1(f"{ce_hash}{ee_hash}".encode("ascii"))
    return ManifestResult(ce_hash, ee_hash, amalgamated_sha.hexdigest(), branch=branch)

if __name__ == "__main__":
    print("!!! Error, this script is not standalone, it is meant to be consumed by other scripts.")
    sys.exit(1)