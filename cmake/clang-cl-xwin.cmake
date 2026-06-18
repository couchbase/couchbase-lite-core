# Toolchain: build LiteCore on Windows with clang/LLVM + an xwin SDK splat.
# No Visual Studio required. Point at an xwin splat dir (default: sibling xwin-sdk).
#   cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=.clang-tools/clang-cl-xwin.cmake
# Override the splat location with -DXWIN_SDK=<path>.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

if(NOT DEFINED XWIN_SDK)
    set(XWIN_SDK "${CMAKE_CURRENT_LIST_DIR}/xwin-sdk")
endif()

# try_compile() runs an isolated sub-project with a fresh cache, so a
# command-line -DXWIN_SDK is NOT visible there and the default above would
# wrongly kick in. Forward it explicitly to every try_compile.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES XWIN_SDK LLVM_SUFFIX)

message(STATUS "Checking XWIN_SDK at ${XWIN_SDK}")
get_filename_component(XWIN_SDK "${XWIN_SDK}" ABSOLUTE)
if(NOT EXISTS "${XWIN_SDK}/crt/include")
    message(FATAL_ERROR "XWIN_SDK does not look like an xwin splat...")
endif()

# LLVM tools (must be on PATH).
set(CMAKE_C_COMPILER   clang-cl${LLVM_SUFFIX})
set(CMAKE_CXX_COMPILER clang-cl${LLVM_SUFFIX})
set(CMAKE_RC_COMPILER  llvm-rc${LLVM_SUFFIX})
set(CMAKE_MT           llvm-mt${LLVM_SUFFIX})
set(CMAKE_AR           llvm-lib${LLVM_SUFFIX})
# Link with lld-link. Do NOT use CMAKE_LINKER_TYPE LLD: it forces the bare
# unsuffixed name "lld-link" into the link rule, ignoring both LLVM_SUFFIX and
# CMAKE_LINKER. On a suffixed-LLVM box (lld-link-19, no unsuffixed alias) that
# exec fails with "no such file or directory". Pin CMAKE_LINKER to the real
# binary instead; the MSVC direct-link rule uses it verbatim.
find_program(CMAKE_LINKER NAMES "lld-link${LLVM_SUFFIX}" lld-link REQUIRED)

# The xwin splat ships only the RELEASE CRT (msvcrt.lib); the debug CRT
# (msvcrtd.lib, vcruntimed.lib, ...) is dev-only and not in the redistributable
# packages xwin downloads. CMake's compiler-id/ABI probe runs as a try_compile
# BEFORE the project's build-type flags apply and defaults to a Debug-style
# link, so it looks for msvcrtd.lib and fails on a clean (no-VS) machine.
# Force every try_compile and all targets to the release runtime instead.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)
set(CMAKE_POLICY_DEFAULT_CMP0091 NEW)             # runtime lib via abstraction
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)  # /MD always (never /MDd)

set(_xwin_target "--target=x86_64-pc-windows-msvc")

# /X drops %INCLUDE%; all headers come from the xwin splat (-imsvc = treat as system).
set(_xwin_incs
    "-imsvc${XWIN_SDK}/crt/include"
    "-imsvc${XWIN_SDK}/sdk/include/ucrt"
    "-imsvc${XWIN_SDK}/sdk/include/um"
    "-imsvc${XWIN_SDK}/sdk/include/shared"
    "-imsvc${XWIN_SDK}/sdk/include/winrt"
    "-imsvc${XWIN_SDK}/sdk/include/cppwinrt"
)
string(JOIN " " _xwin_incs_str ${_xwin_incs})
set(_common "${_xwin_target} /X ${_xwin_incs_str}")
set(CMAKE_C_FLAGS_INIT   "${_common}")
set(CMAKE_CXX_FLAGS_INIT "${_common}")

# Library search dirs from the splat.
set(_xwin_libs
    "/libpath:${XWIN_SDK}/crt/lib/x86_64"
    "/libpath:${XWIN_SDK}/sdk/lib/ucrt/x86_64"
    "/libpath:${XWIN_SDK}/sdk/lib/um/x86_64"
)
string(JOIN " " _xwin_libs_str ${_xwin_libs})
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_xwin_libs_str}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_xwin_libs_str}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${_xwin_libs_str}")

# Only search the splat for libs/headers, not the host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
