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

    print "Running tests on {} (API {} / {})".format(key, value["api"], value["arch"])
    with cd("lib/{}".format(value["arch"])):
        subprocess.check_output(["adb", "-s", key, "push", "libLiteCore.so","libsqlite3.so",
                                "LiteCore/tests/CppTests", "C/tests/C4Tests",
                                "/data/local/tmp/LiteCore"])
        try:
            subprocess.check_output(["adb", "-s", key, "shell", "cd /data/local/tmp/LiteCore && ./run_android_tests.sh"], stderr=subprocess.STDOUT)
        except subprocess.CalledProcessError as e:
            print e.output
            print "Tests failed!"
