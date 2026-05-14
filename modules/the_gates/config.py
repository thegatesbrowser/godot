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

    if env["platform"] == "windows" and env.get("the_gates_sandbox"):
        # Vendored Chromium sandbox headers + a prebuilt static lib produced
        # from the same source. The prebuilt lib lives in our Chromium fork
        # checkout at C:/code; the headers are vendored in-tree under
        # godot/thirdparty/chromium-sandbox/.
        #
        # The lib must have been built with SANDBOX_EXPORTS=1 (see the
        # 08_add_back_SANDBOX_EXPORTS-style patch we apply in our chromium
        # fork); we also define it in the consumer build so SANDBOX_INTERCEPT
        # expands to the dllexport variant in any sandbox header we pull in.

        env.Replace(CC="clang-cl")
        env.Replace(CXX="clang-cl")

        # Required instruction sets for libwebp under clang-cl.
        env.Append(CCFLAGS=["-mssse3", "-msse4.1"])

        # Disable assembly optimizations in R128 (incompatible with clang-cl link).
        env.Prepend(CPPDEFINES=["R128_STDC_ONLY"])

        # NOTE: SANDBOX_EXPORTS=1 is only set on the sandbox module's TUs
        # (see modules/the_gates/sandbox/SCsub) — not engine-wide, otherwise
        # every TU triggers a rebuild on toggle and we touch unrelated code.

        env.Prepend(CPPPATH=[
            "thirdparty/chromium-sandbox",
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
