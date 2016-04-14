###CBForest for CSharp
-----
CBForest for CSharp can target Windows, OS X and Linux via Mono, and iOS and Android via Xamarin.  There is a "prebuilt" folder in this directory where the native binaries for each platform need to go.  These binaries are likely to change often so they will not be committed into the repo, but rather put out for each release.  Do not assume that releases are binary compatible, as we make no such guarantee at this time.  The only guarantee is that commits made affecting the C# portion will be compatible with the binaries generated at that commit.  If you need to make a build not based off of a release, these are the instructions:

####Windows (only 64-bit will be released, but 32-bit should build and work identically)
There is a visual studio project located [here](../CBForest.VS2015/).  Copy the output files x86/CBForest-Interop.dll and x64/CBForest-Interop.dll to the prebuilt folder (maintaining the folder structure).

####OS X (64-bit and 32-bit fat binary)
Simply run the `build-interop-osx.sh` script (you must have XCode installed).

####Linux (See note on Windows)
The build process uses GNU make.  In [this directory](NativeBuild/) simply run `make` to build and `make install` to copy the result to the correct location.

####iOS (armv7 arm64 fat binary)
You can run the `build-interop-ios-fat.sh` to get a fat binary for simulator and device, or use the Xcode project directly to get a slimmer version (device OR simulator)

####Android (arm64-v8a armveabi-v7a x86 x86_64 mips64 [five files])
This requires the Android NDK.  At the time of writing the version being used is r10, though if a new one comes out I'll upgrade it and keep doing so unless issues arise.  The Android build process can be run concurrently.  A good number is one job per logical core (this may be different from the amount of physical cores.  My machine has an Intel Core i7, which has 4 physical cores but 8 logical ones).  The command for running from the [NativeBuild directory](NativeBuild/) of this repo is `ndk-build -j 8 -C jni` where the number after `-j` is the number previously discussed.  Copy the output of the libs folder to the prebuilt folder (i.e. the four folders with architecture names).

Note:  On Windows there is a nasty issue regarding file path lengths when building for Android.  If you have even a moderately long path to the Android project, the ndk-build will fail because the maximum file path length on Windows will be exceeded (260).  To workaround this, you can mount the project folder as a drive letter and build from there, or simply build from somewhere without a long path (i.e. C:/Project).

