import os
import sys


def can_build(env, platform):
    if not env["arch"] or env.msvc:
        return True

    if env["platform"] == "windows":
        pkgconf_error = os.system("pkg-config --version > NUL")
    else:
        pkgconf_error = os.system("pkg-config --version > /dev/null")

    if pkgconf_error:
        print("Error: pkg-config not found. Aborting.")
        return False

    return True


def configure(env):
    if not env["arch"]:
        return

    if env["platform"] == "linuxbsd":
        if os.system("pkg-config --exists libseccomp"):
            print("Error: Seccomp library not found. Aborting.")
            sys.exit(255)
        else:
            env.ParseConfig("pkg-config libseccomp --cflags --libs")
            print("Linking Seccomp")

    if env["platform"] == "windows" and env.get("the_gates_sandbox") and env.msvc:
        # Build flags to link the renderer against Chromium's cef_sandbox.
        # Requires a local Chromium/CEF build at C:/code/chromium_git (see
        # https://github.com/thegatesbrowser/chromium). Paths are intentionally
        # absolute; this is a developer-host build, not a CI build.

        env.Replace(CC="clang-cl")
        env.Replace(CXX="clang-cl")

        # Required instruction sets for libwebp under clang-cl.
        env.Append(CCFLAGS=["-mssse3", "-msse4.1"])

        # Disable assembly optimizations in R128 (incompatible with clang-cl link).
        env.Prepend(CPPDEFINES=["R128_STDC_ONLY"])

        env.Prepend(CPPPATH=[
            "C:/code/chromium_git/chromium/src/",
            "C:/code/chromium_git/chromium/src/out/Release_GN_x64_sandbox/gen/",
            "C:/code/chromium_git/chromium/src/buildtools/third_party/libc++/",
            "C:/code/chromium_git/chromium/src/third_party/perfetto/include/",
            "C:/code/chromium_git/chromium/src/out/Release_GN_x64_sandbox/gen/third_party/perfetto/build_config/",
            "C:/code/chromium_git/chromium/src/out/Release_GN_x64_sandbox/gen/third_party/perfetto/",
            "C:/code/chromium_git/chromium/src/base/allocator/partition_allocator/src/",
            "C:/code/chromium_git/chromium/src/out/Release_GN_x64_sandbox/gen/base/allocator/partition_allocator/src/",
            "C:/code/chromium_git/chromium/src/third_party/abseil-cpp/",
            "C:/code/chromium_git/chromium/src/third_party/boringssl/src/include/",
            "C:/code/chromium_git/chromium/src/third_party/protobuf/src/",
        ])

        env.Append(LIBPATH=[
            "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.43.34808/lib/x64",
            "C:/Program Files (x86)/Windows Kits/10/Lib/10.0.26100.0/um/x64",
            "C:/code/chromium_git/chromium/src/cef/binary_distrib/cef_binary_137.0.10+g7e14fe1+chromium-137.0.7151.69_windows64_sandbox/Release",
        ])

        env.Append(LINKFLAGS=[
            "cef_sandbox.lib",
            "kernel32.lib",
            "user32.lib",
            "gdi32.lib",
            "advapi32.lib",
            "shell32.lib",
            "ole32.lib",
            "oleaut32.lib",
            "uuid.lib",
            "winmm.lib",
            "shlwapi.lib",
            "propsys.lib",
            "powrprof.lib",
            "shcore.lib",
            "ntdll.lib",
            "setupapi.lib",
            "cfgmgr32.lib",
            "version.lib",
            "ws2_32.lib",
            "runtimeobject.lib",
            "dbghelp.lib",
            "userenv.lib",
            "delayimp.lib",
            "wbemuuid.lib",
            "mincore.lib",
        ])
