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
import sys
import tarfile
import zipfile
import urllib.request
from pathlib import Path
from typing import Sequence
from urllib.error import HTTPError

from git import Repo

VALID_PLATFORMS = ["android", "android-x86_64", "android-x86", "android-armeabi-v7a", "android-arm64-v8a", "centos6", "java", "linux", "macos", "macosx", "ios", "windows", "windows-arm-store", "windows-win32", "windows-win32-store", "windows-win64", "windows-win64-store"]

has_platform = False
quiet = False

def conditional_print(msg: str, end: str = '\n') -> None:
    if quiet:
        return

    print(msg, end=end)

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

def filename_for_platform(platform: str, debug: bool) -> str:
    """Calculates the filename to download from the build server
    
    Parameters
    ----------
    platform : str
        The platform identifier (e.g. windows-win64) desired
    debug: bool
        If true, download the debug variant instead of the release one

    Returns
    -------
    str
        The filename to download from the build server
    """

    debug_str = "-debug" if debug else ""
    ext = "tar.gz" if platform == "linux" or platform == "centos6" else "zip"
    return f"couchbase-lite-core-{platform}{debug_str}.{ext}"

def check_variant(download_folder: str, variant: str, debug: bool, output_base: str) -> int:
    """Checks a provided variant and prints information about whether or not it will succeed
    
    This is used for the dry run mode.
    
    Parameters
    ----------
    download_folder : str
        The URL of the folder containing the variant to be downloaded
    variant : str
        The name of the variant (e.g. windows-win64)
    debug: bool
        If true, download the debug variant instead of the release one
    output_base : str
        The path to the base output directory for the downloaded files

    Returns
    -------
    int
        0 on success, 1 on failure (to be used for running tally of failures)
    """

    filename = filename_for_platform(variant, debug)
    failed_count = 0
    download_url = f"{download_folder}/{filename}"
    download_path = calculate_download_path(variant, output_base)
    print(f"--- Checking {filename}".ljust(70, '.'), end='')
    try:
        with urllib.request.urlopen(download_url):
            print("200")
    except HTTPError as e:
        print(e.code)
        failed_count = 1


    print(f"\tDownload path: {download_path}")

    return failed_count

def main(variants, debug: bool, dry: bool, sha: str, ce: str, ee: str, output_path: str) -> int:
    if not sha:
        sha = calculate_sha(ce, ee)

    download_folder = f"http://latestbuilds.service.couchbase.com/builds/latestbuilds/couchbase-lite-core/sha/{sha[0:2]}/{sha}"
    conditional_print(f"--- Using URL {download_folder}/<filename>")
    
    failed_count = 0
    for v in variants:
        if dry:
            failed_count += check_variant(download_folder, v, debug, output_path)
        else:
            failed_count += download_variant(download_folder, v, debug, output_path)

    return failed_count

def resolve_platform_path(path: str) -> Path:
    """Calculates the absolute path to the folder containins platform extensions file.  

    Relative paths are considered relative to this script's folder
    
    Parameters
    ----------
    path : str
        The path given as input to the script

    Returns
    -------
    Path
        The resolved, absolute path to the folder containing the platform extensions file
    """

    ret_val = Path(path)
    if not ret_val.is_absolute():
        ret_val = Path(os.path.dirname(__file__)).joinpath(ret_val).resolve()

    if not ret_val.exists():
        print(f"!!! {ret_val} does not exist, aborting...")
        exit(-1)

    return ret_val

def import_platform_extensions(path: Path):
    """Attempts to import the platform extensions
    
    Parameters
    ----------
    path : Path
        The path to the folder containing the platform extensions file
    """

    try:
        sys.path.insert(0, str(path.absolute()))
        global subdirectory_for_variant
        from platform_fetch import subdirectory_for_variant
        global has_platform
        has_platform = True
    except ImportError:
        print("!!! Unable to import platform extensions, falling back to <os>/<abi> for subdirectory...")

def variant_to_pair(variant: str) -> Sequence[str]:
    """Given a variant, splits it into an OS and ABI pair
    
    Parameters
    ----------
    variant : str
        The variant received as input to the script (e.g. windows-win64)

    Returns
    -------
    Sequence[str]
        A 2 item sequence containing the OS at position 0 and, if applicable, the ABI at position 1 (otherwise empty string)
    """

    if variant == "linux" or variant == "centos6":
        return [variant, "x86_64"]
    
    if variant == "macosx":
        return ["macos", "x86_64"]

    if variant == "ios":
        return ["ios", ""]

    first_dash = variant.index("-")
    osname = variant[0:first_dash]
    abi = variant[first_dash+1:]
    if osname == "android":
        return [osname, abi]
    
    if abi.find("win64") != -1:
        return ["windows", abi.replace("win64", "x86_64")]
    elif abi.find("win32") != -1:
        return ["windows", abi.replace("win32", "x86")]
    else:
        return [osname, abi]

def calculate_download_path(variant: str, output_base: str) -> Path:
    """Calculate the path to download the LiteCore artifacts to

    Relative paths will be considered relative to the current working directory.
    
    Parameters
    ----------
    variant : str
        The variant received as input to the script (e.g. windows-win64)
    output_base : str
        The path to the base output directory for the downloaded files

    Returns
    -------
    Path
        The path of the folder to download the LiteCore artifacts into
    """

    output_base_path = Path(output_base)
    if not output_base_path.is_absolute():
        output_base_path = Path(os.getcwd()).joinpath(output_base_path)
    
    variant_pair = variant_to_pair(variant)
    subdirectory = subdirectory_for_variant(variant_pair[0], variant_pair[1]) if has_platform else f"{variant_pair[0]}/{variant_pair[1]}"
    return output_base_path.joinpath(subdirectory).resolve()

def download_variant(download_folder: str, variant: str, debug: bool, output_base: str) -> int:
    """Performs the download and extraction of LiteCore artifacts
    
    Parameters
    ----------
     download_folder : str
        The URL of the folder containing the variant to be downloaded
    variant : str
        The name of the variant (e.g. windows-win64)
    debug: bool
        If true, download the debug variant instead of the release one
    output_base : str
        The path to the base output directory for the downloaded files

    Returns
    -------
    int
        0 on success, 1 on failure (to be used for running tally of failures)
    """

    filename = filename_for_platform(variant, debug)
    download_url = f"{download_folder}/{filename}"
    download_path = calculate_download_path(variant, output_base)
    conditional_print(f"--- Downloading {filename} to {download_path}...")
    
    os.makedirs(download_path, exist_ok=True)

    full_path = f"{download_path}/{filename}"
    try:
        urllib.request.urlretrieve(download_url, full_path)
    except HTTPError as e:
        print(f"!!! Failed: {e.code}")
        return 0

    conditional_print(f"--- Extracting {filename}...")
    if filename.endswith("tar.gz"):
        with tarfile.open(full_path, "r:gz") as tar:
            tar.extractall(download_path)
    else:
        with zipfile.ZipFile(full_path) as zip:
            zip.extractall(download_path)
    
    os.remove(full_path)
    return 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description='Fetch a specific prebuilt LiteCore')
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
        print("!!! Neither SHA nor CE repo path defined, aborting...")
        parser.print_usage()
        exit(-1)

    full_path = resolve_platform_path(args.ext_path)
    import_platform_extensions(full_path)

    final_variants = set()
    for v in args.variants:
        if v == "dotnet":
            final_variants |= {"linux", "android-x86_64", "android-x86", "android-armeabi-v7a", "android-arm64-v8a", "macosx", "ios", "windows-win64", "windows-win64-store", "windows-win32", "windows-win32-store", "windows-arm-store"}
        elif v == "android":
            final_variants |= {"android-x86_64", "android-x86", "android-armeabi-v7a", "android-arm64-v8a"}
        elif v == "java":
            final_variants |= {"linux", "macosx", "windows-win64"}
        elif v == "windows":
            final_variants |= {"windows-win64", "windows-win64-store", "windows-win32", "windows-win32-store", "windows-arm-store"}
        elif v == "macos":
            final_variants |= {"macosx"}
        else:
            final_variants |= {v}

    quiet = args.quiet
    exit(main(final_variants, args.debug, args.dry_run, args.sha, args.ce, args.ee, args.output))
