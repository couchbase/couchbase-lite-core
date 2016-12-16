# NOTES
- Building the native library with `clang` causes following compilation error. For this use `GCC`. See app/build.gradle



```
arm-linux-androideabi/bin/ld: error: /Users/hideki/github/couchbase-lite-core/Android/CppTests/app/src/main/cpp/../../../../distribution/litecore/lib/armeabi-v7a/libcrypto.a(sha1-armv4-large.o): requires unsupported dynamic reloc R_ARM_REL32; recompile with -fPIC
arm-linux-androideabi/bin/ld: warning: shared library text segment is not shareable
```