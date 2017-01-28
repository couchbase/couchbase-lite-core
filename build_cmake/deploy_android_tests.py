#!/usr/bin/python

import subprocess
import query_android_devices

import os

class cd:
    """Context manager for changing the current working directory"""
    def __init__(self, newPath):
        self.newPath = os.path.expanduser(newPath)

    def __enter__(self):
        self.savedPath = os.getcwd()
        os.chdir(self.newPath)

    def __exit__(self, etype, value, traceback):
        os.chdir(self.savedPath)

device_info = query_android_devices.query_android_devices(True)

for key, value in device_info.iteritems():
    adb = subprocess.Popen(["adb", "-s", key, "push", "run_android_tests.sh", "/data/local/tmp/LiteCore"])
    adb.communicate()

    with cd("lib/{}".format(value["arch"])):
        adb = subprocess.Popen(["adb", "-s", key, "push", "libLiteCore.so","libsqlite3.so",
                                "LiteCore/tests/CppTests", "C/tests/C4Tests",
                                "/data/local/tmp/LiteCore"])
        adb.communicate()
        adb = subprocess.Popen(["adb", "-s", key, "shell", "cd /data/local/tmp/LiteCore && ./run_android_tests.sh"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        for output_line in iter(adb.stdout.readline, b''):
            print output_line.rstrip()


