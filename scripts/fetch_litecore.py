#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""
A script for fetching prebuilt LiteCore artifacts from the Couchbase build artifact server.

REQUIREMENTS: Git must be installed on the machine this script is run on

This script can be extended in the following way:

Create a file called "platform_fetch.py" with a function subdirectory_for_variant(os: str, abi: str) inside of it.
This function should examine the os / abi combination and return a relative directory to use that will be appended
to the output directory when a download occurs.  This file can either be placed in the same directory as this
script, or the path to its parent directory passed in via the --ext-path argument.

Here is a list of the current values you can expect from each variant:
|       VARIANT       |    OS    |      ABI      |
| ------------------- | -------- | ------------- |
| android-x86_64      | android  | x86_64        |
| android-x86         | android  | x86           |
| android-armeabi-v7a | android  | armeabi-v7a   |
| android-arm64-v8a   | android  | arm64-v8a     |
| centos6             | centos6  | x86_64        |
| linux               | linux    | x86_64        |
| macosx              | macos    | x86_64        |
| ios                 | ios      | <empty>       | <-- multiple architectures all in one
| windows-arm-store   | windows  | arm-store     |
| windows-win32       | windows  | x86           |
| windows-win32-store | windows  | x86-store     |
| windows-win64       | windows  | x86_64        |
| windows-win64-store | windows  | x86_64-store  |
"""

import argparse
import hashlib
import os

from git import Repo
from fetch_litecore_base import conditional_print, check_variant, download_variant, resolve_platform_path, import_platform_extensions, calculate_variants, VALID_PLATFORMS, set_quiet

def calculate_sha(ce: str, ee: str) -> str:
    """Calculates the SHA to use for download based on CE and EE repository path
    
    Parameters
    ----------
    ce : str
        The path to the LiteCore CE repo
    ee : str
        The path to the EE repo, or None if CE is to be used

    Returns
    -------
    str
        The hash to use to find the relevant download artifact on the build server
    """

    CE_repo = Repo(ce)
    CE_sha = str(CE_repo.head.commit)
    print(f"--- CE SHA detected as {CE_sha}")

    if not ee:
        return CE_sha
        
    EE_repo = Repo(ee)
    EE_sha = str(EE_repo.head.commit)
    print(f"--- EE SHA detected as {EE_sha}")

    amalgamation = CE_sha + EE_sha
    m = hashlib.sha1()
    m.update(amalgamation.encode('ascii'))
    final_sha = m.digest().hex()
    print(f"--- Final SHA: {final_sha}")
    return final_sha

def download_litecore(variants, debug: bool, dry: bool, sha: str, ce: str, ee: str, output_path: str) -> int:
    if not sha:
        sha = calculate_sha(ce, ee)

    download_folder = f"http://latestbuilds.service.couchbase.com/builds/latestbuilds/couchbase-lite-core/sha/{sha[0:2]}/{sha}"
    conditional_print(f"--- Using URL {download_folder}/<filename>")
    
    failed_count = 0
    for v in variants:
        if dry:
            failed_count += check_variant(download_folder, v, None, debug, output_path)
        else:
            failed_count += download_variant(download_folder, v, None, debug, output_path)

    return failed_count


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Fetch a specific prebuilt LiteCore by SHA')
    parser.add_argument('-v', '--variants', nargs='+', type=str, help='A space separated list of variants to download', required=True, choices=VALID_PLATFORMS, metavar="PLATFORM")
    parser.add_argument('-d', '--debug', action='store_true', help='If specified, download debug variants')
    parser.add_argument('-D', '--dry-run', action='store_true', help='Check for existience of indicated artifacts, but do not perform download')
    parser.add_argument('-s', '--sha', type=str, help="The SHA to download.  If not provided, calculated based on the provided CE and (optionally) EE repos.  Required if CE not specified.")
    parser.add_argument('--ce', type=str, help="The path to the CE LiteCore repo.  Required if SHA not specified.")
    parser.add_argument('--ee', type=str, help="The path to the EE LiteCore repo")
    parser.add_argument('-x', '--ext-path', type=str, help="The path in which the platform specific extensions to this script are defined (platform_fetch.py).  If a relative path is passed, it will be relative to fetch_litecore.py.  By default it is the current working directory.",
        default=os.getcwd())
    parser.add_argument('-o', '--output', type=str, help="The directory in which to save the downloaded artifacts", default=os.getcwd())
    parser.add_argument('-q', '--quiet', action='store_true', help="Suppress all output except during dry run.")

    args = parser.parse_args()

    if not args.sha and not args.ce:
        print("!!! Neither SHA nor CE repo path nor build defined, aborting...")
        parser.print_usage()
        exit(-1)

    full_path = resolve_platform_path(args.ext_path)
    import_platform_extensions(full_path)

    final_variants = calculate_variants(args.variants)
    set_quiet(args.quiet)
    exit(download_litecore(final_variants, args.debug, args.dry_run, args.sha, args.ce, args.ee, args.output))
