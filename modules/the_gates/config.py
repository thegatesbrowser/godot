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

    if env.msvc:
        # TODO: fix not msvc windows build

        # 1. Build libzmq https://www.youtube.com/watch?v=OiGf9T_TPa8
        # 2. Fix linking mismatch https://stackoverflow.com/questions/28887001/lnk2038-mismatch-detected-for-runtimelibrary-value-mt-staticrelease-doesn
        # 3. Place inside C:/Program Files (x86)/ZeroMQ/
        env.Prepend(CPPDEFINES=["ZMQ_STATIC"])
        env.Prepend(CPPPATH=["C:/Program Files (x86)/ZeroMQ/include"])
        env.Append(LIBPATH=["C:/Program Files (x86)/ZeroMQ/lib"])
        env.Append(LINKFLAGS=["libzmq-v143-mt-s-4_3_5.lib"])
        print("Linking ZeroMQ statically")


        # CHROMIUM SANDBOXING

        env.Replace(CC = "clang-cl")
        env.Replace(CXX = "clang-cl")

        # Enable required instruction sets for libwebp
        # TODO: remove this as it was discussed here https://github.com/godotengine/godot/pull/36580/files
        env.Append(CCFLAGS=['-mssse3'])
        env.Append(CCFLAGS=["-msse4.1"])

        # Disable chromium debugging code
        env.Prepend(CPPDEFINES=["NDEBUG"])

        # Disable assembly optimizations in R128 library and use standard C implementation
        env.Prepend(CPPDEFINES=['R128_STDC_ONLY'])

        # Add chromium include paths
        env.Prepend(CPPPATH=[
            'C:/src/chromium/src/',
            'C:/src/chromium/src/out/sandbox_build/gen/',
            'C:/src/chromium/src/buildtools/third_party/libc++/',
            'C:/src/chromium/src/third_party/perfetto/include/',
            'C:/src/chromium/src/out/sandbox_build/gen/third_party/perfetto/build_config/',
            'C:/src/chromium/src/out/sandbox_build/gen/third_party/perfetto/',
            'C:/src/chromium/src/base/allocator/partition_allocator/src/',
            'C:/src/chromium/src/out/sandbox_build/gen/base/allocator/partition_allocator/src/',
            'C:/src/chromium/src/third_party/abseil-cpp/',
            'C:/src/chromium/src/third_party/boringssl/src/include/',
            'C:/src/chromium/src/third_party/protobuf/src/'
        ])

        # Add chromium library paths
        env.Append(LIBPATH=[
            "C:/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.43.34808/lib/x64",
            "C:/Program Files (x86)/Windows Kits/10/Lib/10.0.26100.0/um/x64",
            "C:/Users/Nordup/Downloads/cef_binary_136.1.4+g89c0a8c+chromium-136.0.7103.93_windows64_minimal/cef_binary_136.1.4+g89c0a8c+chromium-136.0.7103.93_windows64_minimal/Release"
        ])

        env.Append(LINKFLAGS=[
            # Chromium Embedded Framework (CEF). Download from https://cef-builds.spotifycdn.com/index.html#windows64
            "cef_sandbox.lib",
            # Core Windows libraries
            "kernel32.lib",
            "user32.lib",
            "gdi32.lib",
            "advapi32.lib",
            "shell32.lib",
            "ole32.lib",
            "oleaut32.lib",
            "uuid.lib",
            # Additional Windows libraries
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

    elif env["platform"] == "linuxbsd":
        if os.system("pkg-config --exists libzmq"):
            print("Error: ZeroMQ librarie not found. Aborting.")
            sys.exit(255)
        else:
            env.ParseConfig("pkg-config libzmq --cflags --libs --static")
            print("Linking ZeroMQ statically")

        if os.system("pkg-config --exists libseccomp"):
            print("Error: Seccomp librarie not found. Aborting.")
            sys.exit(255)
        else:
            env.ParseConfig("pkg-config libseccomp --cflags --libs")
            print("Linking Seccomp")

    else:
        # MacOS

        # 1. Build zeromq with cmake and Xcode both arm64 (macOS 11.0+) and x86_64 (macOS 10.13+) https://github.com/zeromq/libzmq
        # 2. Link them with lipo into universal library
        # 3. Replace libzmq.dylib that pkg-config returns
        if os.system("pkg-config --exists libzmq"):
            print("Error: ZeroMQ librarie not found. Aborting.")
            sys.exit(255)
        else:
            # Linked dynamically
            env.ParseConfig("pkg-config libzmq --cflags --libs")
            print("Linking ZeroMQ dynamically")
