#!/usr/bin/env python3

from git import Repo
import os
import requests
from broadcast_stage_base import parse_build_hashes

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

    version_parse = parse_build_hashes(build)
    if not version_parse.success:
        print(version_parse.errMsg)
        exit(2 + version_parse.errCode)

    ce_hash_found = str(core_repo.commit(core_repo.head).parents[1])
    if version_parse.ceHash != ce_hash_found:
        print(f"Bad build number {build}. Expected CE hash {version_parse.ceHash} but found {ce_hash_found}, aborting...")
        exit(5)
    
    message_to_send = f"""LiteCore {build} staged!

CE hash: {version_parse.ceHash}
EE hash: {version_parse.eeHash}
Amalgamated hash: {version_parse.amalgamatedHash}

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
