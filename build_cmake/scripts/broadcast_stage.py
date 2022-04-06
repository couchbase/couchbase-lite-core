#!/usr/bin/env python3

from git import Repo
import os
import requests
import hashlib
import xml.etree.ElementTree as ET

BASE_DIR = os.path.realpath(os.path.join(os.path.dirname(__file__), "..", ".."))

def broadcast():
    core_repo = Repo(BASE_DIR)
    commit = core_repo.commit(core_repo.head)
    commit_message = commit.message
    if len(commit.parents) != 2:
        print("!!! Commit to staging branch is not a merge commit, aborting...")
        exit(1)

    build = None
    commit_message_lines = []
    for line in commit_message.split("\n"):
        if line.startswith("Build-To-Use:"):
            build = line.split(":")[1].strip()
        else:
            commit_message_lines.append(line)

    commit_message = "\n".join(commit_message_lines).strip()
    if build is None:
        print("!!! No build found in commit message, aborting...")
        exit(2)

    version_info = build.split("-")
    if len(version_info) != 2:
        print(f"!!! Invalid version in commit message: {build}, aborting...")
        exit(3)
    
    ce_hash = None
    ee_hash = None
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
        
    if ce_hash is None or ee_hash is None:
        print("!!! Malformed manifest, aborting...")
        exit(4)
    
    ce_hash_found = str(core_repo.commit(core_repo.head).parents[1])
    if ce_hash != ce_hash_found:
        print(f"Bad build number {build}. Expected CE hash {ce_hash} but found {ce_hash_found}, aborting...")
        exit(5)

    amalgamated_sha = hashlib.sha1(f"{ce_hash}{ee_hash}".encode("ascii"))
    
    message_to_send = f"""LiteCore {build} staged!

CE hash: {ce_hash}
EE hash: {ee_hash}
Amalgamated hash: {amalgamated_sha.hexdigest()}

Details:
{commit_message}"""

    print(message_to_send)
    if not "SLACK_WEBHOOK_URL" in os.environ:
        print()
        print("SLACK_WEBHOOK_URL not set, skipping slack ping...")
        return

    with requests.post(os.environ["SLACK_WEBHOOK_URL"], json={"text": f"<!here> {message_to_send}"}) as r:
        r.raise_for_status()

if __name__ == "__main__":
    broadcast()
