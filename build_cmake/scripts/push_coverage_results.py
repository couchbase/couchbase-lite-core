#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import json
from os import environ
from octokit import Octokit
from argparse import ArgumentParser

def check_response(r, expected: int, method: str):
    if(r._response.status_code != expected):
        raise Exception(f"Bad response received for '{method}': {r._response.status_code}")
    

def post_coverage_comment(result_file: str, pr_number: str):
    gh_pat = environ.get("GH_PAT")
    with open(result_file, "r") as fin:
        result_content = json.load(fin)

    branch_percent = round(float(result_content["data"][0]["totals"]["branches"]["percent"]), 2)
    function_percent = round(float(result_content["data"][0]["totals"]["functions"]["percent"]), 2)
    instantiation_percent = round(float(result_content["data"][0]["totals"]["instantiations"]["percent"]), 2)
    line_percent = round(float(result_content["data"][0]["totals"]["lines"]["percent"]), 2)
    region_percent = round(float(result_content["data"][0]["totals"]["regions"]["percent"]), 2)
    if(gh_pat is None):
        print("WARNING: GH_PAT environment variable not set, skipping comment post...")
        return

    octo_client = Octokit(auth="token", token=gh_pat)
    r = octo_client.issues.list_issue_comments(owner="couchbase", repo="couchbase-lite-core", issue_number=pr_number)
    check_response(r, 200, "list_issue_comments")

    comment_id = 0
    for c in r.json:
        if(str(c["body"]).startswith("Code Coverage Results")):
            comment_id = int(c["id"])
            break

    comment_content = f"""Code Coverage Results:
    
| Type          | Percentage |
| -             | -          |
| Branch        | {branch_percent}      |
| Function      | {function_percent}      |
| Instantiation | {instantiation_percent}      |
| Line          | {line_percent}      |
| Region        | {region_percent}      |
"""

    print(f"Posting comment as \n\n{comment_content}")
    if comment_id == 0:
        r = octo_client.issues.create_an_issue_comment(owner="couchbase", repo="couchbase-lite-core", issue_number=pr_number, body=comment_content)
        check_response(r, 201, "create_comment")
    else:
        r = octo_client.issues.update_an_issue_comment(owner="couchbase", repo="couchbase-lite-core", 
            comment_id=comment_id, body=comment_content)
        check_response(r, 200, "update_comment")
        

if __name__ == "__main__":
    parser = ArgumentParser(description='Posts a comment on a given GitHub PR indicating the code coverage metrics')
    parser.add_argument('-r', '--results', type=str, help="The output JSON from llvm-cov", required=True)
    parser.add_argument('-n', '--pr-number', type=str, help="The PR number to post the comment to", required=True)

    args = parser.parse_args()
    post_coverage_comment(args.results, args.pr_number)

