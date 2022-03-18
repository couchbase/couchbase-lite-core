#!/usr/bin/env python3
# -*- coding: utf-8 -*-

import os
import sys
import tarfile
import zipfile
import urllib.request
from pathlib import Path
from typing import Sequence
from urllib.error import HTTPError

VALID_PLATFORMS = ["android", "android-x86_64", "android-x86", "android-armeabi-v7a", "android-arm64-v8a", "centos6", "dotnet", "java", "linux", "macos", "macosx", "ios", "windows", "windows-arm-store", "windows-win32", "windows-win32-store", "windows-win64", "windows-win64-store"]

has_platform = False
quiet = False

def set_quiet(q: bool):
    global quiet
    quiet = q

def conditional_print(msg: str, end: str = '\n') -> None:
    global quiet
    if quiet:
        return

    print(msg, end=end)

def filename_for_platform(platform: str, debug: bool, build: str = None) -> str:
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

    if build is not None:
        build_parts = build.split("-")
        edition = "enterprise" if len(build_parts) > 2 and build_parts[2] == "EE" else "community"
        return f"couchbase-lite-core-{edition}-{build_parts[0]}-{build_parts[1]}-{platform}{debug_str}.{ext}"

    return f"couchbase-lite-core-{platform}{debug_str}.{ext}"

def check_variant(download_folder: str, variant: str, build: str, debug: bool, output_base: str) -> int:
    """Checks a provided variant and prints information about whether or not it will succeed
    
    This is used for the dry run mode.
    
    Parameters
    ----------
    download_folder : str
        The URL of the folder containing the variant to be downloaded
    variant : str
        The name of the variant (e.g. windows-win64)
    build: str
        If applicable, the build version to use in in filename (append -EE for enterprise e.g. 3.1.0-97-EE)
    debug: bool
        If true, download the debug variant instead of the release one
    output_base : str
        The path to the base output directory for the downloaded files

    Returns
    -------
    int
        0 on success, 1 on failure (to be used for running tally of failures)
    """

    filename = filename_for_platform(variant, debug, build)
    failed_count = 0
    download_url = f"{download_folder}/{filename}"
    download_path = calculate_download_path(variant, output_base)
    print(f"--- Checking {filename}".ljust(80, '.'), end='')
    try:
        with urllib.request.urlopen(download_url):
            print("200")
    except HTTPError as e:
        print(e.code)
        failed_count = 1


    print(f"\tDownload path: {download_path}")

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

def download_variant(download_folder: str, variant: str, build: str, debug: bool, output_base: str) -> int:
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

    filename = filename_for_platform(variant, debug, build)
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

def calculate_variants(original) -> set:
    final_variants = set()
    for v in original:
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

    return final_variants