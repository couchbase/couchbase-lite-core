#!/usr/bin/env python3

import argparse
import os
from re import T
import requests
from broadcast_stage_base import parse_build_hashes

def broadcast(version: str):
    if not "SLACK_WEBHOOK_URL" in os.environ:
        print()
        print("SLACK_WEBHOOK_URL not set, doing dry run...")
        webhook_url = None
    else:
        webhook_url = os.environ["SLACK_WEBHOOK_URL"]

    version_parse = parse_build_hashes(version)
    if not version_parse.success:
        print(version_parse.errMsg)
        exit(version_parse.errCode)

    message_to_send = f"""LiteCore {version} is the new current build for {version_parse.branchName}!

CE hash: {version_parse.ceHash}
EE hash: {version_parse.eeHash}
Amalgamated hash: {version_parse.amalgamatedHash}"""

    print(message_to_send)
    if webhook_url:
        with requests.post(os.environ["SLACK_WEBHOOK_URL"], json={"text": f"<!here> {message_to_send}"}) as r:
            r.raise_for_status()

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Broadcasts a notification for a new good buid for a given version of LiteCore')
    parser.add_argument('-v', '--version', type=str, help="The full version of the library to broadcast for (e.g. 3.0.0-123)", required=True)

    args = parser.parse_args()
    broadcast(args.version)