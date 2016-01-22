# NOTE: armeabi and mips has libatomic issue
APP_ABI := armeabi-v7a x86 arm64-v8a x86_64 
#APP_ABI := armeabi-v7a x86 mips armeabi
#APP_ABI := armeabi-v7a x86 mips
#APP_ABI := armeabi-v7a x86
#APP_ABI := armeabi-v7a
#APP_ABI := armeabi
#APP_ABI := mips
#APP_ABI := x86
#APP_ABI := all
APP_PLATFORM := android-19
# it seems no backward compatibility. 
#APP_PLATFORM := android-21
#NDK_TOOLCHAIN_VERSION := 4.9
NDK_TOOLCHAIN_VERSION := clang
APP_STL := gnustl_static
#APP_OPTIM := debug
