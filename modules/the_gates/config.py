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

    if env["platform"] == "windows" and env.get("tg_sandbox"):
        # The Windows sandbox is built directly from the vendored Chromium
        # sources under godot/thirdparty/chromium-sandbox/. No external
        # checkout required: every header, every .cc, and every generated
        # buildflag header lives in tree.
        #
        # The SANDBOX_EXPORTS patch (Firefox-style cross-exe broker/target)
        # is applied to the vendored sandbox/win/src/ files. Module-local
        # CPPDEFINES live in modules/the_gates/sandbox/SCsub so toggling
        # SANDBOX_EXPORTS doesn't invalidate engine-wide TU caches.

        env.Replace(CC="clang-cl")
        env.Replace(CXX="clang-cl")

        # Required instruction sets for libwebp under clang-cl.
        env.Append(CCFLAGS=["-mssse3", "-msse4.1"])

        # Disable assembly optimizations in R128 (incompatible with clang-cl link).
        env.Prepend(CPPDEFINES=["R128_STDC_ONLY"])

        # Windows system libs the Chromium sandbox + base/ subset needs at link
        # time. sandbox/win/BUILD.gn declares `libs = [ "ntdll.lib",
        # "userenv.lib" ]`. base/win/ adds the rest (RuntimeObject for WinRT
        # strings, SetupAPI for device enum, propsys for VARIANT, etc.).
        env.Append(LINKFLAGS=[
            "ntdll.lib",
            "userenv.lib",
            "powrprof.lib",
            "version.lib",
            "runtimeobject.lib",
            "setupapi.lib",
            "cfgmgr32.lib",
            "propsys.lib",
            "shlwapi.lib",
            "shcore.lib",
            "dbghelp.lib",
            "winmm.lib",
            "wbemuuid.lib",
            "mincore.lib",
            "delayimp.lib",
        ])
