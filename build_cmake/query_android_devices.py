#!/usr/bin/python

import subprocess
import re
import argparse

KEYMAP = {
    "ro.product.cpu.abi": "arch",
    "ro.build.version.sdk": "api"
}
VERBOSE = False

def query_android_devices(verbose):
    global VERBOSE
    VERBOSE = verbose
    retVal = {}
    adb = subprocess.Popen(["adb", "devices"], stdout=subprocess.PIPE)
    output, _ = adb.communicate()
    for line in output.splitlines():
        split = line.split('\t')
        if len(split) == 2:
            dev_id = split[0]
            if VERBOSE:
                print "Found {}".format(dev_id)
            device_props = extract_props(dev_id)
            device_props["ips"] = get_ip_addr(dev_id)
            retVal[dev_id] = device_props

    return retVal

def extract_props(device_id):
    retVal = {"emulator":False}
    regex = re.compile("\[(.*)\]: \[(.*)\]")
    adb = subprocess.Popen(["adb", "-s", device_id, "shell", "getprop"], stdout=subprocess.PIPE)
    output, _ = adb.communicate()
    for line in output.splitlines():
        match = regex.match(line)
        if match:
            for key, value in KEYMAP.iteritems():
                if match.group(1) == key:
                    retVal[value] = match.group(2)
                    if VERBOSE:
                        print "\t{} = {}".format(value, match.group(2))
                elif match.group(1) == "ro.build.characteristics" and "emulator" in match.group(2):
                    retVal["emulator"] = True
                    if VERBOSE:
                        print "\tEmulator detected ({} = {})".format(match.group(1), match.group(2))
                elif "genymotion" in match.group(1) and retVal["emulator"] == False:
                    retVal["emulator"] = True
                    if VERBOSE:
		        print "\tEmulator detected ({} = {})".format(match.group(1), match.group(2))

    return retVal

def get_ip_addr(device_id):
    retVal = []
    regex = re.compile("inet (\\S+)/")
    adb = subprocess.Popen(["adb", "-s", device_id, "shell", "ip", "a"], stdout=subprocess.PIPE)
    output, _ = adb.communicate()
    for line in output.splitlines():
        match = regex.search(line)
        if match:
            if match.group(1) != "127.0.0.1":
                retVal.append(match.group(1))

    if VERBOSE and len(retVal) > 0:
        print "\tips = {}".format(retVal)
    return retVal

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Query information about connected Android devices")
    parser.add_argument("--verbose", help="Print information to console",
                        action="store_true")
    args = parser.parse_args()
    print query_android_devices(args.verbose)

